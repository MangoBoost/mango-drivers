/* SPDX-License-Identifier: GPL-2.0 */
/*
 * smi_facts.h - What a settled session states about the card.
 *
 * The session fills it once and every layer above reads it without the
 * session, so it sits below both.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_FACTS_H
#define SMI_FACTS_H

#include <linux/types.h>

/**
 * enum smi_smc_boot_devices - The flash devices a card can boot from.
 * @SMI_SMC_BOOT_DEVICE_PRIMARY: primary boot device
 * @SMI_SMC_BOOT_DEVICE_SECONDARY: secondary boot device
 * @SMI_SMC_BOOT_DEVICE_MAX: how many there are
 */
enum smi_smc_boot_devices {
	SMI_SMC_BOOT_DEVICE_PRIMARY = 0,
	SMI_SMC_BOOT_DEVICE_SECONDARY,

	SMI_SMC_BOOT_DEVICE_MAX
};

/* Room for the card's build name and its terminator. The record states 32
 * bytes and need not terminate them. */
#define SMI_BUILD_TAG_BYTES (33)

/**
 * struct smc_version - stores SMC version information
 * @ver_major: Major version number.
 * @ver_minor: Minor version number.
 * @ver_patch: Patch version number.
 * @ver_date: SMC version date (YYMMDD)
 */
struct smc_version {
	uint8_t ver_major;
	uint8_t ver_minor;
	uint8_t ver_patch;
	uint8_t ver_date[3];
};

/* The one spelling of a firmware version: MAJOR.MINOR.PATCH-YYMMDD. Both the
 * sysfs attribute and the kapi answer with it, so it is written once. */
#define SMI_FW_VERSION_FMT "%hhu.%hhu.%hhu-%02x%02x%02x"
#define SMI_FW_VERSION_ARGS(v)                                                 \
	(v)->ver_major, (v)->ver_minor, (v)->ver_patch, (v)->ver_date[0],      \
		(v)->ver_date[1], (v)->ver_date[2]

/**
 * struct smi_smc_facts - What a settled session states, fixed for its life.
 * @version: the firmware's own MAJOR.MINOR.PATCH-YYMMDD.
 * @build_tag: the name the card's tree gave that build, empty when unstated.
 * @gens_bit: the wire generation it settled on, 0 when none did.
 * @eeprom_bytes: the identity EEPROM's extent.
 * @publishes: whether the card publishes sections in the window unasked.
 * @boot_id: the boot those sections carry; 0 where nothing is published.
 * @locates: whether the card blinks its locate beacon on request.
 * @present: whether a session was set up at all. The others mean nothing
 *	     without it, and it is what a reporter gates on.
 *
 * Taken once, so a reader needs neither the session nor a lock for any of it.
 */
struct smi_smc_facts {
	struct smc_version version;
	char build_tag[SMI_BUILD_TAG_BYTES];
	uint16_t gens_bit;
	uint32_t eeprom_bytes;
	bool publishes;
	uint32_t boot_id;
	bool locates;
	bool present;
};

#endif /* SMI_FACTS_H */
