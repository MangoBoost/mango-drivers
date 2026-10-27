// SPDX-License-Identifier: GPL-2.0-only
/*
 * smi_rpmsg.c - RPMsg driver for communicating with OpenAMP firmware
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved. */

#include <linux/module.h>

#include "contract/smi/smi_abi.h"

#include "smi_rpmsg_glue.h"

#ifdef CONFIG_OF
#include <linux/rpmsg.h>

DECLARE_COMPLETION(rpmsg_ready_comp);

static struct rpmsg_device *g_rpdev;

struct smi_rpmsg_data {
	struct rpmsg_device *rpdev;
};

static int smi_rpmsg_cb(struct rpmsg_device *rpdev, void *data, int len,
			void *priv, u32 src)
{
	dev_dbg(&rpdev->dev, "smi_rpmsg_cb(len=%d): %*ph\n", len, len,
		(char *)data);

	fw_if_rpmsg_rx_push((const uint8_t *)data, (uint32_t)len);

	return 0;
}

static int smi_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct smi_rpmsg_data *rdata;

	dev_info(&rpdev->dev, "[RPMsg] Channel bound... src: 0x%x, dst: 0x%x\n",
		 rpdev->src, rpdev->dst);

	rdata = devm_kzalloc(&rpdev->dev, sizeof(*rdata), GFP_KERNEL);
	if (!rdata)
		return -ENOMEM;

	rdata->rpdev = rpdev;
	dev_set_drvdata(&rpdev->dev, rdata);

	g_rpdev = rpdev;

	complete(&rpmsg_ready_comp);

	return 0;
}

static void smi_rpmsg_remove(struct rpmsg_device *rpdev)
{
	dev_info(&rpdev->dev, "[RPMsg] Channel unbound\n");
	g_rpdev = NULL;
	smi_rpmsg_unregister_channel();
}

/* A name missing here strands the lane silently: the bus advertises one
 * nothing matches, and the probe times out with no reason in it. */
static struct rpmsg_device_id smi_rpmsg_id_table[] = {
	{ .name = SMI_RPMSG_EPT_NAME_SMID },
	{},
};
MODULE_DEVICE_TABLE(rpmsg, smi_rpmsg_id_table);

static struct rpmsg_driver smi_rpmsg_driver = {
	.drv.name = "smi_rpmsg_drv",
	.id_table = smi_rpmsg_id_table,
	.probe = smi_rpmsg_probe,
	.callback = smi_rpmsg_cb,
	.remove = smi_rpmsg_remove,
};

int smi_rpmsg_driver_init(void)
{
	return register_rpmsg_driver(&smi_rpmsg_driver);
}

void smi_rpmsg_driver_exit(void)
{
	unregister_rpmsg_driver(&smi_rpmsg_driver);
}

struct rpmsg_device *smi_rpmsg_get_current_rpdev(void)
{
	return g_rpdev;
}

#else /* !CONFIG_OF */

DECLARE_COMPLETION(rpmsg_ready_comp);

struct rpmsg_device *smi_rpmsg_get_current_rpdev(void)
{
	return NULL;
}

int smi_rpmsg_driver_init(void)
{
	return 0;
}
void smi_rpmsg_driver_exit(void)
{
}

#endif /* CONFIG_OF */
