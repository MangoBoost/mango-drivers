// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost SMI API - types, module init and address-keyed reads
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_SMI_API_H_
#define _MANGO_MGMT_SMI_API_H_

#include <linux/notifier.h>
#include <linux/types.h>

/* SMI Alarm Type is future extensible not limited to thermal event */
enum smi_alarm_type {
	SMI_ALARM_FPGA_OVER_TEMP,
	SMI_ALARM_BOARD_OVER_TEMP, /* Not supported. Severity threshold not defined */
	SMI_ALARM_ETH_OVER_TEMP,
	SMI_ALARM_MAX
};

enum smi_alarm_state { SMI_ALARM_STATE_RAISED, SMI_ALARM_STATE_CLEARED };

enum smi_alarm_severity {
	SMI_ALARM_SEVERITY_WARN,
	SMI_ALARM_SEVERITY_CRIT,
	SMI_ALARM_SEVERITY_FATAL,
	SMI_ALARM_SEVERITY_MAX
};

enum smi_bus_type {
	SMI_BUS_TYPE_PCI, /* PCIe */
	SMI_BUS_TYPE_PLT, /* Platform */
};

struct smi_hw_address {
	enum smi_bus_type bus_type;
	union {
		struct platform_device *plt_dev;
		struct pci_dev *pci_dev;
	};
};

struct smi_alarm_event {
	struct smi_hw_address addr;
	enum smi_alarm_type type;
	enum smi_alarm_state action;
	enum smi_alarm_severity severity;
	/* Degrees Celsius, whatever exponent the card's record carries. */
	long current_value;
	long threshold;
};

/*
 * SMI_SENSOR_TEMP returns the first temperature record (legacy aggregate).
 * The TEMP_* variants select a specific sensor by its hwmon label.
 */
enum smi_sensor_type {
	SMI_SENSOR_POWER = 0,
	SMI_SENSOR_TEMP,
	SMI_SENSOR_TEMP_BOARD,
	SMI_SENSOR_TEMP_FPGA,
	SMI_SENSOR_TEMP_ETHERNET,
	SMI_SENSOR_MAX
};

/*
 * Threshold levels a sensor record may carry, in ascending order of value.
 * A record advertises which of these it actually holds, and most hold none.
 */
enum smi_sensor_threshold {
	SMI_SENSOR_THRESHOLD_LOWER_FATAL = 0,
	SMI_SENSOR_THRESHOLD_LOWER_CRIT,
	SMI_SENSOR_THRESHOLD_LOWER_WARN,
	SMI_SENSOR_THRESHOLD_UPPER_WARN,
	SMI_SENSOR_THRESHOLD_UPPER_CRIT,
	SMI_SENSOR_THRESHOLD_UPPER_FATAL,
	SMI_SENSOR_THRESHOLD_MAX
};

/* Which scalar fields a backend actually reported. */
#define SMI_SENSOR_HAS_VALUE BIT(0)
#define SMI_SENSOR_HAS_MIN BIT(1)
#define SMI_SENSOR_HAS_MAX BIT(2)
#define SMI_SENSOR_HAS_AVG BIT(3)

/**
 * struct smi_sensor_reading - Everything a backend can report for one sensor.
 * @value: Current reading.
 * @min: Lowest value the card has observed.
 * @max: Highest value the card has observed.
 * @avg: Average maintained by the card.
 * @fields_valid: Bitmask of SMI_SENSOR_HAS_*. Only the fields named here hold
 *          a value; a backend that cannot report one leaves it out rather than
 *          reporting zero, which a caller cannot tell from a real zero.
 * @thresholds: Threshold values indexed by enum smi_sensor_threshold. Only
 *          entries whose bit is set in @thresholds_valid hold a value.
 * @thresholds_valid: Bitmask of BIT(enum smi_sensor_threshold).
 * @status: Raw sensor status byte, zero when the backend has none.
 *
 * Values are signed because a temperature reading and a lower threshold can
 * both fall below zero, and the underlying record decodes to a signed value;
 * an unsigned field would turn a cold sensor into an enormous positive number.
 */
struct smi_sensor_reading {
	s64 value;
	s64 min;
	s64 max;
	s64 avg;
	u32 fields_valid;
	s64 thresholds[SMI_SENSOR_THRESHOLD_MAX];
	u32 thresholds_valid;
	u8 status;
};

enum smi_board_info {
	SMI_BOARD_INFO_MAC_ADDR,
	SMI_BOARD_INFO_SERIAL_NUMBER,
	SMI_BOARD_INFO_MAX
};

