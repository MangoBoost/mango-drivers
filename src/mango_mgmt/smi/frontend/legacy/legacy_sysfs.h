// SPDX-License-Identifier: GPL-2.0-only
/*
 * legacy_sysfs.h - The legacy surface's files on the card's bus device.
 *
 * Copyright (c) 2023-present Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_LEGACY_SYSFS_H
#define SMI_LEGACY_SYSFS_H

#include <linux/sysfs.h>
#include <linux/device.h>

struct pf_dev_struct;

/**
 * struct smi_qsfp_attrs - the two files one cage presents
 * @raw: the block itself, read whole; the bytes are not interpreted
 * @state: the word this driver has for what that cage is doing
 * @raw_name: storage for "qsfpN_raw", which the attribute points at
 * @state_name: storage for "qsfpN_state"
 * @cage: which cage these belong to, recovered from either attribute
 *
 * One array per card, sized by what the card publishes, so a card with three
 * cages presents three pairs without anything here being widened.
 */
struct smi_qsfp_attrs {
	struct bin_attribute raw;
	struct device_attribute state;
	char raw_name[16];
	char state_name[18];
	uint8_t cage;
};

/**
 * register_sysfs() - Registers the sysfs subsystem.
 * @pf_dev: The card whose files these are.
 *
 * Return: 0 or negative error.
 */
int register_sysfs(struct pf_dev_struct *pf_dev);

/**
 * remove_sysfs() - Remove all previously registered sysfs attributes.
 * @pf_dev: The card given to register_sysfs(), not looked up: it has left the
 *	    registry; a lookup would strand files the next attach collides with.
 */
void remove_sysfs(struct pf_dev_struct *pf_dev);

#endif /* SMI_LEGACY_SYSFS_H */
