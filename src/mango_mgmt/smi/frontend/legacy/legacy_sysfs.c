// SPDX-License-Identifier: GPL-2.0-only
/*
 * legacy_sysfs.c - The legacy surface's files on the card's bus device.
 *
 * Copyright (c) 2023-present Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include <linux/err.h>
#include <linux/sysfs.h>
#include <linux/vmalloc.h>

#include <mango_compat/sysfs.h>

#include "core/smi_rpc.h" /* smi_pub_status_read() */
#include "core/smi_sensor.h"

#include "legacy_card.h"
#include "legacy_pub.h"
#include "legacy_qsfp.h"
#include "legacy_sysfs.h"
#include "smi_frontend.h"
#include "smi_top.h"

#define FPGA_CONFIG_MODE_QSPIX4 (7)
#define FPGA_CONFIG_MODE_OSPI (8)
#define MAX_POWER_MODE_75W (0)
#define MAX_POWER_MODE_150W (1)
#define MAX_POWER_MODE_225W (2)
#define MAX_POWER_MODE_350W (3)

/**
 * struct dev_state_map - Map of device state to human readable string.
 * @state: Device state.
 * @name: String representation of device state.
 */
struct dev_state_map {
	enum pf_dev_state state;
	const char *name;
};

static const struct dev_state_map state_names[] = {
	{ PF_DEV_STATE_INIT, "INIT" },
	{ PF_DEV_STATE_READY, "READY" },
	{ PF_DEV_STATE_MISSING_INFO, "MISSING_INFO" },
	{ PF_DEV_STATE_NO_SMC, "NO_SMC" },
	{ PF_DEV_STATE_INIT_ERROR, "INIT_ERROR" },
	{ PF_DEV_STATE_SHUTDOWN, "SHUTDOWN" },
};

/**
 * get_state_name() - Get the string representation of a device state.
 * @state: Device state to lookup.
 *
 * Return: Human readable string (empty string when state not found).
 */
static const char *get_state_name(enum pf_dev_state state)
{
	int i = 0;

	for (i = 0; i < ARRAY_SIZE(state_names); i++)
		if (state_names[i].state == state)
			return state_names[i].name;
	return "";
}

/* Compatibility placeholder for existing userspace; no hardware UUID is
 * read. */
static ssize_t logic_uuid_show(struct device *dev, struct device_attribute *da,
			       char *buf)
{
	return sysfs_emit(buf, "%s\n", "00000000000000000000000000000000");
}
static DEVICE_ATTR_RO(logic_uuid);

/**
 * dev_state_show() - Sysfs read callback for 'dev_state' attribute.
 * @dev: Device this attribute belongs to.
 * @da: Pointer to device attribute struct.
 * @buf: Output character buffer.
 *
 * Return: Number of bytes written to output buffer.
 */
static ssize_t dev_state_show(struct device *dev, struct device_attribute *da,
			      char *buf)
{
	int ret = 0;
	struct pf_dev_struct *pf_dev = NULL;

	if (!dev || !da || !buf)
		return -EINVAL;

	pf_dev = get_pf_dev_entry(dev, PF_DEV_CACHE_DEV);

	if (pf_dev) {
		ret = sysfs_emit(buf, "%s\n", get_state_name(pf_dev->state));
		put_pf_dev_entry(pf_dev);
	} else {
		ret = -ENODEV;
	}

	return ret;
}
static DEVICE_ATTR_RO(dev_state);

/**
 * dev_name_show() - Sysfs read callback for 'dev_name' attribute.
 * @dev: Device this attribute belongs to.
 * @da: Pointer to device attribute struct.
 * @buf: Output character buffer.
 *
 * Return: Number of bytes written to output buffer.
 */
static ssize_t dev_name_show(struct device *dev, struct device_attribute *da,
			     char *buf)
{
	int ret = 0;
	struct pf_dev_struct *pf_dev = NULL;
	struct smi_board *board = NULL;

	if (!dev || !da || !buf)
		return -EINVAL;

	pf_dev = get_pf_dev_entry(dev, PF_DEV_CACHE_DEV);

	if (!pf_dev)
		return -ENODEV;

	/* Must use dynamically allocated memory here due to a large struct. */
	board = vzalloc(sizeof(*board));

	if (!board) {
		put_pf_dev_entry(pf_dev);
		return -ENOMEM;
	}

	ret = read_board_info(pf_dev, board);

	if (!ret) {
		ret = sysfs_emit(buf, "%s\n",
				 board->f[SmiBoardField_ProductName].bytes);
	} else {
		ret = sysfs_emit(buf, "Unknown Device\n");
	}

	put_pf_dev_entry(pf_dev);
	vfree(board);
	return ret;
}
static DEVICE_ATTR_RO(dev_name);

/**
 * smc_version_show() - Sysfs read callback for the firmware version attribute.
 * @dev: Device this attribute belongs to.
 * @da: Pointer to device attribute struct.
 * @buf: Output character buffer.
 *
 * Return: Number of bytes written to output buffer.
 */
static ssize_t smc_version_show(struct device *dev, struct device_attribute *da,
				char *buf)
{
	int ret = 0;
	struct pf_dev_struct *pf_dev = NULL;

	if (!dev || !da || !buf)
		return -EINVAL;

	pf_dev = get_pf_dev_entry(dev, PF_DEV_CACHE_DEV);

	if (pf_dev) {
		const struct smc_version *v = &pf_dev->facts.version;

		/* The firmware's own version, MAJOR.MINOR.PATCH-YYMMDD. It
		 * gates nothing; a fixed 1.0.0 left the date as the only way
		 * to tell two firmwares apart. */
		if (pf_dev->facts.present)
			ret = sysfs_emit(buf, SMI_FW_VERSION_FMT "\n",
					 SMI_FW_VERSION_ARGS(v));
		put_pf_dev_entry(pf_dev);
	} else {
		ret = -ENODEV;
	}

	return ret;
}
static DEVICE_ATTR_RO(smc_version);

