// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - SMI platform glue (SoC)
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_SMI_PLATFORM_H_
#define _MANGO_MGMT_SMI_PLATFORM_H_

#define COMPAT_PLT_MB_SMI "mangoboost,smi"

int mango_mgmt_smi_platform_register(void);
void mango_mgmt_smi_platform_unregister(void);

#endif
