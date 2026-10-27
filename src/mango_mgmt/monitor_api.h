// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - Public monitor API
 *
 * Implementation-agnostic monitor interface exported to other modules
 * (e.g. mango_core).  Callers identify a device by its management hardware
 * address and never see the underlying backend; the management
 * driver resolves the address to a device and dispatches to the right
 * backend internally.
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_MONITOR_API_H_
#define _MANGO_MGMT_MONITOR_API_H_

#include <linux/bits.h>
#include <linux/notifier.h>
#include <linux/types.h>

/* Defined in mango_mgmt/smi/smi_api.h; only ever passed by pointer here. */
struct smi_qsfp_status;

enum mango_mgmt_monitor_card_info {
	MANGO_MGMT_MONITOR_CARD_INFO_SERIAL = 0,
	MANGO_MGMT_MONITOR_CARD_INFO_MACADDR,
};

/* Capacity mango_mgmt_monitor_get_info() takes per item, not the answer's
 * length: a shorter buffer is rejected, the rest of it is zeroed. */
#define MANGO_MGMT_MONITOR_SERIAL_LEN 32
#define MANGO_MGMT_MONITOR_MACADDR_LEN 8

/* Sensors a caller can ask for, independent of which monitor answers. */
enum mango_mgmt_monitor_sensor {
	MANGO_MGMT_MONITOR_SENSOR_TOTAL_POWER = 0,
	MANGO_MGMT_MONITOR_SENSOR_TEMP_BOARD,
	MANGO_MGMT_MONITOR_SENSOR_TEMP_FPGA,
	MANGO_MGMT_MONITOR_SENSOR_TEMP_ETHERNET,
	MANGO_MGMT_MONITOR_SENSOR_MAX
};

/* Threshold levels, in ascending order of value. */
enum mango_mgmt_sensor_threshold {
	MANGO_MGMT_SENSOR_THRESHOLD_LOWER_FATAL = 0,
	MANGO_MGMT_SENSOR_THRESHOLD_LOWER_CRIT,
	MANGO_MGMT_SENSOR_THRESHOLD_LOWER_WARN,
	MANGO_MGMT_SENSOR_THRESHOLD_UPPER_WARN,
	MANGO_MGMT_SENSOR_THRESHOLD_UPPER_CRIT,
	MANGO_MGMT_SENSOR_THRESHOLD_UPPER_FATAL,
	MANGO_MGMT_SENSOR_THRESHOLD_MAX
};

/* Which scalar fields the answering monitor actually reported. */
#define MANGO_MGMT_SENSOR_HAS_VALUE BIT(0)
#define MANGO_MGMT_SENSOR_HAS_MIN BIT(1)
#define MANGO_MGMT_SENSOR_HAS_MAX BIT(2)
#define MANGO_MGMT_SENSOR_HAS_AVG BIT(3)

/**
 * struct mango_mgmt_sensor_reading - One sensor, as a monitor reports it.
 * @value: Current reading.
 * @min: Lowest value the card has observed.
 * @max: Highest value the card has observed.
 * @avg: Average maintained by the card.
 * @fields_valid: Bitmask of MANGO_MGMT_SENSOR_HAS_*. Only the fields named
 *          here hold a value; a monitor that cannot report one leaves it out
 *          rather than reporting zero, which a caller cannot tell from a real
 *          zero.
 * @thresholds: Values indexed by enum mango_mgmt_sensor_threshold. Only
 *          entries whose bit is set in @thresholds_valid hold a value.
 * @thresholds_valid: Bitmask of BIT(enum mango_mgmt_sensor_threshold).
 * @status: Backend-specific sensor status byte, zero when there is none.
 *
 * Units follow the sensor: microwatts for power, degrees Celsius for the
 * temperatures. Values are signed because a temperature and a lower threshold
 * can both fall below zero.
 */
struct mango_mgmt_sensor_reading {
	s64 value;
	s64 min;
	s64 max;
	s64 avg;
	u32 fields_valid;
	s64 thresholds[MANGO_MGMT_SENSOR_THRESHOLD_MAX];
	u32 thresholds_valid;
	u8 status;
};

/**
 * mango_mgmt_monitor_get_info() - Read card information by HW address.
 * @hw_addr: Management HW address (e.g. "pcie:0000:64:00.0").
 * @type: Type of card info to read.
 * @buf: Caller-supplied buffer.
 * @len: Length of @buf; at least the item's MANGO_MGMT_MONITOR_*_LEN.
 *
 * Return: 0 on success, negative error code on failure.
 */
int mango_mgmt_monitor_get_info(const char *hw_addr,
				enum mango_mgmt_monitor_card_info type, u8 *buf,
				size_t len);

/**
 * mango_mgmt_monitor_get_sensor() - Read a sensor by HW address.
 * @hw_addr: Management HW address (e.g. "pcie:0000:64:00.0").
 * @sensor: Sensor to read.
 * @out: Reading to fill (output).
 *
 * Reports whatever the platform monitor has for that sensor: the current
 * value, the min, max and average, the thresholds, and a status byte. Fields
 * the monitor cannot report are left out of the validity masks instead of
 * being reported as zero.
 *
 * This is the only sensor path out of the management driver; it names nothing
 * backend-specific, so consumers need not read hwmon directly.
 *
 * Return: 0 on success, -EOPNOTSUPP when the platform monitor has no sensor
 * support, -ENODATA when it lacks that particular sensor, other negative error
 * code on failure.
 */
int mango_mgmt_monitor_get_sensor(const char *hw_addr,
				  enum mango_mgmt_monitor_sensor sensor,
				  struct mango_mgmt_sensor_reading *out);

/**
 * mango_mgmt_monitor_get_qsfp_status() - Read decoded QSFP link status.
 * @hw_addr: Management HW address (e.g. "pcie:0000:64:00.0").
 * @link: QSFP link index (0-based).
 * @status: Decoded link status (output).
 * @num_links: Total number of QSFP links, may be NULL (output).
 *
 * The definition of struct smi_qsfp_status lives in mango_mgmt/smi/smi_api.h
 * (the same contract as struct smi_alarm_event for alarm callbacks).
 *
 * Return: 0 on success, -ENODATA when the firmware provides no QSFP repo
 * or it is empty (e.g. old firmware), -EINVAL when @link is out of range,
 * -EOPNOTSUPP when the platform monitor lacks QSFP support,
 * other negative error code on failure.
 */
int mango_mgmt_monitor_get_qsfp_status(const char *hw_addr, u8 link,
				       struct smi_qsfp_status *status,
				       u8 *num_links);

/**
 * mango_mgmt_monitor_register_alarm_notifier() - Register an alarm notifier.
 * @hw_addr: Management HW address (selects the backend to register with).
 * @nb: Notifier block to register.
 *
 * Return: 0 on success, negative error code on failure.
 */
int mango_mgmt_monitor_register_alarm_notifier(const char *hw_addr,
					       struct notifier_block *nb);

/**
 * mango_mgmt_monitor_unregister_alarm_notifier() - Unregister an alarm notifier.
 * @hw_addr: Management HW address (selects the backend to unregister from).
 * @nb: Notifier block to unregister.
 *
 * Return: 0 on success, negative error code on failure.
 */
int mango_mgmt_monitor_unregister_alarm_notifier(const char *hw_addr,
						 struct notifier_block *nb);

#endif /* _MANGO_MGMT_MONITOR_API_H_ */