/* Which wire generation this card settled on, as its SMI_GEN_BIT_* bit. */
static ssize_t wire_gen_show(struct device *dev, struct device_attribute *da,
			     char *buf)
{
	struct pf_dev_struct *pf_dev = NULL;
	uint16_t bit = 0;

	if (!dev || !da || !buf)
		return -EINVAL;

	pf_dev = get_pf_dev_entry(dev, PF_DEV_CACHE_DEV);
	if (!pf_dev)
		return -ENODEV;

	bit = pf_dev->facts.gens_bit;
	put_pf_dev_entry(pf_dev);

	/* Unsettled reads as nothing rather than as a generation. */
	return bit ? sysfs_emit(buf, "0x%04x\n", bit) : sysfs_emit(buf, "\n");
}
static DEVICE_ATTR_RO(wire_gen);

/* Which UAPI surface this card presents. A different axis from wire_gen. */
static ssize_t uapi_show(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	struct pf_dev_struct *pf_dev = NULL;
	const char *name = NULL;

	if (!dev || !da || !buf)
		return -EINVAL;

	pf_dev = get_pf_dev_entry(dev, PF_DEV_CACHE_DEV);
	if (!pf_dev)
		return -ENODEV;

	/* A surface's name is static storage, so it outlives this reference. */
	name = smi_frontend_name(pf_dev);
	put_pf_dev_entry(pf_dev);

	/* Unbound reads as nothing rather than as a surface. */
	return name ? sysfs_emit(buf, "%s\n", name) : sysfs_emit(buf, "\n");
}
static DEVICE_ATTR_RO(uapi);

/* The words of the Card facts, by the wire number each value has. A number
 * with no word here -- unknown, or one a newer card added -- reads "unknown". */
static const char *const smi_boot_source_words[] = {
	[SmiBootSource_Primary] = "primary",
	[SmiBootSource_Golden] = "golden",
};

static const char *const smi_boot_reason_words[] = {
	[SmiBootReason_PoweredUp] = "powered_up",
	[SmiBootReason_ExtPor] = "ext_por",
	[SmiBootReason_SwPor] = "sw_por",
	[SmiBootReason_SlrPor] = "slr_por",
	[SmiBootReason_ErrPor] = "err_por",
	[SmiBootReason_DapSrst] = "dap_srst",
	[SmiBootReason_ErrSrst] = "err_srst",
	[SmiBootReason_SwSrst] = "sw_srst",
	[SmiBootReason_SlrSrst] = "slr_srst",
};

static const char *const smi_health_words[] = {
	[SmiHealth_Healthy] = "healthy",
	[SmiHealth_Degraded] = "degraded",
	[SmiHealth_Critical] = "critical",
	[SmiHealth_Fatal] = "fatal",
};

/* One Card fact's word, its record found by id rather than place. A card that
 * states none, or states it without data, reads "unknown". */
static ssize_t card_item_show(struct device *dev, char *buf,
			      enum smi_card_item item, const char *const *words,
			      size_t n)
{
	struct pf_dev_struct *pf_dev = get_pf_dev_entry(dev, PF_DEV_CACHE_DEV);
	struct smi_sensor rec = { 0 };
	const char *word = NULL;

	if (!pf_dev)
		return -ENODEV;

	if (!read_sensor_fresh(pf_dev, SmiRepo_Card, item - 1, &rec, NULL) &&
	    (SmiSensorStatus_Valid == rec.status) && (rec.value >= 0) &&
	    (rec.value < n))
		word = words[rec.value];
	put_pf_dev_entry(pf_dev);

	return sysfs_emit(buf, "%s\n", word ? word : "unknown");
}

static ssize_t boot_source_show(struct device *dev, struct device_attribute *da,
				char *buf)
{
	return card_item_show(dev, buf, SmiCardItem_BootSource,
			      smi_boot_source_words,
			      ARRAY_SIZE(smi_boot_source_words));
}
static DEVICE_ATTR_RO(boot_source);

static ssize_t boot_reason_show(struct device *dev, struct device_attribute *da,
				char *buf)
{
	return card_item_show(dev, buf, SmiCardItem_BootReason,
			      smi_boot_reason_words,
			      ARRAY_SIZE(smi_boot_reason_words));
}
static DEVICE_ATTR_RO(boot_reason);

static ssize_t health_show(struct device *dev, struct device_attribute *da,
			   char *buf)
{
	return card_item_show(dev, buf, SmiCardItem_Health, smi_health_words,
			      ARRAY_SIZE(smi_health_words));
}
static DEVICE_ATTR_RO(health);

/* Write-only, in seconds: the beacon blinks that long and goes out by itself,
 * and 0 turns it off. */
static ssize_t locate_store(struct device *dev, struct device_attribute *da,
			    const char *buf, size_t count)
{
	struct pf_dev_struct *pf_dev;
	u16 seconds;
	int ret;

	ret = kstrtou16(buf, 0, &seconds);
	if (ret)
		return ret;

	pf_dev = get_pf_dev_entry(dev, PF_DEV_CACHE_DEV);
	if (!pf_dev)
		return -ENODEV;

	ret = smi_pf_dev_locate(pf_dev, seconds);
	put_pf_dev_entry(pf_dev);

	return ret ? ret : count;
}
static DEVICE_ATTR_WO(locate);

/**
 * show_cmd_aborted() - Emit one planned-abort counter.
 * @dev: Device this attribute belongs to.
 * @da: Pointer to device attribute struct.
 * @buf: Output character buffer.
 * @reason: Which counter to report.
 *
 * Debug-level logs leave no standing signal, so these counters are it: one
 * value per file, monotonic, reset only by a re-probe.
 *
 * Return: Number of bytes written to output buffer.
 */
static ssize_t show_cmd_aborted(struct device *dev, struct device_attribute *da,
				char *buf, enum smi_cmd_abort_reason reason)
{
	struct pf_dev_struct *pf_dev = NULL;
	ssize_t ret;

	if (!dev || !da || !buf)
		return -EINVAL;

	pf_dev = get_pf_dev_entry(dev, PF_DEV_CACHE_DEV);

	if (!pf_dev)
		return -ENODEV;

	ret = sysfs_emit(buf, "%lld\n",
			 atomic64_read(&pf_dev->cmd_aborted[reason]));

	put_pf_dev_entry(pf_dev);
	return ret;
}

