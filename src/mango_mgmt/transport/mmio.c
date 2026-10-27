// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - MMIO transport
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#if __has_include(<linux/unaligned.h>)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif

#include <mango_compat/platform_device.h>

#include "mmio.h"
#include "dev.h"
#include "regmap.h"
#include "transport.h"
#include "../frontend.h"
#include "mpipe/hw.h"

#define MAX_MMREGS (16)

#define TO_MMIO_DATA(mgmt)                                                     \
	((struct mango_mgmt_mmio_data *)mgmt->transport->data)

struct mango_mgmt_mmio_data {
	struct mango_mgmt_transport transport;
	struct platform_device *pdev;

	void __iomem *mmreg_vaddr[MAX_MMREGS];
	phys_addr_t mmreg_paddr[MAX_MMREGS];
	size_t mmreg_size[MAX_MMREGS];

	/* Interrupt management */
	enum mango_mgmt_irq_mode irq_mode;
	int irq;

	char mgmt_addr[48]; /* "mmio:<base_addr>.<node_name>" */
	u8 mgmt_addr_bin[9]; /* [type:1] [base_phys:8B BE] */
};

/* MPIPE only; irq_setup falls back to polling when the device tree carries
 * no line. */
static const struct mango_mgmt_desc mango_mgmt_desc_mmio_v2 = {
	.regmap = &mango_mgmt_regmap_mmio_v2,
	.side = MGMT_SIDE_SOC,
	.irq_mode = MANGO_MGMT_IRQ_MODE_LEGACY,
	.role = "mgmt",
};

static const struct of_device_id mango_mgmt_mmio_of_ids[] = {
	{ .compatible = COMPAT_PLT_MB_MGMT_V2,
	  .data = &mango_mgmt_desc_mmio_v2 },
	{}
};

/* The module parameter overrides the descriptor's IRQ contract. */
static enum mango_mgmt_irq_mode
mango_mgmt_mmio_get_irq_mode(struct mango_mgmt_dev *mgmt)
{
	if (mango_mgmt_irq_mode >= MANGO_MGMT_IRQ_MODE_NONE &&
	    mango_mgmt_irq_mode <= MANGO_MGMT_IRQ_MODE_POLLING)
		return (enum mango_mgmt_irq_mode)mango_mgmt_irq_mode;

	return mgmt->desc->irq_mode;
}

/* Requested by mpipe while its HW context exists (irq_setup/irq_teardown). */
static irqreturn_t mango_mgmt_mmio_intr_handler(int irq, void *arg)
{
	struct mango_mgmt_mmio_data *data = arg;

	mango_mgmt_mpipe_hw_irq_handler(data->transport.mgmt);
	return IRQ_HANDLED;
}

/* The DT line -> polling (0). Called by mpipe once its HW context exists. */
static int mango_mgmt_mmio_irq_setup(struct mango_mgmt_dev *mgmt)
{
	struct mango_mgmt_mmio_data *data = TO_MMIO_DATA(mgmt);
	struct platform_device *pdev = data->pdev;
	struct device *dev = mgmt->dev;
	int err;

	data->irq_mode = mango_mgmt_mmio_get_irq_mode(mgmt);

	if (data->irq_mode == MANGO_MGMT_IRQ_MODE_POLLING ||
	    data->irq_mode == MANGO_MGMT_IRQ_MODE_NONE) {
		dev_info(dev, "Polling mode");
		data->irq_mode = MANGO_MGMT_IRQ_MODE_POLLING;
		return 0;
	}

	data->irq = platform_get_irq(pdev, 0);
	if (data->irq < 0) {
		dev_info(
			dev,
			"No interrupt found in device tree, using polling mode");
		data->irq_mode = MANGO_MGMT_IRQ_MODE_POLLING;
		return 0;
	}

	err = request_irq(data->irq, mango_mgmt_mmio_intr_handler, IRQF_SHARED,
			  "mango-mgmt-mmio", data);
	if (err < 0) {
		dev_warn(
			dev,
			"Failed to request IRQ %d: %d, falling back to polling",
			data->irq, err);
		data->irq_mode = MANGO_MGMT_IRQ_MODE_POLLING;
		return 0;
	}

	data->irq_mode = MANGO_MGMT_IRQ_MODE_LEGACY;

	dev_info(dev, "MMIO interrupt setup completed (IRQ %d)", data->irq);
	return 1;
}

