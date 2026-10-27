/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MangoBoost Management Module - the SMI surface's name
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_SMI_NAME_H_
#define _MANGO_MGMT_SMI_NAME_H_

struct attribute_group;

/* Names the SMI class, cdev, drivers and log prefix. */
#define SMI_NAME "mango_smi"

/* Driver-wide sysfs: devices map, version, debug switch. */
extern const struct attribute_group *smi_driver_groups[];

#endif
