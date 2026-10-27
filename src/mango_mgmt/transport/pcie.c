// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - PCIe transport
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>
#if __has_include(<linux/unaligned.h>)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif

#include "dev.h"
#include "mango_mgmt_api.h"
#include "regmap.h"
#include "smi/smi_name.h"
#include "mpipe/hw.h"
#include "pcie.h"
#include "transport/transport.h"
#include "../frontend.h"

#define MAX_PCIE_BAR (6)

#define TO_PCIE_DATA(mgmt)                                                     \
	((struct mango_mgmt_pcie_data *)mgmt->transport->data)

/* Largest memory read request the GCQ command plane serves. */
#define MANGO_MGMT_GCQ_MAX_READ_REQUEST 512

/* MPIPE and SMI on one function. The function wires no MPIPE interrupt,
 * so MPIPE polls. */
static const struct mango_mgmt_desc mango_mgmt_desc_mgmt_v2 = {
	.regmap = &mango_mgmt_regmap_pcie_v2,
	.side = MGMT_SIDE_HOST,
	.smi = MGMT_SMI_GCQ,
	.irq_mode = MANGO_MGMT_IRQ_MODE_POLLING,
	.role = "mgmt",
};

static const struct pci_device_id mango_mgmt_pcie_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_MANGOBOOST, MANGO_HW_PCI_DEVICE_MGMT_V2),
	  .driver_data = (kernel_ulong_t)&mango_mgmt_desc_mgmt_v2 },
	{}
};
MODULE_DEVICE_TABLE(pci, mango_mgmt_pcie_table);

/* Binds nothing: registered only for its driver attributes, which list every
 * SMI device under /sys/bus/pci/drivers/mango_smi, where userspace looks. */
static const struct pci_device_id mango_smi_pcie_table[] = {
	{}
};

struct mango_mgmt_pcie_data {
	struct mango_mgmt_transport transport;
	struct pci_dev *pdev;

	void __iomem *bar[MAX_PCIE_BAR];
	u64 bar_addr[MAX_PCIE_BAR];
	size_t bar_size[MAX_PCIE_BAR];

	char mgmt_addr[18];
	u8 mgmt_addr_bin[5]; /* [type:1] [domain:2] [bus:1] [devfn:1] */
};

/* MPIPE polls on this function: nothing to wire. */
static int mango_mgmt_pcie_irq_setup(struct mango_mgmt_dev *mgmt)
{
	dev_info(mgmt->dev, "Polling mode");
	return 0;
}

static void mango_mgmt_pcie_irq_teardown(struct mango_mgmt_dev *mgmt)
{
}

static void mango_mgmt_pcie_unmap_bars(struct mango_mgmt_pcie_data *data)
{
	int i;

	for (i = 0; i < MAX_PCIE_BAR; i++) {
		if (!data->bar[i])
			continue;
		pci_iounmap(data->pdev, data->bar[i]);
		data->bar[i] = NULL;
	}
}

static int mango_mgmt_pcie_init(struct mango_mgmt_dev *mgmt)
{
	struct mango_mgmt_pcie_data *data = TO_PCIE_DATA(mgmt);
	struct pci_dev *pdev = data->pdev;
	struct device *dev = mgmt->dev;
	unsigned long bars = mango_mgmt_regmap_region_mask(mgmt->desc->regmap);
	int i, err;

	err = pci_enable_device(pdev);
	if (err) {
		dev_err(dev, "Failed to enable PCIe device: %d", err);
		return err;
	}

	err = pci_request_regions(pdev, MANGO_MGMT_NAME);
	if (err) {
		dev_err(dev, "Failed to request PCI regions: %d", err);
		goto disable_device;
	}

	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (err) {
		dev_err(dev, "Failed to set DMA mask: %d", err);
		goto release_regions;
	}

	pci_set_master(pdev);

	/* Only the BARs the card's regmap places blocks in. */
	for_each_set_bit (i, &bars, MAX_PCIE_BAR) {
		void __iomem *regs;

		if (!(pci_resource_flags(pdev, i) & IORESOURCE_MEM) ||
		    !pci_resource_len(pdev, i)) {
			dev_err(dev, "BAR%d not assigned", i);
			err = -ENODEV;
			goto unmap_bars;
		}

		regs = pci_iomap(pdev, i, 0);
		if (!regs) {
			dev_err(dev, "BAR%d map failed", i);
			err = -ENOMEM;
			goto unmap_bars;
		}

		data->bar[i] = regs;
		data->bar_addr[i] = pci_resource_start(pdev, i);
		data->bar_size[i] = pci_resource_len(pdev, i);
		dev_info(dev, "BAR[%d]: 0x%llx (size: 0x%zx)", i,
			 data->bar_addr[i], data->bar_size[i]);
	}

	err = mango_mgmt_regmap_validate(mgmt);
	if (err)
		goto unmap_bars;

	/* The cap is for the GCQ plane's payload reads, so the link the
	 * descriptor names decides which functions get it. */
	if (mgmt->desc->smi == MGMT_SMI_GCQ &&
	    pcie_get_readrq(pdev) > MANGO_MGMT_GCQ_MAX_READ_REQUEST) {
		err = pcie_set_readrq(pdev, MANGO_MGMT_GCQ_MAX_READ_REQUEST);
		if (err) {
			dev_err(dev, "Failed to cap the read request: %d", err);
			goto unmap_bars;
		}
	}

	/* Snapshot config, cap included, so the AER slot_reset handler can
	 * restore it (aer_root_reset does not restore child config). */
	pci_save_state(pdev);

	return 0;

unmap_bars:
	mango_mgmt_pcie_unmap_bars(data);
release_regions:
	pci_release_regions(pdev);
disable_device:
	pci_disable_device(pdev);
	return err;
}