static void mango_mgmt_mmio_irq_teardown(struct mango_mgmt_dev *mgmt)
{
	struct mango_mgmt_mmio_data *data = TO_MMIO_DATA(mgmt);

	if (data->irq_mode == MANGO_MGMT_IRQ_MODE_LEGACY && data->irq >= 0) {
		free_irq(data->irq, data);
		data->irq_mode = MANGO_MGMT_IRQ_MODE_NONE;
	}
}

static int mango_mgmt_mmio_init(struct mango_mgmt_dev *mgmt)
{
	struct mango_mgmt_mmio_data *data = TO_MMIO_DATA(mgmt);
	struct device *dev = mgmt->dev;
	struct device_node *np;
	int i, err;

	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (err) {
		dev_err(dev, "Failed to set DMA mask: %d", err);
		return err;
	}

	np = dev->of_node;

	/* get reg info from a device tree */
	for (i = 0; i < MAX_MMREGS; i++) {
		struct resource mmreg;

		err = of_address_to_resource(np, i, &mmreg);
		if (err) {
			if (i == 0) {
				dev_err(dev, "Failed to get mmreg info");
				return -EINVAL;
			}
			dev_info(dev, "Found %d MMIO regions", i);
			break;
		}

		/* Validate resource range */
		if (resource_size(&mmreg) == 0) {
			dev_err(dev, "Invalid resource size for region %d", i);
			return -EINVAL;
		}

		dev_info(dev, "MMIO region %d: 0x%llx-0x%llx (size: 0x%llx)", i,
			 mmreg.start, mmreg.end, resource_size(&mmreg));

		/* no need to explicitly release them */
		data->mmreg_vaddr[i] = devm_ioremap_resource(dev, &mmreg);
		if (IS_ERR(data->mmreg_vaddr[i])) {
			dev_err(dev, "Failed to remap %d-th mmreg resource", i);
			return PTR_ERR(data->mmreg_vaddr[i]);
		}
		data->mmreg_paddr[i] = mmreg.start;
		data->mmreg_size[i] = resource_size(&mmreg);

		dev_info(dev, "MMIO region %d mapped to virtual address %p", i,
			 data->mmreg_vaddr[i]);
	}

	return mango_mgmt_regmap_validate(mgmt);
}

static void mango_mgmt_mmio_fini(struct mango_mgmt_dev *mgmt)
{
	struct mango_mgmt_mmio_data *data = TO_MMIO_DATA(mgmt);
	int i;

	for (i = 0; i < MAX_MMREGS; i++) {
		data->mmreg_vaddr[i] = NULL;
		data->mmreg_paddr[i] = 0;
	}
}

static int mango_mgmt_mmio_region_get(struct mango_mgmt_dev *mgmt, u32 idx,
				      struct mango_mgmt_region *region)
{
	struct mango_mgmt_mmio_data *data = TO_MMIO_DATA(mgmt);

	if (idx >= MAX_MMREGS || !data->mmreg_vaddr[idx])
		return -ENODEV;

	region->base = data->mmreg_vaddr[idx];
	region->phys = data->mmreg_paddr[idx];
	region->size = data->mmreg_size[idx];
	return 0;
}

static const char *mango_mgmt_mmio_get_mgmt_addr(struct mango_mgmt_dev *mgmt)
{
	struct mango_mgmt_mmio_data *data = TO_MMIO_DATA(mgmt);

	return data->mgmt_addr;
}

static const u8 *mango_mgmt_mmio_get_mgmt_addr_bin(struct mango_mgmt_dev *mgmt,
						   int *len)
{
	struct mango_mgmt_mmio_data *data = TO_MMIO_DATA(mgmt);

	*len = sizeof(data->mgmt_addr_bin);
	return data->mgmt_addr_bin;
}

