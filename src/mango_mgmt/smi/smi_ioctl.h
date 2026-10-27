/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * smi_ioctl.h - the SMI ioctl contract, shared with userspace
 *
 * Copyright (c) 2023 Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

/*
 * The command numbers and payloads userspace passes: a leaf any tool compiles
 * (fixed-width types, the ioctl macros, the QSFP cage contract). The NUMBERS
 * are the contract; a rename is safe, a renumber is not.
 */

#ifndef SMI_IOCTL_H
#define SMI_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
#include <sys/ioctl.h>
#endif

/* QsfpRw's op and power byte, and the block qsfp<N>_raw passes through */
#include "contract/smi/smi_abi_qsfp.h"

/* IOCTL data. Shared with userspace code. */

#define SMI_IOC_FPT_UPDATE_MAGIC (0xAAAAAAAA)
#define SMI_IOC_SENSOR_STATUS_LEN (40)
#define SMI_IOC_DATA_PAYLOAD_MAX_SZ                                            \
	(152 * 1024 * 1024) /* Max 152MB Payload (PDI)*/
/* The most one eeprom ioctl may express, which is what bounds the buffer the
 * driver allocates for it. It is NOT the bound a transfer is held to: the card
 * advertises its part's actual extent during negotiation and the driver enforces
 * that, so a request past the end is refused without spending a round trip.
 * Read the extent from the device's eeprom_size attribute. */
#define SMI_IOC_EEPROM_MAX_XFER (1024)
#define SMI_IOC_MODULE_MAX_XFER (128) /* Max 128 Bytes per IOCTL */
/* One page of a module's map, which is what one request may name. */
#define SMI_IOC_QSFP_MAX_XFER SMI_QSFP_PAGE_BYTES
/* One shell line in, its rendered answer out. The line bound is the card's: its
 * shell reads into 256 bytes and spends the last on the terminator, so 255 is
 * the longest line it will run -- restated here rather than included, because
 * this header is a leaf every tool compiles. The answer bound is this side's,
 * and a caller that wants more than it asked for is told how much more. */
#define SMI_IOC_SHELL_MAX_LINE (255)
#define SMI_IOC_SHELL_MAX_OUT (64 * 1024)
/* Longest beacon or force one LED request may ask for, in seconds. */
#define SMI_IOC_LED_MAX_TIMEOUT_S (600)

/**
 * struct smi_ioc_data_payload - payload struct for dynamically sized ioctl data
 * @size: Size of data buffer.
 * @addr: Location of data buffer in userspace memory.
 * @boot_device: Target boot device (for fpt, download).
 * @partition: Partition number to flash (also used for boot select).
 * @src_device: Source device (for copy operation).
 * @src_part: Source partition (for copy operation).
 * @dest_device: Destination device (for copy operation).
 * @dest_part: Destination partition (for copy operation).
 * @cap_override: Ignored; kept for ABI. Privilege is CAP_SYS_ADMIN.
 * @efd: File descriptor for event notifications (used for progress reporting when
 *     performing long running operations like PDI downloads) - optional
 *
 * Note that addr can be an address to any arbitrary data type,
 * depending on the context. This struct is reused for the boot select
 * command, with only the `partition` field populated.
 *
 * When performing a PDI download operation, if `partition` is equal to
 * `FPT_UPDATE_MAGIC`, the FPT will be overwritten with the provided
 * PDI image!
 */

struct smi_ioc_data_payload {
	uint32_t size;
	unsigned long addr;
	uint8_t boot_device;
	uint32_t partition;
	uint32_t src_device;
	uint32_t src_part;
	uint32_t dest_device;
	uint32_t dest_part;
	bool cap_override;
	int efd;
};

/**
 * enum smi_ioc_sensor_type - list of supported sensor types
 * @IOC_SENSOR_TYPE_TEMP: Temperature sensor ("temp" in hwmon)
 * @IOC_SENSOR_TYPE_VOLTAGE: Voltage sensor ("in" in hwmon)
 * @IOC_SENSOR_TYPE_CURRENT: Current sensor ("curr" in hwmon)
 * @IOC_SENSOR_TYPE_POWER: Power sensor ("power" in hwmon)
 * @IOC_SENSOR_TYPE_QSFP: QSFP link record, not a hwmon sensor; its fields are
 *                        smi_abi_qsfp.h's SMI_QSFP_LINK_*
 *
 * This enum is to be use in conjunction with `struct smi_ioc_sensor_value`.
 */
enum smi_ioc_sensor_type {
	IOC_SENSOR_TYPE_TEMP,
	IOC_SENSOR_TYPE_VOLTAGE,
	IOC_SENSOR_TYPE_CURRENT,
	IOC_SENSOR_TYPE_POWER,
	IOC_SENSOR_TYPE_QSFP,
};

