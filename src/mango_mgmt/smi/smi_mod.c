// SPDX-License-Identifier: GPL-2.0-only
/*
 * smi_mod.c - libsmi's module-wide init and exit
 *
 * Copyright (c) 2023 - 2025 Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include <linux/module.h>

#include "backend/transport/smi_rpmsg_glue.h"
#include "backend/transport/smi_transport.h" /* fw_if_err_to_errno() */
#include "backend/transport/fal/gcq/fw_if_gcq.h"

#include "frontend/smi_frontend.h"
#include "smi_api.h"
#include "smi_top.h"

static FW_IF_GCQ_INIT_CFG fw_if_gcq_init_cfg = { 0 };

/* What every SMI device shares: the GCQ FAL, the surfaces' module-wide state
 * and the RPMsg endpoint driver (the RPMsg FAL's receive side). Device drivers
 * are the management transport's; each probe attaches SMI with smi_device_init(). */
int mango_mgmt_smi_init(void)
{
	int ret;

	ret = fw_if_err_to_errno(ulFW_IF_GCQ_Init(&fw_if_gcq_init_cfg));
	if (ret)
		goto fail;

	ret = smi_frontend_module_init();
	if (ret)
		goto undo_fal;

	ret = smi_rpmsg_driver_init();
	if (ret)
		goto undo_frontend;

	return 0;

undo_frontend:
	smi_frontend_module_exit();
undo_fal:
	ulFW_IF_GCQ_Deinit();
fail:
	PR_ERR("Failed to load driver to the kernel");
	return ret;
}

void mango_mgmt_smi_exit(void)
{
	smi_rpmsg_driver_exit();
	smi_frontend_module_exit();
	ulFW_IF_GCQ_Deinit();
}