#define MANGO_CMD_ABORTED_ATTR(_name, _reason)                                 \
	static ssize_t _name##_show(struct device *dev,                        \
				    struct device_attribute *da, char *buf)    \
	{                                                                      \
		return show_cmd_aborted(dev, da, buf, (_reason));              \
	}                                                                      \
	static DEVICE_ATTR_RO(_name)

/* Growth means sensor reads are being cut short (poll timeout, Ctrl-C). */
MANGO_CMD_ABORTED_ATTR(cmd_aborted_signal, SMI_CMD_ABORT_SIGNAL);

/* Growth means teardown won the final submit gate before the FAL write. */
MANGO_CMD_ABORTED_ATTR(cmd_aborted_quiesce, SMI_CMD_ABORT_QUIESCE);

/* Live-service view. Removal logs the final value too: disabling the device
 * makes its sysfs callbacks reject new references before teardown. */
MANGO_CMD_ABORTED_ATTR(cmd_aborted_shutdown, SMI_CMD_ABORT_SHUTDOWN);

/* Growth means something keeps polling a card whose heartbeat is gone. */
MANGO_CMD_ABORTED_ATTR(cmd_aborted_nodev, SMI_CMD_ABORT_NODEV);

/**
 * enum sysfs_mfg_field - List of exposed EEPROM fields.
 * @SYSFS_MFG_EEPROM_VERSION: The eeprom version.
 * @SYSFS_MFG_PRODUCT_NAME: the product name.
 * @SYSFS_MFG_BOARD_REV: the board revision.
 * @SYSFS_MFG_BOARD_SERIAL: the board serial number.
 * @SYSFS_MFG_MEMORY_SIZE: Max memory in GB.
 * @SYSFS_MFG_PART_NUM: Board part number.
 * @SYSFS_MFG_M_PART_NUM: Manufacturer part number.
 * @SYSFS_MFG_MAC_ADDR_C: Number of MAC IDs.
 * @SYSFS_MFG_MAC_ADDR: Primary MAC ID.
 * @SYSFS_MFG_MAC_ADDR_N:  Number of MAC IDs.
 * @SYSFS_MFG_ACTIVE_STATE: ‘A’ = Active, ‘P’ = Passive.
 * @SYSFS_MFG_CONFIG_MODE: 07: Master SPI x4 (QSPIx4), 08: OSPI.
 * @SYSFS_MFG_M_DATE: Manufacturing Date.
 * @SYSFS_MFG_UUID: Used to uniquely ID the product.
 * @SYSFS_MFG_PCIE_ID: Vendor ID, Device ID, SubVendor ID, SubDevice ID.
 * @SYSFS_MFG_POWER_MODE: Max power mode.
 * @SYSFS_MFG_OEM_ID: OEM ID.
 * @SYSFS_MFG_CAPABILITY: Capability word.
 */
enum sysfs_mfg_field {
	SYSFS_MFG_EEPROM_VERSION,
	SYSFS_MFG_PRODUCT_NAME,
	SYSFS_MFG_BOARD_REV,
	SYSFS_MFG_BOARD_SERIAL,
	SYSFS_MFG_MEMORY_SIZE,
	SYSFS_MFG_PART_NUM,
	SYSFS_MFG_M_PART_NUM,
	SYSFS_MFG_MAC_ADDR_C,
	SYSFS_MFG_MAC_ADDR,
	SYSFS_MFG_MAC_ADDR_N,
	SYSFS_MFG_ACTIVE_STATE,
	SYSFS_MFG_CONFIG_MODE,
	SYSFS_MFG_M_DATE,
	SYSFS_MFG_UUID,
	SYSFS_MFG_PCIE_ID,
	SYSFS_MFG_POWER_MODE,
	SYSFS_MFG_OEM_ID,
	SYSFS_MFG_CAPABILITY,
};

/**
 * struct mfg_attribute - sysfs attribute for EEPROM fields
 * @attr: Low level attribute struct.
 * @var: EEPROM field identifier.
 */
struct mfg_attribute {
	struct device_attribute attr;
	enum sysfs_mfg_field var;
};

/* Wrapper around a sysfs attribute that stores extra context, so one 'show'
 * function serves every field. */
#define DEVICE_MFG_ATTR(_name, _show, _var)                                    \
	struct mfg_attribute dev_attr_##_name = {                              \
		__ATTR(_name, 0444, _show, NULL), _var                         \
	}

/**
 * read_mfg_field() - Sysfs read callback for EEPROM attributes.
 * @dev: Device this attribute belongs to.
 * @da: Pointer to device attribute struct.
 * @buf: Output character buffer.
 *
 * Return: Number of bytes written to output buffer.
 */
