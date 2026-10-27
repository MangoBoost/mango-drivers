// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_SYSFS_H_
#define _MANGO_MGMT_SYSFS_H_

#include "dev.h"

int mango_mgmt_sysfs_setup(struct mango_mgmt_dev *);
void mango_mgmt_sysfs_teardown(struct mango_mgmt_dev *);

int mango_mgmt_sysfs_register(void);
void mango_mgmt_sysfs_unregister(void);

#endif
