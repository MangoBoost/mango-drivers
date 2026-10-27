// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - Transport layer
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include "transport.h"
#include "pcie.h"
#include "mmio.h"
#include "smi/smi_platform.h"
#include "dev.h"

int mango_mgmt_transport_register(void)
{
	int err;

	err = mango_mgmt_pcie_register();
	if (err) {
		pr_err("Failed to register the MGMT PCI driver.\n");
		return err;
	}

	err = mango_mgmt_mmio_register();
	if (err) {
		pr_err("Failed to register the MGMT MMIO driver.\n");
		goto unregister_pcie;
	}

	err = mango_mgmt_smi_pcie_register();
	if (err) {
		pr_err("Failed to register the SMI PCI driver.\n");
		goto unregister_mmio;
	}

	err = mango_mgmt_smi_platform_register();
	if (err) {
		pr_err("Failed to register the SMI platform driver.\n");
		goto unregister_smi_pcie;
	}

	return 0;

unregister_smi_pcie:
	mango_mgmt_smi_pcie_unregister();
unregister_mmio:
	mango_mgmt_mmio_unregister();
unregister_pcie:
	mango_mgmt_pcie_unregister();

	return err;
}

void mango_mgmt_transport_unregister(void)
{
	mango_mgmt_smi_platform_unregister();
	mango_mgmt_smi_pcie_unregister();
	mango_mgmt_mmio_unregister();
	mango_mgmt_pcie_unregister();
}

bool mango_mgmt_transport_is_soc_device(struct mango_mgmt_dev *mgmt_dev)
{
	return mgmt_dev && mgmt_dev->desc &&
	       mgmt_dev->desc->side == MGMT_SIDE_SOC;
}