static ssize_t read_mfg_field(struct device *dev, struct device_attribute *da,
			      char *buf)
{
	int ret = 0;
	struct smi_board *bd = NULL;
	struct pf_dev_struct *pf_dev = NULL;
	struct mfg_attribute *attr = NULL;

	if (!dev || !da || !buf)
		return -EINVAL;

	pf_dev = get_pf_dev_entry(dev, PF_DEV_CACHE_DEV);

	if (!pf_dev)
		return -ENODEV;

	/* Dynamically allocated: the record is large, and a copy is what keeps
	 * a concurrent FPT refresh from freeing it mid-read. */
	bd = vzalloc(sizeof(*bd));

	if (!bd) {
		put_pf_dev_entry(pf_dev);
		return -ENOMEM;
	}

	if (read_board_info(pf_dev, bd)) {
		put_pf_dev_entry(pf_dev);
		vfree(bd);
		return -EINVAL;
	}

	attr = container_of(da, struct mfg_attribute, attr);

	switch (attr->var) {
	case SYSFS_MFG_EEPROM_VERSION:
		ret = sysfs_emit(buf, "%s\n",
				 bd->f[SmiBoardField_EepromVersion].bytes);
		break;

	case SYSFS_MFG_PRODUCT_NAME:
		ret = sysfs_emit(buf, "%s\n",
				 bd->f[SmiBoardField_ProductName].bytes);
		break;

	case SYSFS_MFG_BOARD_REV:
		ret = sysfs_emit(buf, "%s\n",
				 bd->f[SmiBoardField_BoardRev].bytes);
		break;

	case SYSFS_MFG_BOARD_SERIAL:
		ret = sysfs_emit(buf, "%s\n",
				 bd->f[SmiBoardField_BoardSerial].bytes);
		break;

	case SYSFS_MFG_MEMORY_SIZE:
		if (bd->f[SmiBoardField_MemorySize].len) {
			ret = sysfs_emit(buf, "%s\n",
					 bd->f[SmiBoardField_MemorySize].bytes);
		} else {
			ret = sysfs_emit(buf, "%s\n", "N/A");
		}
		break;

	case SYSFS_MFG_PART_NUM:
		ret = sysfs_emit(buf, "%s\n",
				 bd->f[SmiBoardField_PartNumber].bytes);
		break;

	case SYSFS_MFG_M_PART_NUM:
		if (bd->f[SmiBoardField_MfgPartNumber].len) {
			ret = sysfs_emit(
				buf, "%s\n",
				bd->f[SmiBoardField_MfgPartNumber].bytes);
		} else {
			ret = sysfs_emit(buf, "%s\n", "N/A");
		}
		break;

	case SYSFS_MFG_MAC_ADDR_C:
		ret = sysfs_emit(buf, "%d\n",
				 bd->f[SmiBoardField_MacAddrCount].bytes[0]);
		break;

	case SYSFS_MFG_MAC_ADDR:
		ret = sysfs_emit(buf, "%02x:%02x:%02x:%02x:%02x:%02x\n",
				 bd->f[SmiBoardField_FirstMacAddr].bytes[0],
				 bd->f[SmiBoardField_FirstMacAddr].bytes[1],
				 bd->f[SmiBoardField_FirstMacAddr].bytes[2],
				 bd->f[SmiBoardField_FirstMacAddr].bytes[3],
				 bd->f[SmiBoardField_FirstMacAddr].bytes[4],
				 bd->f[SmiBoardField_FirstMacAddr].bytes[5]);
		break;

	case SYSFS_MFG_MAC_ADDR_N: {
		uint64_t mac_n =
			((uint64_t)bd->f[SmiBoardField_FirstMacAddr].bytes[0]
			 << 40) |
			((uint64_t)bd->f[SmiBoardField_FirstMacAddr].bytes[1]
			 << 32) |
			((uint64_t)bd->f[SmiBoardField_FirstMacAddr].bytes[2]
			 << 24) |
			((uint64_t)bd->f[SmiBoardField_FirstMacAddr].bytes[3]
			 << 16) |
			((uint64_t)bd->f[SmiBoardField_FirstMacAddr].bytes[4]
			 << 8) |
			((uint64_t)bd->f[SmiBoardField_FirstMacAddr].bytes[5]);

		/* Add count-1: the first address is already counted */
		if (0 != bd->f[SmiBoardField_MacAddrCount].bytes[0])
			mac_n += bd->f[SmiBoardField_MacAddrCount].bytes[0] - 1;
		else
			mac_n = 0;

		ret = sysfs_emit(buf, "%02x:%02x:%02x:%02x:%02x:%02x\n",
				 (uint8_t)(mac_n >> 40), (uint8_t)(mac_n >> 32),
				 (uint8_t)(mac_n >> 24), (uint8_t)(mac_n >> 16),
				 (uint8_t)(mac_n >> 8), (uint8_t)(mac_n));

		break;
	}

	case SYSFS_MFG_ACTIVE_STATE:
		switch (bd->f[SmiBoardField_ActiveState].bytes[0]) {
		case 'A':
			ret = sysfs_emit(buf, "%s\n", "Active");
			break;

		case 'P':
			ret = sysfs_emit(buf, "%s\n", "Passive");
			break;

		default:
			ret = sysfs_emit(buf, "%s\n", "N/A");
			break;
		}
		break;

	case SYSFS_MFG_CONFIG_MODE:
		switch (bd->f[SmiBoardField_ConfigMode].bytes[0]) {
		case FPGA_CONFIG_MODE_QSPIX4:
			ret = sysfs_emit(buf, "%s\n", "QSPIx4");
			break;

		case FPGA_CONFIG_MODE_OSPI:
			ret = sysfs_emit(buf, "%s\n", "OSPI");
			break;

		default:
			ret = sysfs_emit(buf, "%s\n", "N/A");
			break;
		}
		break;

	case SYSFS_MFG_M_DATE: {
		struct smi_board_field *date = &bd->f[SmiBoardField_MfgDate];
		uint8_t len = min_t(uint8_t, date->len,
				    (uint8_t)(sizeof(date->bytes) - 1));

		/* Terminated on this copy, in bounds: the field carries a
		 * length, not a NUL, and the store is no longer the buffer. */
		date->bytes[len] = '\0';
		ret = sysfs_emit(buf, "%s\n", date->bytes);
		break;
	}

	case SYSFS_MFG_UUID:
		ret = sysfs_emit(
			buf,
			"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
			bd->f[SmiBoardField_Uuid].bytes[0],
			bd->f[SmiBoardField_Uuid].bytes[1],
			bd->f[SmiBoardField_Uuid].bytes[2],
			bd->f[SmiBoardField_Uuid].bytes[3],
			bd->f[SmiBoardField_Uuid].bytes[4],
			bd->f[SmiBoardField_Uuid].bytes[5],
			bd->f[SmiBoardField_Uuid].bytes[6],
			bd->f[SmiBoardField_Uuid].bytes[7],
			bd->f[SmiBoardField_Uuid].bytes[8],
			bd->f[SmiBoardField_Uuid].bytes[9],
			bd->f[SmiBoardField_Uuid].bytes[10],
			bd->f[SmiBoardField_Uuid].bytes[11],
			bd->f[SmiBoardField_Uuid].bytes[12],
			bd->f[SmiBoardField_Uuid].bytes[13],
			bd->f[SmiBoardField_Uuid].bytes[14],
			bd->f[SmiBoardField_Uuid].bytes[15]);
		break;

	case SYSFS_MFG_PCIE_ID:
		if (bd->f[SmiBoardField_PcieId].len) {
			/* Vendor, Device, Subsystem Vendor, Subsystem Device */
			ret = sysfs_emit(
				buf, "%02x%02x, %02x%02x, %02x%02x, %02x%02x\n",
				bd->f[SmiBoardField_PcieId].bytes[0],
				bd->f[SmiBoardField_PcieId].bytes[1],
				bd->f[SmiBoardField_PcieId].bytes[2],
				bd->f[SmiBoardField_PcieId].bytes[3],
				bd->f[SmiBoardField_PcieId].bytes[4],
				bd->f[SmiBoardField_PcieId].bytes[5],
				bd->f[SmiBoardField_PcieId].bytes[6],
				bd->f[SmiBoardField_PcieId].bytes[7]);
		} else {
			ret = sysfs_emit(buf, "%s\n", "N/A");
		}
		break;

	case SYSFS_MFG_POWER_MODE:
		switch (bd->f[SmiBoardField_MaxPowerMode].bytes[0]) {
		case MAX_POWER_MODE_75W:
			ret = sysfs_emit(buf, "%s\n", "75W");
			break;

		case MAX_POWER_MODE_150W:
			ret = sysfs_emit(buf, "%s\n", "150W");
			break;

		case MAX_POWER_MODE_225W:
			ret = sysfs_emit(buf, "%s\n", "225W");
			break;

		case MAX_POWER_MODE_350W:
			ret = sysfs_emit(buf, "%s\n", "350W");
			break;

		default:
			ret = sysfs_emit(buf, "%s\n", "N/A");
			break;
		}
		break;

	case SYSFS_MFG_OEM_ID:
		/* Its own field's length, and both bytes it prints. */
		if (bd->f[SmiBoardField_OemId].len >= 2) {
			ret = sysfs_emit(buf, "0x%02x%02x\n",
					 bd->f[SmiBoardField_OemId].bytes[1],
					 bd->f[SmiBoardField_OemId].bytes[0]);
		} else {
			ret = sysfs_emit(buf, "%s\n", "N/A");
		}
		break;

	case SYSFS_MFG_CAPABILITY:
		if (bd->f[SmiBoardField_Capability].len) {
			/* Vendor, Device, Subsystem Vendor, Subsystem Device */
			ret = sysfs_emit(
				buf, "%02x%02x\n",
				bd->f[SmiBoardField_Capability].bytes[1],
				bd->f[SmiBoardField_Capability].bytes[0]);
		} else {
			ret = sysfs_emit(buf, "%s\n", "N/A");
		}
		break;

	default:
		ret = -EINVAL;
		break;
	}

	put_pf_dev_entry(pf_dev);
	vfree(bd);
	return ret;
}
static DEVICE_MFG_ATTR(eeprom_version, read_mfg_field,
		       SYSFS_MFG_EEPROM_VERSION);
