// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - PCIe transport
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_TRANSPORT_PCIE_H_
#define _MANGO_MGMT_TRANSPORT_PCIE_H_

#include <linux/types.h>
#include <linux/pci.h>

#include "hw_contract.h"

#ifndef PCI_VENDOR_ID_MANGOBOOST
#define PCI_VENDOR_ID_MANGOBOOST MANGO_HW_PCI_VENDOR_ID
#endif

struct mango_mgmt_dev;
struct mango_mgmt_desc;

int mango_mgmt_pcie_register(void);
void mango_mgmt_pcie_unregister(void);
int mango_mgmt_smi_pcie_register(void);
void mango_mgmt_smi_pcie_unregister(void);

/* The table row @pdev matches, bound or not; NULL when none. */
const struct mango_mgmt_desc *mango_mgmt_pcie_match(struct pci_dev *pdev);

#endif
