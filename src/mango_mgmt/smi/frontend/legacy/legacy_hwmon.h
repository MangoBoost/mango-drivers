// SPDX-License-Identifier: GPL-2.0-only
/*
 * legacy_hwmon.h - The legacy hwmon surface, frozen.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_LEGACY_HWMON_H
#define SMI_LEGACY_HWMON_H

#include <linux/hwmon.h>
#include <linux/types.h>

#include "smi_hwmon.h" /* enum smi_sensor_attribute */

struct device;
struct pf_dev_struct;

/**
 * smi_hwmon_v1_register() - Present the legacy surface.
 * @dev: The PCI device structure.
 * @pf_dev: The PCI device data structure.
 *
 * Frozen: the chip name, the attribute set and what each attribute means are
 * what released tools parse, so they stay as they shipped.
 *
 * Return: 0 or error.
 */
int smi_hwmon_v1_register(struct device *dev, struct pf_dev_struct *pf_dev);

/**
 * smi_hwmon_v1_chan() - The record a channel of the frozen surface names.
 *
 * Return: 0, or -ENOENT for a type or channel it does not serve.
 */
int smi_hwmon_v1_chan(struct pf_dev_struct *pf_dev,
		      enum hwmon_sensor_types type, int channel, uint8_t *rid,
		      uint8_t *id);

/* The words the legacy UAPI spells a record's status in; "" for a status it
 * has none for. */
const char *smi_legacy_status_name(int status);

/**
 * read_sensor_val() - Read one attribute of one channel, in hwmon's units.
 * @pf_dev: PCI device data structure.
 * @type: Hwmon sensor type.
 * @smi_attr: What is wanted of the record -- the meaning, not a file name.
 * @channel: Hwmon channel.
 * @val: Variable to store sensor value.
 * @status: The record's status word (optional).
 * @fresh: Variable to store cache status for this value (optional).
 *
 * Return: 0 or negative error code.
 */
int read_sensor_val(struct pf_dev_struct *pf_dev, enum hwmon_sensor_types type,
		    enum smi_sensor_attribute smi_attr, int channel, long *val,
		    const char **status, bool *fresh);

#endif /* SMI_LEGACY_HWMON_H */
