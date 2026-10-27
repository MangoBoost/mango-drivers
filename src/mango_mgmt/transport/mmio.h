// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - MMIO transport
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_TRANSPORT_MMIO_H_
#define _MANGO_MGMT_TRANSPORT_MMIO_H_

#define COMPAT_PLT_MB_MGMT_V2 "mangoboost,mgmt-v2"

int mango_mgmt_mmio_register(void);
void mango_mgmt_mmio_unregister(void);

#endif