static int mango_mgmt_mmio_probe(struct platform_device *pdev)
{
	struct mango_mgmt_dev *mgmt;
	struct mango_mgmt_mmio_data *data;
	struct mango_mgmt_transport *transport;
	int err;
	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->pdev = pdev;
	data->irq = -1;

	if (strlen(dev_name(&pdev->dev)) > sizeof(data->mgmt_addr) - 6)
		dev_warn(&pdev->dev,
			 "mgmt_addr truncated: dev_name too long (%zu)\n",
			 strlen(dev_name(&pdev->dev)));
	snprintf(data->mgmt_addr, sizeof(data->mgmt_addr), "mmio:%s",
		 dev_name(&pdev->dev));

	/* Binary encoding: [0x02] [base_phys:8B BE] */
	{
		struct resource *res =
			platform_get_resource(pdev, IORESOURCE_MEM, 0);

		data->mgmt_addr_bin[0] = 0x02;
		put_unaligned_be64(res ? res->start : 0,
				   &data->mgmt_addr_bin[1]);
	}

	transport = &data->transport;
	transport->data = data;
	transport->init = mango_mgmt_mmio_init;
	transport->fini = mango_mgmt_mmio_fini;
	transport->region_get = mango_mgmt_mmio_region_get;
	transport->irq_setup = mango_mgmt_mmio_irq_setup;
	transport->irq_teardown = mango_mgmt_mmio_irq_teardown;
	transport->get_mgmt_addr = mango_mgmt_mmio_get_mgmt_addr;
	transport->get_mgmt_addr_bin = mango_mgmt_mmio_get_mgmt_addr_bin;

	mgmt = mango_mgmt_dev_alloc(&pdev->dev, transport,
				    device_get_match_data(&pdev->dev));
	if (IS_ERR(mgmt)) {
		err = PTR_ERR(mgmt);
		goto free_data;
	}

	err = mango_mgmt_frontend_init_device(mgmt);
	if (err) {
		dev_err(&pdev->dev,
			"Failed to initialize frontend for MMIO device: %d",
			err);
		goto free_mgmt;
	}
	dev_info(&pdev->dev, "MMIO device frontend initialized");

	platform_set_drvdata(pdev, mgmt);
	dev_info(&pdev->dev, "MMIO device driver initialized");
	return 0;

free_mgmt:
	mango_mgmt_dev_free(mgmt);
free_data:
	kfree(data);
	return err;
}

static MANGO_COMPAT_PLATFORM_REMOVE_RET
mango_mgmt_mmio_remove(struct platform_device *pdev)
{
	struct mango_mgmt_dev *mgmt = platform_get_drvdata(pdev);
	struct mango_mgmt_mmio_data *data = TO_MMIO_DATA(mgmt);

	mango_mgmt_frontend_cleanup_device(mgmt);
	mango_mgmt_dev_free(mgmt);

	kfree(data);

	return MANGO_COMPAT_PLATFORM_REMOVE_OK;
}

static void mango_mgmt_mmio_shutdown(struct platform_device *pdev)
{
	struct mango_mgmt_dev *mgmt = platform_get_drvdata(pdev);

	if (!mgmt)
		return;

	dev_info(&pdev->dev, "Shutting down MangoBoost MMIO Management device");

	mango_mgmt_frontend_quiesce(mgmt);

	dev_info(&pdev->dev,
		 "MangoBoost MMIO Management device shutdown completed");
}

static struct platform_driver
	mango_mgmt_mmio_driver = { .probe = mango_mgmt_mmio_probe,
				   .remove = mango_mgmt_mmio_remove,
				   .shutdown = mango_mgmt_mmio_shutdown,
				   .driver = {
					   .name = MANGO_MGMT_NAME,
					   .owner = THIS_MODULE,
					   .of_match_table =
						   mango_mgmt_mmio_of_ids,
				   },
  };

static struct platform_driver *const plt_drivers[] = {
	&mango_mgmt_mmio_driver,
};

int mango_mgmt_mmio_register()
{
	return platform_register_drivers(plt_drivers, ARRAY_SIZE(plt_drivers));
}

void mango_mgmt_mmio_unregister()
{
	platform_unregister_drivers(plt_drivers, ARRAY_SIZE(plt_drivers));
}