static DEVICE_MFG_ATTR(product_name, read_mfg_field, SYSFS_MFG_PRODUCT_NAME);
static DEVICE_MFG_ATTR(board_rev, read_mfg_field, SYSFS_MFG_BOARD_REV);
static DEVICE_MFG_ATTR(board_serial, read_mfg_field, SYSFS_MFG_BOARD_SERIAL);
static DEVICE_MFG_ATTR(memory_size, read_mfg_field, SYSFS_MFG_MEMORY_SIZE);
static DEVICE_MFG_ATTR(part_num, read_mfg_field, SYSFS_MFG_PART_NUM);
static DEVICE_MFG_ATTR(mfg_part_num, read_mfg_field, SYSFS_MFG_M_PART_NUM);
static DEVICE_MFG_ATTR(mac_addr_count, read_mfg_field, SYSFS_MFG_MAC_ADDR_C);
static DEVICE_MFG_ATTR(mac_addr, read_mfg_field, SYSFS_MFG_MAC_ADDR);
static DEVICE_MFG_ATTR(mac_addr_n, read_mfg_field, SYSFS_MFG_MAC_ADDR_N);
static DEVICE_MFG_ATTR(active_state, read_mfg_field, SYSFS_MFG_ACTIVE_STATE);
static DEVICE_MFG_ATTR(fpga_config_mode, read_mfg_field, SYSFS_MFG_CONFIG_MODE);
static DEVICE_MFG_ATTR(mfg_date, read_mfg_field, SYSFS_MFG_M_DATE);
static DEVICE_MFG_ATTR(mfg_uuid, read_mfg_field, SYSFS_MFG_UUID);
static DEVICE_MFG_ATTR(pcie_id, read_mfg_field, SYSFS_MFG_PCIE_ID);
static DEVICE_MFG_ATTR(power_mode, read_mfg_field, SYSFS_MFG_POWER_MODE);
static DEVICE_MFG_ATTR(oem_id, read_mfg_field, SYSFS_MFG_OEM_ID);
static DEVICE_MFG_ATTR(mfg_capability, read_mfg_field, SYSFS_MFG_CAPABILITY);

static const char *sdr_repo_state_str(enum smi_repo_state state)
{
	switch (state) {
	case SMI_REPO_PRESENT:
		return "present";
	case SMI_REPO_EMPTY:
		return "empty";
	default:
		return "absent";
	}
}