static void mango_mgmt_pcie_fini(struct mango_mgmt_dev *mgmt)
{
	struct mango_mgmt_pcie_data *data = TO_PCIE_DATA(mgmt);

	mango_mgmt_pcie_unmap_bars(data);
	pci_release_regions(data->pdev);
	pci_disable_device(data->pdev);
}

static int mango_mgmt_pcie_region_get(struct mango_mgmt_dev *mgmt, u32 idx,
				      struct mango_mgmt_region *region)
{
	struct mango_mgmt_pcie_data *data = TO_PCIE_DATA(mgmt);

	if (idx >= MAX_PCIE_BAR || !data->bar[idx])
		return -ENODEV;

	region->base = data->bar[idx];
	region->phys = data->bar_addr[idx];
	region->size = data->bar_size[idx];
	return 0;
}

static const char *
mango_mgmt_pcie_get_mgmt_addr(struct mango_mgmt_dev *mgmt_dev);
static const u8 *
mango_mgmt_pcie_get_mgmt_addr_bin(struct mango_mgmt_dev *mgmt_dev, int *len);

static int mango_mgmt_pcie_probe(struct pci_dev *pdev,
				 const struct pci_device_id *id)
{
	struct mango_mgmt_dev *mgmt;
	struct mango_mgmt_pcie_data *data;
	struct mango_mgmt_transport *transport;
	int err;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->pdev = pdev;
	snprintf(data->mgmt_addr, sizeof(data->mgmt_addr),
		 "pcie:%04x:%02x:%02x.%x", pci_domain_nr(pdev->bus),
		 pdev->bus->number, PCI_SLOT(pdev->devfn),
		 PCI_FUNC(pdev->devfn));

	/* Binary encoding: [0x01] [domain:2B BE] [bus:1B] [devfn:1B] */
	data->mgmt_addr_bin[0] = 0x01;
	put_unaligned_be16(pci_domain_nr(pdev->bus), &data->mgmt_addr_bin[1]);
	data->mgmt_addr_bin[3] = pdev->bus->number;
	data->mgmt_addr_bin[4] = pdev->devfn;

	transport = &data->transport;
	transport->data = data;
	transport->init = mango_mgmt_pcie_init;
	transport->fini = mango_mgmt_pcie_fini;
	transport->region_get = mango_mgmt_pcie_region_get;
	transport->irq_setup = mango_mgmt_pcie_irq_setup;
	transport->irq_teardown = mango_mgmt_pcie_irq_teardown;
	transport->get_mgmt_addr = mango_mgmt_pcie_get_mgmt_addr;
	transport->get_mgmt_addr_bin = mango_mgmt_pcie_get_mgmt_addr_bin;

	mgmt = mango_mgmt_dev_alloc(
		&pdev->dev, transport,
		(const struct mango_mgmt_desc *)id->driver_data);
	if (IS_ERR(mgmt)) {
		err = PTR_ERR(mgmt);
		goto free_data;
	}

	pci_set_drvdata(pdev, mgmt);

	err = mango_mgmt_frontend_init_device(mgmt);
	if (err) {
		dev_err(&pdev->dev, "Failed to initialize frontend: %d", err);
		goto free_mgmt;
	}

	return 0;

free_mgmt:
	mango_mgmt_dev_free(mgmt);
	pci_set_drvdata(pdev, NULL);
free_data:
	kfree(data);
	return err;
}

static void mango_mgmt_pcie_remove(struct pci_dev *pdev)
{
	struct mango_mgmt_dev *mgmt = pci_get_drvdata(pdev);
	struct mango_mgmt_transport *transport = mgmt->transport;
	struct mango_mgmt_pcie_data *data = transport ? transport->data : NULL;

	/* Clear driver data first to prevent double cleanup */
	pci_set_drvdata(pdev, NULL);

	mango_mgmt_frontend_cleanup_device(mgmt);
	mango_mgmt_dev_free(mgmt);

	kfree(data);
}

static void mango_mgmt_pcie_shutdown(struct pci_dev *pdev)
{
	struct mango_mgmt_dev *mgmt = pci_get_drvdata(pdev);

	if (!mgmt)
		return;

	dev_info(&pdev->dev, "Shutting down MangoBoost Management device");

	mango_mgmt_frontend_quiesce(mgmt);
	pci_clear_master(pdev);

	dev_info(&pdev->dev, "MangoBoost Management device shutdown completed");
}

