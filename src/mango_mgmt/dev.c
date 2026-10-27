// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - Management device
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include <linux/err.h>
#include <linux/slab.h>

#include "dev.h"
#include "regmap.h"
#include "transport/transport.h"
#include "monitor/monitor.h"

#define TO_MGMT_PRIV(mgmt) container_of(mgmt, struct mango_mgmt_priv, mgmt)

struct mango_mgmt_priv {
	struct mango_mgmt_dev mgmt;
};

struct mango_mgmt_dev *
mango_mgmt_dev_alloc(struct device *dev, struct mango_mgmt_transport *transport,
		     const struct mango_mgmt_desc *desc)
{
	struct mango_mgmt_dev *mgmt;
	struct mango_mgmt_priv *priv;
	int err;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	mgmt = &priv->mgmt;
	mgmt->dev = dev;
	mgmt->desc = desc;
	mgmt->transport = transport;
	mgmt->mgmt.devid = -1;
	transport->mgmt = mgmt; /* IRQ handlers reach mgmt through it */

	err = transport->init(mgmt);
	if (err < 0)
		goto err_free_priv;

	mgmt->monitor = mango_mgmt_monitor_get(mgmt);

	return mgmt;

err_free_priv:
	kfree(priv);
	return ERR_PTR(err);
}

void mango_mgmt_dev_free(struct mango_mgmt_dev *mgmt)
{
	struct mango_mgmt_priv *priv = TO_MGMT_PRIV(mgmt);

	mgmt->transport->fini(mgmt);

	kfree(priv);
}