/**
 * sdr_repos_show() - Sysfs read callback for the 'sdr_repos' attribute.
 * @dev: Device this attribute belongs to.
 * @da: Pointer to device attribute struct.
 * @buf: Output character buffer.
 *
 * One line per repo: name, state, then the record count or the errno for its
 * absence -- which matters, since "no such repo" and "the read failed" differ
 * only there:
 *
 *   -EIO (-5)          SMC reported a generic failure, or teardown removed
 *                      the command
 *   -ENOMEM (-12)      the response or its payload could not be allocated
 *   -EINVAL (-22)      SMC rejected the request - most often a repo it does
 *                      not implement - or the frame header made no sense
 *   -ENOMSG (-42)      a frame that carried zero records
 *   -ENODATA (-61)     SMC answered InvalidConfiguration
 *   -EPROTO (-71)      the response did not match the request
 *   -EOVERFLOW (-75)   a field ran past its bound
 *   -EILSEQ (-84)      the frame did not end where it said it would
 *   -ETIMEDOUT (-110)  no response
 *
 * A positive value is a bug. The sensor listing is all or nothing, so a sensor
 * repo listed absent is optional; the partition tables are read apart and
 * listed with their own errno.
 *
 * Return: Number of bytes written to output buffer.
 */
static ssize_t sdr_repos_show(struct device *dev, struct device_attribute *da,
			      char *buf)
{
	struct pf_dev_struct *pf_dev = NULL;
	struct smi_repo_summary repos[SMI_REPO_CNT] = { 0 };
	ssize_t len = 0;
	int i, count;

	if (!dev || !da || !buf)
		return -EINVAL;

	pf_dev = get_pf_dev_entry(dev, PF_DEV_CACHE_DEV);

	if (!pf_dev)
		return -ENODEV;

	/* One census, so the rows below cannot mix a live array with a freed
	 * one -- delete_sensors() clears both the array and its length. The
	 * partition tables are this surface's own, so their row is too. */
	count = read_sensor_repos(pf_dev, repos, ARRAY_SIZE(repos));
	for (i = 0; i < count; i++)
		if (repos[i].rid == SmiRepo_Fpt)
			smi_fpt_summary(&smi_legacy_card(pf_dev)->fpt,
					&repos[i]);

	for (i = 0; i < count; i++)
		len += sysfs_emit_at(buf, len, "%-12s %-8s %d\n",
				     smi_repo_name(repos[i].rid),
				     sdr_repo_state_str(repos[i].state),
				     repos[i].state == SMI_REPO_PRESENT
					     ? repos[i].num_records
					     : repos[i].last_error);

	put_pf_dev_entry(pf_dev);
	return len;
}
static DEVICE_ATTR_RO(sdr_repos);

/**
 * eeprom_size_show() - The addressable extent of the identity EEPROM, in bytes.
 *
 * Advertised at negotiation, so a tool bounds reads by what the part has; a
 * card that cannot be asked reports the assumed extent.
 *
 * Return: Number of bytes written to output buffer.
 */
static ssize_t eeprom_size_show(struct device *dev, struct device_attribute *da,
				char *buf)
{
	struct pf_dev_struct *pf_dev = NULL;
	uint32_t extent;

	if (!dev || !da || !buf)
		return -EINVAL;

	pf_dev = get_pf_dev_entry(dev, PF_DEV_CACHE_DEV);
	if (!pf_dev)
		return -ENODEV;

	if (!pf_dev->facts.present) {
		put_pf_dev_entry(pf_dev);
		return -ENODEV;
	}

	extent = pf_dev->facts.eeprom_bytes;
	put_pf_dev_entry(pf_dev);
	return sysfs_emit(buf, "%u\n", extent);
}
static DEVICE_ATTR_RO(eeprom_size);

/**
 * fw_build_show() - The name the card's tree gave the firmware it runs.
 *
 * Beside smc_version, which is the three numbers every build of a release
 * answers alike: this is what tells two of them apart. Empty from a card whose
 * window states no name.
 *
 * Return: Number of bytes written to output buffer.
 */
static ssize_t fw_build_show(struct device *dev, struct device_attribute *da,
			     char *buf)
{
	struct pf_dev_struct *pf_dev = NULL;
	ssize_t ret;

	if (!dev || !da || !buf)
		return -EINVAL;

	pf_dev = get_pf_dev_entry(dev, PF_DEV_CACHE_DEV);
	if (!pf_dev)
		return -ENODEV;

	if (!pf_dev->facts.present) {
		put_pf_dev_entry(pf_dev);
		return -ENODEV;
	}

	ret = sysfs_emit(buf, "%s\n", pf_dev->facts.build_tag);
	put_pf_dev_entry(pf_dev);
	return ret;
}
static DEVICE_ATTR_RO(fw_build);

/**
 * published_show() - The Status section firmware writes unasked, and its
 * counters.
 *
 * One read of what the card keeps current, so it costs no round trip. Each
 * counter is named as the card's Counter record names it.
 *
 * Return: Number of bytes written to output buffer.
 */
static ssize_t published_show(struct device *dev, struct device_attribute *da,
			      char *buf)
{
	struct published_counter cnt[SMI_LEGACY_COUNTERS_MAX];
	struct pf_dev_struct *pf_dev = NULL;
	struct smi_pub_status st;
	ssize_t len = 0;
	int ret, n, i;

	if (!dev || !da || !buf)
		return -EINVAL;

	pf_dev = get_pf_dev_entry(dev, PF_DEV_CACHE_DEV);
	if (!pf_dev)
		return -ENODEV;

	if (!pf_dev->facts.present) {
		put_pf_dev_entry(pf_dev);
		return -ENODEV;
	}

	ret = smi_pub_status_read(pf_dev, &st);
	if (ret) {
		put_pf_dev_entry(pf_dev);
		return ret;
	}
	n = read_published_counters(pf_dev, cnt, ARRAY_SIZE(cnt));
	put_pf_dev_entry(pf_dev);

	len += sysfs_emit_at(buf, len, "seq        %u\n", st.head.sect.seq);
	len += sysfs_emit_at(buf, len, "tick       %u\n", st.head.tick);
	len += sysfs_emit_at(buf, len, "uptime_ms  %u\n", st.head.uptime_ms);
	len += sysfs_emit_at(buf, len, "state      %u\n", st.head.state);
	len += sysfs_emit_at(buf, len, "busy_op    0x%x\n", st.busy_op);
	/* The card's tick cadence, which a watcher of the line above needs
	 * before a tick that has not moved means anything. */
	len += sysfs_emit_at(buf, len, "tick_ms    %u\n", st.head.tick_ms);

	/* Named where the card names a slot, numbered where it names none. */
	for (i = 0; i < n; i++) {
		if (cnt[i].name[0])
			len += sysfs_emit_at(buf, len, "%-10s %u\n",
					     cnt[i].name, cnt[i].value);
		else
			len += sysfs_emit_at(buf, len, "cnt%-7d %u\n", i,
					     cnt[i].value);
	}

	return len;
}
static DEVICE_ATTR_RO(published);