/**
 * struct smi_qsfp_status - Decoded QSFP link status record.
 * @speed_gbps: Negotiated link speed in Gbps (0 if link down).
 * @link_up: True when the record reports the link up, as smi_qsfp_link_up()
 *           in the contract reads it.
 * @cable_present: True when a cable is detected in the cage.
 * @cage: Physical cage index (0-based).
 * @raw: Raw packed value as published by the SMC, for diagnostics.
 *       A reserved encoding (link status >= 2, bits [7:5]) leaves every
 *       decoded field 0; @raw preserves it.
 */
struct smi_qsfp_status {
	u16 speed_gbps;
	bool link_up;
	bool cable_present;
	u8 cage;
	u32 raw;
};

/**
 * mango_mgmt_smi_init() - Bring up the SMI library: FAL, class and cdev,
 *                         RPMsg endpoint driver. Before any probe attaches SMI.
 *
 * Return: 0 on success, negative error code on failure.
 */
int mango_mgmt_smi_init(void);

/**
 * mango_mgmt_smi_exit() - Tear the SMI library down.
 */
void mango_mgmt_smi_exit(void);

/**
 * mango_mgmt_smi_get_board_info() - Retrieve board information by HW address.
 * @hw_addr: HW address, "pcie:DOMAIN:BUS:SLOT.FUNC" or "mmio:...".
 * @type: Type of board info to retrieve (MAC address, Serial Number, etc.)
 * @buf: User-supplied buffer to store the information.
 * @len: Length of the buffer.
 *
 * Return: the number of bytes written -- the length the card stated for the
 * field -- or a negative error code.
 */
int mango_mgmt_smi_get_board_info(const char *hw_addr, enum smi_board_info type,
				  u8 *buf, size_t len);

/**
 * mango_mgmt_smi_get_fw_version() - Management firmware version by HW address.
 * @hw_addr: HW address, "pcie:DOMAIN:BUS:SLOT.FUNC" or "mmio:...".
 * @buf: Caller-supplied buffer, receives "MAJOR.MINOR.PATCH-YYMMDD".
 * @len: Length of @buf. 16 bytes hold every value the format can produce.
 *
 * The version the firmware stated at probe, which is also what the SMI
 * function's smc_version attribute reports. No frame is sent.
 *
 * Return: 0 on success, negative error code on failure.
 */
int mango_mgmt_smi_get_fw_version(const char *hw_addr, char *buf, size_t len);

/**
 * mango_mgmt_smi_register_alarm_notifier() - Register an alarm notifier.
 * @nb: The notifier block to register.
 *
 * The caller must initialize the notifier_block with a callback function
 * matching the following signature:
 *
 * int (*notifier_call)(struct notifier_block *nb, unsigned long action, void *data);
 *
 * Parameters for the callback:
 *   @nb:     Pointer to this notifier block.
 *   @action: The alarm type (enum smi_alarm_type).
 *   @data:   Pointer to the alarm event data (struct smi_alarm_event *).
 *
 * Return: 0 on success, negative error code on failure.
 */
int mango_mgmt_smi_register_alarm_notifier(struct notifier_block *nb);

/**
 * mango_mgmt_smi_unregister_alarm_notifier() - Unregister an alarm notifier.
 * @nb: The notifier block to unregister.
 *
 * Return: 0 on success, negative error code on failure.
 */
int mango_mgmt_smi_unregister_alarm_notifier(struct notifier_block *nb);

/**
 * mango_mgmt_smi_get_sensor_info() - Retrieve a sensor record by HW address.
 * @hw_addr: HW address, "pcie:DOMAIN:BUS:SLOT.FUNC" or "mmio:...".
 * @type: Type of sensor to retrieve (power, temperature, etc.)
 * @out: Reading to fill (output).
 *
 * Reports every field the backend has: the current value, the min, max and
 * average, the thresholds, and the sensor status. Fields the backend cannot
 * report are left out of @fields_valid and @thresholds_valid.
 *
 * Units follow the sensor type: microwatts for SMI_SENSOR_POWER and degrees
 * Celsius for the temperatures.
 *
 * Return: 0 on success, negative error code on failure.
 */
int mango_mgmt_smi_get_sensor_info(const char *hw_addr,
				   enum smi_sensor_type type,
				   struct smi_sensor_reading *out);

/**
 * mango_mgmt_smi_get_qsfp_status() - Read a QSFP link record by HW address.
 * @hw_addr: HW address, "pcie:DOMAIN:BUS:SLOT.FUNC" or "mmio:...".
 * @link: Cage index to read.
 * @status: Record to fill (output).
 * @num_links: Receives how many cages the card has.
 *
 * Return: 0 on success, negative error code on failure.
 */
int mango_mgmt_smi_get_qsfp_status(const char *hw_addr, u8 link,
				   struct smi_qsfp_status *status,
				   u8 *num_links);

#endif /* _MANGO_MGMT_SMI_API_H_ */
