/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MangoBoost Management Module - hardware contract
 *
 * PCI IDs and where each IP block sits on the card, as region index (PCIe BAR
 * or MMIO region), offset and length.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_HW_CONTRACT_H_
#define _MANGO_MGMT_HW_CONTRACT_H_

#define MANGO_HW_PCI_VENDOR_ID 0x1F52
#define MANGO_HW_PCI_DEVICE_MGMT_V2 0x2100

/* Management function (0x2100): the identification block (SYSMAN), the host
 * MPIPE bank, GCQ and its payload in BAR2. */
#define MANGO_HW_MGMT_V2_IDX 2
#define MANGO_HW_MGMT_V2_SYSMAN_OFF 0x00000000
#define MANGO_HW_MGMT_V2_SYSMAN_LEN 0x1000
#define MANGO_HW_MGMT_V2_MPIPE_OFF 0x00010000
#define MANGO_HW_MGMT_V2_MPIPE_LEN 0x1000
#define MANGO_HW_MGMT_V2_GCQ_OFF 0x00012000
#define MANGO_HW_MGMT_V2_GCQ_LEN 0x1000
#define MANGO_HW_MGMT_V2_GCQ_PAYLOAD_OFF 0x08000000
#define MANGO_HW_MGMT_V2_GCQ_PAYLOAD_LEN 0x08000000

/* SoC side (mangoboost,mgmt-v2): MMIO region 0 is the window the host sees
 * as BAR2, so SYSMAN and MPIPE sit at the host offsets above; MPIPE with
 * both banks, as this side's bank is at +0x1000. */
#define MANGO_HW_MMIO_IDX 0
#define MANGO_HW_MMIO_V2_MPIPE_LEN 0x2000

#endif