/**
 * struct smi_ioc_sensor_value - single sensor value and status
 * @val: Sensor value. Populated by the driver.
 * @status: Sensor status. Populated by the driver.
 * @fresh: Whether or not the value/status were read from the cache. Populated by the driver.
 * @hwmon_channel: The hwmon sensor channel number. This is passed in from userspace.
 * @sensor_type: Sensor type. This is passed in from userspace.
 *
 * This API is provided as an alternative to hwmon due to a possible race condition
 * where the user would have to read two separate hwmon files to fetch both the
 * sensor value and its status. Currently, this is only supported for the
 * instantaneous value of a sensor.
 */
struct smi_ioc_sensor_value {
	long val;
	char status[SMI_IOC_SENSOR_STATUS_LEN];
	bool fresh;
	int hwmon_channel;
	int sensor_type;
};

/**
 * struct smi_ioc_fpt_hdr_value - the fpt header
 * @boot_device: Target boot device.
 * @version: The version. Populated by the driver.
 * @hdr_size: The size of the header in bytes. Populated by the driver.
 * @entry_size: The entry size in bytes. Populated by the driver.
 * @num_entries: The number of partitions. Populated by the driver.
 *
 * This struct is to be use in conjunction with `struct smi_ioc_fpt_hdr_value` to get the
 * individual partition information.
 */
struct smi_ioc_fpt_hdr_value {
	uint8_t boot_device;
	uint8_t version;
	uint8_t hdr_size;
	uint8_t entry_size;
	uint8_t num_entries;
};

/**
 * struct smi_ioc_fpt_partition_value - the individual partition information.
 * @boot_device: Target boot device.
 * @partition: This is passed in from userspace.
 * @type: The partition type. Populated by the driver.
 * @base_addr: The partition base address. Populated by the driver.
 * @partition_size: The parition size. Populated by the driver.
 */
struct smi_ioc_fpt_partition_value {
	uint8_t boot_device;
	uint32_t partition;
	uint32_t type;
	uint32_t base_addr;
	uint32_t partition_size;
};

/**
 * struct smi_ioc_eeprom_payload - payload struct for dynamically sized ioctl eeprom data
 * @addr: Location of data buffer in userspace memory.
 * @len: The number of bytes to read/write, at most SMI_IOC_EEPROM_MAX_XFER.
 * @offset: Offset from the EEPROM base address.
 */
struct smi_ioc_eeprom_payload {
	unsigned long addr;
	uint16_t len;
	uint8_t offset;
};

/**
 * struct smi_ioc_module_payload - payload struct for dynamically sized ioctl qsfp data
 * @addr: Location of data buffer in userspace memory.
 * @device_id: Module device ID.
 * @page: Page number to access.
 * @len: Number of bytes to read/write.
 * @offset: Offset within page.
 */
struct smi_ioc_module_payload {
	unsigned long addr;
	uint8_t device_id;
	uint8_t page;
	uint8_t len;
	uint8_t offset;
};

/**
 * struct smi_ioc_qsfp_payload - one window of one QSFP cage's map.
 *
 * @addr: user address of the window, sent for every op (write data, ack mask,
 *        power policy byte, CDB page) and overwritten where the answer fills
 *        it (a Read's bytes).
 * @epoch: the seating the caller decided against. A module swapped since is
 *         refused, with the current seating in @val for a new decision.
 * @val: [out] the answer's val, as smi_abi_qsfp.h states it beside the ops;
 *       0 when the card did not answer.
 * @control_gen: Write and CdbWrite only: the last change the caller saw; a
 *               card whose count moved refuses, so two hosts cannot lose each
 *               other's change.
 * @cage: which cage, numbered as the published blocks are.
 * @bank: banked pages (10h and up) only, else 0.
 * @page: Upper page number, never FFh. An offset below 128 is Lower whatever
 *        this says.
 * @offset: the standard's own byte address.
 * @len: 1..SMI_IOC_QSFP_MAX_XFER bytes, within one page.
 * @op: SmiQsfpOp_*; one this card lacks is refused with -EOPNOTSUPP.
 *
 * The struct is copied back whatever the card answered, so a refusal's @val
 * arrives; the window only as the answer filled it, whole or not at all.
 *
 * Errors:
 * - EPERM: no CAP_SYS_ADMIN
 * - ENXIO: the PF is not READY or MISSING_INFO
 * - EINVAL: a shape the driver or the card refuses
 * - EOPNOTSUPP: an op the card lacks
 * - EAGAIN: the card is congested, retry
 * - EBUSY: the host's queue to the card is full or stuck
 * - ESTALE: the seating moved; @val is the current epoch
 * - EALREADY: another host wrote; @val is the current control_gen
 * - ENODATA: the cage is not Ready, or its module is busy with a CDB command;
 *   @val is the epoch
 * - EIO: a transfer failed, on the module's I2C or the host's link to the card
 * - ETIMEDOUT: no answer
 * - EPROTO: a malformed exchange
 *
 * EPERM, ENXIO, ENODEV, ECANCELED, EINTR, EAGAIN and EBUSY mean the request
 * was not taken; after EIO, ETIMEDOUT or ESHUTDOWN a change may have landed,
 * even in part.
 */
