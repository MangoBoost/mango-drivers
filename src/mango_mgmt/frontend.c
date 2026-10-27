// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - Frontend Implementation
 *
 * Composes the components a management function carries: MGMT (its class
 * device, then MPIPE -> RPC API -> terminal) and SMI. Each comes up and goes
 * down on its own; only the shared resources fail the probe.
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include <linux/printk.h>

#include "frontend.h"
#include "term/server.h"
#include "term/client.h"
#include "transport/transport.h"
#include "mpipe/netdev.h"
#include "api/api.h"
#include "smi_glue.h"
#include "sysfs.h"
#include "dev.h"
#include "regmap.h"

/* MPIPE link (HW + netdev, registered down), the RPC API on it, then the
 * terminal: virtio-console server on the SoC, chardev client on the Host. */
static int mango_mgmt_frontend_mpipe_init(struct mango_mgmt_dev *mgmt)
{
	bool is_soc = mango_mgmt_transport_is_soc_device(mgmt);
	int ret;

	ret = mango_mgmt_mpipe_init(mgmt);
	if (ret) {
		dev_err(mgmt->dev, "mango_mgmt_mpipe_init failed: %d", ret);
		return ret;
	}

	ret = mango_mgmt_api_init(mgmt);
	if (ret < 0) {
		dev_err(mgmt->dev, "mango_mgmt_api_init failed (%d)", ret);
		goto err_channel;
	}

	ret = is_soc ? mango_mgmt_term_server_init(mgmt)
		     : mango_mgmt_term_client_init(mgmt);
	if (ret < 0) {
		dev_err(mgmt->dev, "terminal init failed (%d)", ret);
		goto err_api;
	}

	return 0;

err_api:
	mango_mgmt_api_exit(mgmt);
err_channel:
	mango_mgmt_mpipe_exit(mgmt);
	return ret;
}

static void mango_mgmt_frontend_mpipe_exit(struct mango_mgmt_dev *mgmt)
{
	if (mango_mgmt_transport_is_soc_device(mgmt))
		mango_mgmt_term_server_exit(mgmt);
	else
		mango_mgmt_term_client_exit(mgmt);

	mango_mgmt_api_exit(mgmt);
	mango_mgmt_mpipe_exit(mgmt);
}

/* Class device first: its minor is the devid the API and netdev take their
 * names from. The FIFO chain follows where it works, and ready marks the
 * service registered. */
static int mango_mgmt_frontend_mgmt_init(struct mango_mgmt_dev *mgmt)
{
	int ret;

	ret = mango_mgmt_sysfs_setup(mgmt);
	if (ret)
		return ret;

	if (!mango_mgmt_regmap_has(mgmt, MGMT_BLK_MPIPE))
		return 0;

	ret = mango_mgmt_frontend_mpipe_init(mgmt);
	if (ret) {
		mango_mgmt_sysfs_teardown(mgmt);
		return ret;
	}

	atomic_set(&mgmt->mgmt.ready, 1);
	return 0;
}

static void mango_mgmt_frontend_mgmt_exit(struct mango_mgmt_dev *mgmt)
{
	struct mango_mgmt_service *svc = &mgmt->mgmt;

	if (svc->devid < 0)
		return;

	atomic_set(&svc->ready, 0);
	if (svc->netdev)
		mango_mgmt_frontend_mpipe_exit(mgmt);
	mango_mgmt_sysfs_teardown(mgmt);
}

int mango_mgmt_frontend_init_device(struct mango_mgmt_dev *mgmt)
{
	/* The MPIPE block is the MGMT component's: no block, no surface. */
	bool want_mgmt = mango_mgmt_regmap_has(mgmt, MGMT_BLK_MPIPE);
	bool want_smi = mgmt->desc->smi != MGMT_SMI_NONE;
	int mgmt_ret = -ENODEV, smi_ret = -ENODEV;

	if (want_mgmt) {
		mgmt_ret = mango_mgmt_frontend_mgmt_init(mgmt);
		if (mgmt_ret)
			dev_err(mgmt->dev, "MGMT unavailable (%d)", mgmt_ret);
	}

	/* Last: attach waits for the card's SMC, which MPIPE need not. */
	if (want_smi) {
		smi_ret = mango_mgmt_smi_attach(mgmt);
		/* A deferral is the bus's to retry, so the whole function
		 * goes back rather than binding without SMI for good. */
		if (smi_ret == -EPROBE_DEFER) {
			mango_mgmt_frontend_mgmt_exit(mgmt);
			return smi_ret;
		}
		if (smi_ret)
			dev_err(mgmt->dev, "SMI unavailable (%d)", smi_ret);
	}

	/* One component up is a bound function; none is a failed probe. */
	if (mgmt_ret && smi_ret)
		return want_mgmt ? mgmt_ret : smi_ret;

	dev_info(mgmt->dev, "Frontend initialization completed");
	return 0;
}

void mango_mgmt_frontend_cleanup_device(struct mango_mgmt_dev *mgmt)
{
	if (!mgmt)
		return;

	mango_mgmt_smi_detach(mgmt);
	mango_mgmt_frontend_mgmt_exit(mgmt);
}

void mango_mgmt_frontend_quiesce(struct mango_mgmt_dev *mgmt)
{
	atomic_set(&mgmt->mgmt.ready, 0);
	mango_mgmt_mpipe_quiesce(mgmt);
	mango_mgmt_smi_quiesce(mgmt);
}

int mango_mgmt_frontend_resume(struct mango_mgmt_dev *mgmt)
{
	int ret = mango_mgmt_mpipe_resume(mgmt);

	if (ret)
		return ret;
	if (mgmt->mgmt.netdev)
		atomic_set(&mgmt->mgmt.ready, 1);
	/* The submit gate reopens only on a fresh attach. */
	if (mgmt->smi)
		dev_warn(mgmt->dev, "SMI stays quiesced until re-attach");
	return 0;
}