/* The boot the card's sections carry, as stated at attach: a restart ends the
 * session, and the next one reads a different number. */
static ssize_t boot_id_show(struct device *dev, struct device_attribute *da,
			    char *buf)
{
	struct pf_dev_struct *pf_dev = get_pf_dev_entry(dev, PF_DEV_CACHE_DEV);
	uint32_t boot;

	if (!pf_dev)
		return -ENODEV;

	boot = pf_dev->facts.boot_id;
	put_pf_dev_entry(pf_dev);

	return sysfs_emit(buf, "%u\n", boot);
}
static DEVICE_ATTR_RO(boot_id);

/* The card behind a cage file, counted: -ENODEV while none is registered,
 * -ENXIO while its PF serves no request, the ioctl gate's set. */
static struct pf_dev_struct *qsfp_pf_dev_get(struct device *dev)
{
	struct pf_dev_struct *pf_dev = get_pf_dev_entry(dev, PF_DEV_CACHE_DEV);

	if (!pf_dev)
		return ERR_PTR(-ENODEV);
	if (!smi_pf_dev_ready(pf_dev)) {
		put_pf_dev_entry(pf_dev);
		return ERR_PTR(-ENXIO);
	}

	return pf_dev;
}

/* The topology counter, straight off Status: when it moves, a cage changed and
 * its block says how. Present only where the card publishes cages. */
static ssize_t qsfp_gen_show(struct device *dev, struct device_attribute *da,
			     char *buf)
{
	struct pf_dev_struct *pf_dev = qsfp_pf_dev_get(dev);
	struct smi_pub_status st;
	int ret;

	if (IS_ERR(pf_dev))
		return PTR_ERR(pf_dev);

	ret = smi_pub_status_read(pf_dev, &st);
	put_pf_dev_entry(pf_dev);

	return ret ? ret : sysfs_emit(buf, "%u\n", st.qsfp_gen);
}
static DEVICE_ATTR_RO(qsfp_gen);

/* Common attributes for every device type (PCI and Platform). Only the last
 * element may be NULL. */
static struct attribute *smi_common_attrs[] = {
	&dev_attr_dev_state.attr, &dev_attr_dev_name.attr,
	&dev_attr_smc_version.attr, &dev_attr_boot_source.attr,
	&dev_attr_boot_reason.attr, &dev_attr_health.attr,
	&dev_attr_locate.attr, &dev_attr_sdr_repos.attr,
	&dev_attr_eeprom_size.attr, &dev_attr_published.attr,
	&dev_attr_boot_id.attr, &dev_attr_logic_uuid.attr,
	&dev_attr_wire_gen.attr, &dev_attr_uapi.attr,
	&dev_attr_pcie_id.attr.attr, &dev_attr_fw_build.attr,
	&dev_attr_qsfp_gen.attr,

	/* planned command aborts (logged at debug level only) */
	&dev_attr_cmd_aborted_signal.attr, &dev_attr_cmd_aborted_quiesce.attr,
	&dev_attr_cmd_aborted_shutdown.attr, &dev_attr_cmd_aborted_nodev.attr,

	/* mfg data */
	&dev_attr_eeprom_version.attr.attr, &dev_attr_product_name.attr.attr,
	&dev_attr_board_rev.attr.attr, &dev_attr_board_serial.attr.attr,
	&dev_attr_memory_size.attr.attr, &dev_attr_part_num.attr.attr,
	&dev_attr_mfg_part_num.attr.attr, &dev_attr_mac_addr_count.attr.attr,
	&dev_attr_mac_addr.attr.attr, &dev_attr_mac_addr_n.attr.attr,
	&dev_attr_active_state.attr.attr, &dev_attr_fpga_config_mode.attr.attr,
	&dev_attr_mfg_date.attr.attr, &dev_attr_mfg_uuid.attr.attr,
	&dev_attr_power_mode.attr.attr, &dev_attr_oem_id.attr.attr,
	&dev_attr_mfg_capability.attr.attr,

	NULL
};

/* Hide what this card cannot answer: an attribute that exists and always fails
 * traps every reader. Both facts are settled at attach. */
static umode_t smi_common_attr_visible(struct kobject *kobj,
				       struct attribute *attr, int n)
{
	struct pf_dev_struct *pf_dev = smi_pf_dev_of(kobj_to_dev(kobj));

	if (!pf_dev || !pf_dev->facts.present)
		return (attr == &dev_attr_published.attr ||
			attr == &dev_attr_boot_id.attr ||
			attr == &dev_attr_fw_build.attr)
			       ? 0
			       : attr->mode;

	if ((attr == &dev_attr_published.attr) ||
	    (attr == &dev_attr_boot_id.attr))
		return pf_dev->facts.publishes ? attr->mode : 0;

	if (attr == &dev_attr_fw_build.attr)
		return pf_dev->facts.build_tag[0] ? attr->mode : 0;

	if (attr == &dev_attr_qsfp_gen.attr)
		return smi_legacy_card(pf_dev)->qsfp.cnt ? attr->mode : 0;

	if (attr == &dev_attr_locate.attr)
		return pf_dev->facts.locates ? attr->mode : 0;

	return attr->mode;
}

static const struct attribute_group smi_common_group = {
	.attrs = smi_common_attrs,
	.is_visible = smi_common_attr_visible,
};

/* ── the cage files ───────────────────────────────────────────────────────
 * One pair per published cage, built at attach as the card sets the count.
 * The raw file is the block verbatim, so an unknown format still reads. */

/* One read serves a whole block, which the contract bounds below any page */
static_assert(SMI_QSFP_BLK_MAX <= PAGE_SIZE, "a cage block fits one read");