struct smi_ioc_qsfp_payload {
	uint64_t addr;
	uint32_t epoch;
	uint32_t val;
	uint16_t control_gen;
	uint8_t cage;
	uint8_t bank;
	uint8_t page;
	uint8_t offset;
	uint8_t len;
	uint8_t op;
};

/**
 * struct smi_ioc_shell_payload - one debug-shell line and its answer.
 *
 * @cmd: user address of the line, @cmd_len bytes, no terminator needed.
 * @cmd_len: length of the line, at most SMI_IOC_SHELL_MAX_LINE.
 * @out: user address the rendered answer is written to.
 * @out_cap: bytes @out holds; also the slice the card is asked for.
 * @out_len: [out] bytes written to @out.
 * @dropped: [out] bytes the card could not fit. Zero means the answer is
 *           whole; otherwise the total is @out_len + @dropped, so a caller
 *           that wants all of it asks again with that capacity.
 */
struct smi_ioc_shell_payload {
	uint64_t cmd;
	uint64_t out;
	uint32_t cmd_len;
	uint32_t out_cap;
	uint32_t out_len;
	uint32_t dropped;
};

/**
 * struct smi_ioc_led_payload - the locate beacon, or one LED forced.
 * @op: 0 beacon off, 1 beacon on, 2 force off, 3 force on, 4 release. 2 to 4
 *      need CAP_SYS_ADMIN.
 * @id: per-card LED index; force and release only.
 * @color: 0 green, 1 yellow; force on only.
 * @rsvd: zero.
 * @timeout_s: seconds the card holds the beacon or the force, 1 to
 *             SMI_IOC_LED_MAX_TIMEOUT_S for ops 1 to 3; unused by 0 and 4.
 */
struct smi_ioc_led_payload {
	uint8_t op;
	uint8_t id;
	uint8_t color;
	uint8_t rsvd;
	uint32_t timeout_s;
};

/**
 * enum smi_ioc_app_setup - accepted values for the SMI_IOC_APP_SETUP IOCTL
 * @IOC_APP_SETUP_REGISTER: Register a process with a device.
 * @IOC_APP_SETUP_DEREGISTER: Deregister a process from a device.
 */
enum smi_ioc_app_setup {
	IOC_APP_SETUP_REGISTER,
	IOC_APP_SETUP_DEREGISTER,
};

/* A verb taking a pointer declares that pointer's type; one whose argument
 * IS the value is _IO, since _IOW(..., <scalar>) would mean a pointer. */
#define SMI_IOC_MAGIC 'a'
#define SMI_IOC_DOWNLOAD_PDI                                                   \
	_IOW(SMI_IOC_MAGIC, 0, struct smi_ioc_data_payload *)
/* 1 and 2 were the raw BAR read/write ioctls, retired; not to be reused. */
#define SMI_IOC_GET_SENSOR_VALUE                                               \
	_IOWR(SMI_IOC_MAGIC, 3, struct smi_ioc_sensor_value *)
#define SMI_IOC_DEVICE_BOOT                                                    \
	_IOW(SMI_IOC_MAGIC, 4, struct smi_ioc_data_payload *)
#define SMI_IOC_COPY_PARTITION                                                 \
	_IOW(SMI_IOC_MAGIC, 5, struct smi_ioc_data_payload *)
#define SMI_IOC_SET_SENSOR_REFRESH _IO(SMI_IOC_MAGIC, 6)
#define SMI_IOC_GET_FPT_HDR                                                    \
	_IOR(SMI_IOC_MAGIC, 7, struct smi_ioc_fpt_hdr_value *)
#define SMI_IOC_GET_FPT_PARTITION                                              \
	_IOWR(SMI_IOC_MAGIC, 8, struct smi_ioc_fpt_partition_value *)
#define SMI_IOC_READ_EEPROM                                                    \
	_IOWR(SMI_IOC_MAGIC, 9, struct smi_ioc_eeprom_payload *)
#define SMI_IOC_WRITE_EEPROM                                                   \
	_IOW(SMI_IOC_MAGIC, 10, struct smi_ioc_eeprom_payload *)
#define SMI_IOC_APP_SETUP _IO(SMI_IOC_MAGIC, 11)
#define SMI_IOC_READ_MODULE                                                    \
	_IOW(SMI_IOC_MAGIC, 12, struct smi_ioc_module_payload *)
#define SMI_IOC_WRITE_MODULE                                                   \
	_IOW(SMI_IOC_MAGIC, 13, struct smi_ioc_module_payload *)
#define SMI_IOC_DEBUG_VERBOSITY _IO(SMI_IOC_MAGIC, 14)
#define SMI_IOC_SHELL_EXEC                                                     \
	_IOWR(SMI_IOC_MAGIC, 15, struct smi_ioc_shell_payload *)
#define SMI_IOC_LED _IOW(SMI_IOC_MAGIC, 16, struct smi_ioc_led_payload *)
#define SMI_IOC_QSFP_RW _IOWR(SMI_IOC_MAGIC, 17, struct smi_ioc_qsfp_payload *)
#define SMI_IOC_MAX (18)

#endif /* SMI_IOCTL_H */