const struct mango_mgmt_desc *mango_mgmt_pcie_match(struct pci_dev *pdev)
{
	const struct pci_device_id *id =
		pci_match_id(mango_mgmt_pcie_table, pdev);

	return id ? (const struct mango_mgmt_desc *)id->driver_data : NULL;
}

bool mango_mgmt_pcie_is_mgmt(struct pci_dev *pdev)
{
	const struct mango_mgmt_desc *desc = mango_mgmt_pcie_match(pdev);

	return desc && !strcmp(desc->role, "mgmt");
}
EXPORT_SYMBOL_GPL(mango_mgmt_pcie_is_mgmt);

/* No supported PCIe function sits on the SoC side. */
bool mango_mgmt_pcie_is_soc(struct pci_dev *pdev)
{
	return false;
}
EXPORT_SYMBOL_GPL(mango_mgmt_pcie_is_soc);

static const char *
mango_mgmt_pcie_get_mgmt_addr(struct mango_mgmt_dev *mgmt_dev)
{
	struct mango_mgmt_pcie_data *data = TO_PCIE_DATA(mgmt_dev);
	return data->mgmt_addr;
}

static const u8 *
mango_mgmt_pcie_get_mgmt_addr_bin(struct mango_mgmt_dev *mgmt_dev, int *len)
{
	struct mango_mgmt_pcie_data *data = TO_PCIE_DATA(mgmt_dev);

	*len = sizeof(data->mgmt_addr_bin);
	return data->mgmt_addr_bin;
}

/*
 * AER handlers. mgmt shares its bridge with a data PF; without an
 * error_detected callback the PCI core votes NO_AER_DRIVER for this function
 * and disconnects the whole subtree on any AER. mgmt never drives the reset
 * decision (the data PF does), so it quiesces and votes CAN_RECOVER, then
 * restores config on slot_reset and re-attaches on resume.
 */
static pci_ers_result_t
mango_mgmt_pcie_error_detected(struct pci_dev *pdev, pci_channel_state_t state)
{
	struct mango_mgmt_dev *mgmt = pci_get_drvdata(pdev);

	dev_warn(&pdev->dev, "mgmt: PCIe error detected (state %u)", state);

	/* A dead function is the core's to remove; the teardown below would
	 * only write config space at a device that cannot answer. */
	if (state == pci_channel_io_perm_failure)
		return PCI_ERS_RESULT_DISCONNECT;

	if (mgmt) {
		mango_mgmt_frontend_quiesce(mgmt);
		pci_clear_master(pdev);
	}
	return PCI_ERS_RESULT_CAN_RECOVER;
}

static pci_ers_result_t mango_mgmt_pcie_slot_reset(struct pci_dev *pdev)
{
	if (pci_enable_device(pdev)) {
		dev_err(&pdev->dev, "mgmt: slot_reset: failed to re-enable");
		return PCI_ERS_RESULT_DISCONNECT;
	}
	pci_restore_state(pdev);
	pci_save_state(pdev); /* stay armed for a future reset */
	return PCI_ERS_RESULT_RECOVERED;
}

static void mango_mgmt_pcie_resume(struct pci_dev *pdev)
{
	struct mango_mgmt_dev *mgmt = pci_get_drvdata(pdev);

	if (!mgmt)
		return;

	dev_info(&pdev->dev, "mgmt: resuming after AER reset");
	pci_set_master(pdev);
	if (mango_mgmt_frontend_resume(mgmt))
		dev_err(&pdev->dev, "mgmt: MPIPE did not come back");
}

static const struct pci_error_handlers mango_mgmt_pcie_err_handlers = {
	.error_detected = mango_mgmt_pcie_error_detected,
	.slot_reset = mango_mgmt_pcie_slot_reset,
	.resume = mango_mgmt_pcie_resume,
};

static struct pci_driver mango_mgmt_pcie_driver = {
	.name = MANGO_MGMT_NAME,
	.id_table = mango_mgmt_pcie_table,
	.probe = mango_mgmt_pcie_probe,
	.remove = mango_mgmt_pcie_remove,
	.shutdown = mango_mgmt_pcie_shutdown,
	.err_handler = &mango_mgmt_pcie_err_handlers,
	.groups = smi_driver_groups,
};

static struct pci_driver mango_smi_pcie_driver = {
	.name = SMI_NAME,
	.id_table = mango_smi_pcie_table,
	.groups = smi_driver_groups,
};

int mango_mgmt_pcie_register(void)
{
	return pci_register_driver(&mango_mgmt_pcie_driver);
}

void mango_mgmt_pcie_unregister(void)
{
	pci_unregister_driver(&mango_mgmt_pcie_driver);
}

int mango_mgmt_smi_pcie_register(void)
{
	return pci_register_driver(&mango_smi_pcie_driver);
}

void mango_mgmt_smi_pcie_unregister(void)
{
	pci_unregister_driver(&mango_smi_pcie_driver);
}
