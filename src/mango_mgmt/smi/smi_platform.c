// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - SMI platform glue (SoC)
 *
 * The `mangoboost,smi` node: SMI over RPMsg, no register window. Bound as
 * SMI_NAME (a shipped CLI reads /sys/bus/platform/drivers/mango_smi);
 * registered by the management transport like every device driver here.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <mango_compat/platform_device.h>

#include "smi_platform.h"
#include "smi_name.h"
#include "smi_top.h"

#ifdef CONFIG_OF
#include <linux/remoteproc.h>

#include "backend/transport/smi_rpmsg_glue.h"

#define DT_PROP_REMOTEPROC "remoteproc"

struct smi_platform_glue {
	struct rproc *rproc; /* optional: for kernel-driven remoteproc start */
	struct pf_dev_struct *pf_dev;
};

static int smi_platform_probe(struct platform_device *pdev)
{
	struct smi_platform_glue *glue;
	struct device_node *rproc_np;
	phandle ph;
	int ret;
	struct smi_init_data init = { 0 };

	dev_info(&pdev->dev, "[Platform] Probing SMI device...\n");

	glue = devm_kzalloc(&pdev->dev, sizeof(*glue), GFP_KERNEL);
	if (!glue)
		return -ENOMEM;

	/* Start remoteproc from DT reference so no userspace "echo start" is needed */
	rproc_np = of_parse_phandle(pdev->dev.of_node, DT_PROP_REMOTEPROC, 0);
	if (rproc_np) {
		ph = rproc_np->phandle;
		of_node_put(rproc_np);

		glue->rproc = rproc_get_by_phandle(ph);

		if (glue->rproc) {
			reinit_completion(&rpmsg_ready_comp);
			ret = rproc_boot(glue->rproc);
			if (ret) {
				dev_err(&pdev->dev, "rproc_boot failed (%d)\n",
					ret);
				goto err_rproc;
			}
			dev_info(&pdev->dev,
				 "remoteproc started from kernel\n");
		} else {
			dev_info(&pdev->dev,
				 "remoteproc not ready yet (defer)\n");
			return -EPROBE_DEFER;
		}
	}

	init.transport_type = FW_IF_TYPE_RPMSG;
	init.msg_view = NULL;

	glue->pf_dev = smi_device_init(&pdev->dev, &init);
	if (IS_ERR(glue->pf_dev)) {
		ret = PTR_ERR(glue->pf_dev);
		glue->pf_dev = NULL;
		dev_err(&pdev->dev, "Failed to initialize SMI core (%d)\n",
			ret);
		goto err_rproc;
	}
	platform_set_drvdata(pdev, glue);

	dev_info(&pdev->dev, "SMI probe complete\n");
	return 0;

err_rproc:
	if (glue->rproc) {
		rproc_detach(
			glue->rproc); /* undo rproc_boot, keep RPU running */
		rproc_put(glue->rproc); /* undo rproc_get_by_phandle */
		glue->rproc = NULL;
	}
	return ret;
}

static MANGO_COMPAT_PLATFORM_REMOVE_RET
smi_platform_remove(struct platform_device *pdev)
{
	struct smi_platform_glue *glue = platform_get_drvdata(pdev);

	dev_info(&pdev->dev, "Removing SMI device\n");

	if (glue->pf_dev) {
		struct pf_dev_struct *pf_dev = glue->pf_dev;

		/* smi_device_remove() frees the handle; unpublish first. */
		glue->pf_dev = NULL;
		smi_device_remove(pf_dev);
	}

	if (glue->rproc) {
		rproc_shutdown(glue->rproc);
		rproc_put(glue->rproc);
		glue->rproc = NULL;
	}

	return MANGO_COMPAT_PLATFORM_REMOVE_OK;
}

static const struct of_device_id smi_platform_of_match[] = {
	{
		.compatible = COMPAT_PLT_MB_SMI,
	},
	{}
};
MODULE_DEVICE_TABLE(of, smi_platform_of_match);

static struct platform_driver smi_platform_driver = {
	.probe = smi_platform_probe,
	.remove = smi_platform_remove,
	.driver = {
		.name = SMI_NAME,
		.of_match_table = smi_platform_of_match,
		.groups = smi_driver_groups,
	},
};

int mango_mgmt_smi_platform_register(void)
{
	return platform_driver_register(&smi_platform_driver);
}

void mango_mgmt_smi_platform_unregister(void)
{
	platform_driver_unregister(&smi_platform_driver);
}

#else /* !CONFIG_OF */

int mango_mgmt_smi_platform_register(void)
{
	return 0;
}

void mango_mgmt_smi_platform_unregister(void)
{
}

#endif /* CONFIG_OF */