/* A read of qsfp<N>_raw or qsfp<N>_state fails with
 * - EAGAIN: the card committed while it was read; read again
 * - ENXIO: the PF is not READY or MISSING_INFO, as the ioctl answers
 * - ENODEV: the card, or its cage plane, is gone */
static ssize_t qsfp_raw_read(struct file *f, struct kobject *kobj,
			     MANGO_COMPAT_BIN_ATTR_ARG *a, char *buf,
			     loff_t pos, size_t count)
{
	const struct smi_qsfp_attrs *at =
		container_of(a, struct smi_qsfp_attrs, raw);
	struct pf_dev_struct *pf_dev;
	int ret;

	if (pos >= a->size)
		return 0;
	pf_dev = qsfp_pf_dev_get(kobj_to_dev(kobj));
	if (IS_ERR(pf_dev))
		return PTR_ERR(pf_dev);

	/* Straight into the reader's buffer, as the span it asked for */
	count = min_t(size_t, count, (size_t)(a->size - pos));
	ret = smi_qsfp_block(pf_dev, at->cage, (uint32_t)pos, buf, count);
	put_pf_dev_entry(pf_dev);

	return ret ? ret : (ssize_t)count;
}

static ssize_t qsfp_state_show(struct device *dev, struct device_attribute *da,
			       char *buf)
{
	struct smi_qsfp_attrs *at =
		container_of(da, struct smi_qsfp_attrs, state);
	struct pf_dev_struct *pf_dev = qsfp_pf_dev_get(dev);
	struct smi_pub_qsfp_cage hdr = { 0 };
	char word[16];
	int ret;

	if (IS_ERR(pf_dev))
		return PTR_ERR(pf_dev);

	/* The baseline into a zeroed header, as attach reads it */
	ret = smi_qsfp_block(pf_dev, at->cage, 0, &hdr, SMI_QSFP_HDR_BASELINE);
	put_pf_dev_entry(pf_dev);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%s\n",
			  smi_qsfp_state_word(hdr.state, word, sizeof(word)));
}

static void qsfp_files_remove(struct pf_dev_struct *pf_dev)
{
	struct smi_legacy_card *card = smi_legacy_card(pf_dev);
	struct device *dev = pf_dev->dev;
	int i;

	if (!card->qsfp_attrs)
		return;

	for (i = 0; i < card->qsfp_attrs_cnt; i++) {
		/* The name is set where the file is created, so it is also the
		 * record of whether there is one to remove. */
		if (card->qsfp_attrs[i].state.attr.name)
			device_remove_file(dev, &card->qsfp_attrs[i].state);
		sysfs_remove_bin_file(&dev->kobj, &card->qsfp_attrs[i].raw);
	}
	card->qsfp_attrs = NULL;
	card->qsfp_attrs_cnt = 0;
}

/* A card whose block format this build does not know keeps the raw file and
 * loses the word: the bytes are opaque either way, the state is not. */
static int qsfp_files_create(struct pf_dev_struct *pf_dev)
{
	struct smi_legacy_card *card = smi_legacy_card(pf_dev);
	const struct smi_qsfp_store *qsfp = &card->qsfp;
	struct device *dev = pf_dev->dev;
	struct smi_qsfp_attrs *at;
	int i, ret;

	if (!qsfp->cnt)
		return 0;

	at = devm_kcalloc(dev, qsfp->cnt, sizeof(*at), GFP_KERNEL);
	if (!at)
		return -ENOMEM;
	card->qsfp_attrs = at;
	card->qsfp_attrs_cnt = qsfp->cnt;

	for (i = 0; i < qsfp->cnt; i++) {
		at[i].cage = (uint8_t)i;

		scnprintf(at[i].raw_name, sizeof(at[i].raw_name), "qsfp%d_raw",
			  i);
		sysfs_bin_attr_init(&at[i].raw);
		at[i].raw.attr.name = at[i].raw_name;
		at[i].raw.attr.mode = 0444;
		/* What the card states now: a later firmware with a longer
		 * block is read whole by the reader that attached to it. */
		at[i].raw.size = qsfp->len[i];
		at[i].raw.read = qsfp_raw_read;

		ret = sysfs_create_bin_file(&dev->kobj, &at[i].raw);
		if (ret)
			goto undo;

		if (!qsfp->decoded)
			continue;

		scnprintf(at[i].state_name, sizeof(at[i].state_name),
			  "qsfp%d_state", i);
		sysfs_attr_init(&at[i].state.attr);
		at[i].state.attr.name = at[i].state_name;
		at[i].state.attr.mode = 0444;
		at[i].state.show = qsfp_state_show;

		ret = device_create_file(dev, &at[i].state);
		if (ret) {
			sysfs_remove_bin_file(&dev->kobj, &at[i].raw);
			goto undo;
		}
	}

	return 0;

undo:
	while (--i >= 0) {
		if (at[i].state.attr.name)
			device_remove_file(dev, &at[i].state);
		sysfs_remove_bin_file(&dev->kobj, &at[i].raw);
	}
	card->qsfp_attrs = NULL;
	card->qsfp_attrs_cnt = 0;

	return ret;
}

/*
 * Register all supported sysfs attributes for a device. */
int register_sysfs(struct pf_dev_struct *pf_dev)
{
	struct device *dev;
	int ret;

	if (!pf_dev || !pf_dev->dev)
		return -EINVAL;

	dev = pf_dev->dev;

	ret = sysfs_create_group(&dev->kobj, &smi_common_group);
	if (ret)
		return ret;

	ret = qsfp_files_create(pf_dev);
	if (ret) {
		sysfs_remove_group(&dev->kobj, &smi_common_group);
		return ret;
	}

	return ret;
}

/*
 * Remove all sysfs attributes for a device. */
void remove_sysfs(struct pf_dev_struct *pf_dev)
{
	if (!pf_dev || !pf_dev->dev)
		return;

	qsfp_files_remove(pf_dev);
	sysfs_remove_group(&pf_dev->dev->kobj, &smi_common_group);
}
