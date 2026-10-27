// SPDX-License-Identifier: GPL-2.0
/*
 * smi_transport_rpmsg.c - RPMSG Transport Implementation
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved. */

#include <linux/device.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/of_address.h>
#include <linux/slab.h>

#include "fw_if_rpmsg.h"
#include "smi_boot.h"
#include "smi_rpmsg_glue.h"
#include "smi_transport_backend.h"

#ifdef CONFIG_OF

/* How long the card's channel may take to appear once its firmware is up. */
#define RPMSG_CHANNEL_WAIT_MS (2000)

static FW_IF_CFG *current_rpmsg_fal;

static int rpmsg_start(struct smi_transport *tp_ctx)
{
	smi_transport_set_halted(tp_ctx, false);

	return 0;
}

static void rpmsg_stop(struct smi_transport *tp_ctx)
{
	smi_transport_set_halted(tp_ctx, true);

	if (current_rpmsg_fal == &tp_ctx->fw_if_cfg)
		current_rpmsg_fal = NULL;
}

static int rpmsg_shm_read(struct smi_transport *tp_ctx, uint32_t offset,
			  void *dst, size_t len)
{
	if (!tp_ctx || !dst || !tp_ctx->payload_base_virt_addr)
		return -EINVAL;

	if (!smi_payload_off_ok(&tp_ctx->mem_layout, offset, len))
		return -EINVAL;

	memcpy(dst, (u8 *)tp_ctx->payload_base_virt_addr + offset, len);

	return 0;
}

static int rpmsg_shm_write(struct smi_transport *tp_ctx, uint32_t offset,
			   const void *src, size_t len)
{
	if (!tp_ctx || !src || !tp_ctx->payload_base_virt_addr)
		return -EINVAL;

	if (!smi_payload_off_ok(&tp_ctx->mem_layout, offset, len))
		return -EINVAL;

	memcpy((u8 *)tp_ctx->payload_base_virt_addr + offset, src, len);
	return 0;
}

/**
 * rpmsg_window_read() - copy raw window bytes for the bootstrap axis.
 *
 * Runs before a layout exists, so the mapping's own length is the bound: what
 * the descriptor will state is exactly what has not been read yet.
 */
static int rpmsg_window_read(struct smi_transport *tp_ctx, uint32_t off,
			     void *dst, size_t len)
{
	if (!tp_ctx || !dst || !tp_ctx->payload_base_virt_addr)
		return -EINVAL;

	if (!smi_span_in(tp_ctx->payload_len, off, len))
		return -ERANGE;

	memcpy(dst, (u8 *)tp_ctx->payload_base_virt_addr + off, len);

	return 0;
}

/* One load of mapped memory, where a copy could tear a word the card rewrites. */
static int rpmsg_window_read32(struct smi_transport *tp_ctx, uint32_t off,
			       uint32_t *out)
{
	const u8 *base;

	if (!tp_ctx || !out || !tp_ctx->payload_base_virt_addr)
		return -EINVAL;

	if (!smi_span_in(tp_ctx->payload_len, off, sizeof(*out)))
		return -ERANGE;

	base = (const u8 *)tp_ctx->payload_base_virt_addr;
	*out = READ_ONCE(*(const u32 *)(base + off));

	return 0;
}

static const struct smi_transport_ops rpmsg_ops = {
	.read = rpmsg_shm_read,
	.write = rpmsg_shm_write,
	.window_read = rpmsg_window_read,
	.window_read32 = rpmsg_window_read32,
	.start = rpmsg_start,
	.stop = rpmsg_stop,
	.check_compatibility = NULL,
};

/**
 * rpmsg_map_region() - Map one of this node's reserved regions.
 * @dev: the platform device, whose node names the regions.
 * @name: the entry in "memory-region-names", or NULL to take @idx directly.
 * @idx: the "memory-region" index used when @name is absent, -1 for none.
 * @len: receives the region's length, when the caller needs it.
 *
 * Return: the mapping, or NULL when the node names no such region.
 */
static void *rpmsg_map_region(struct device *dev, const char *name, int idx,
			      resource_size_t *len, resource_size_t *pa)
{
	struct device_node *node;
	struct resource r;
	void *base;
	int at = idx;

	if (name) {
		const int named = of_property_match_string(
			dev->of_node, "memory-region-names", name);

		if (named >= 0)
			at = named;
	}
	if (at < 0)
		return NULL;

	node = of_parse_phandle(dev->of_node, "memory-region", at);
	if (!node)
		return NULL;

	if (of_address_to_resource(node, 0, &r)) {
		of_node_put(node);
		return NULL;
	}
	of_node_put(node);

	base = devm_memremap(dev, r.start, resource_size(&r), MEMREMAP_WC);
	if (IS_ERR_OR_NULL(base))
		return NULL;

	dev_info(dev, "%s region: 0x%llx, %llu bytes at %p\n",
		 name ? name : "shm", (unsigned long long)r.start,
		 (unsigned long long)resource_size(&r), base);

	if (len)
		*len = resource_size(&r);
	if (pa)
		*pa = r.start;

	return base;
}

/* What this medium carries: one virtio rpmsg buffer less its header. The
 * channel could be asked on a new enough kernel, but not on every kernel this
 * driver builds against -- and a frame here is tens of bytes, so the buffer is
 * the honest ledger. A channel that carries less says so at send time. */
#define RPMSG_TX_CAPACITY (496u)

static int rpmsg_bind_channel(struct smi_transport *tp_ctx)
{
	struct device *dev = tp_ctx->dev;
	struct rpmsg_device *rpdev = smi_rpmsg_get_current_rpdev();

	/* The completion is an edge, so a channel that arrived before this call
	 * would leave the wait to time out on an event already spent. */
	if (!rpdev && !wait_for_completion_timeout(
			      &rpmsg_ready_comp,
			      msecs_to_jiffies(RPMSG_CHANNEL_WAIT_MS))) {
		dev_dbg(dev, "RPMsg channel wait timeout\n");
		return -EPROBE_DEFER;
	}

	rpdev = rpdev ? rpdev : smi_rpmsg_get_current_rpdev();
	if (!rpdev) {
		/* A deferral, so quiet: the driver core reports one that never
		 * resolves, and this one retries on every probe. */
		dev_dbg(dev, "RPMsg channel is complete but rpdev is NULL\n");
		return -EPROBE_DEFER;
	}

	tp_ctx->tx_capacity = RPMSG_TX_CAPACITY;

	return smi_rpmsg_bind_channel(&tp_ctx->fw_if_cfg, rpdev);
}

static int rpmsg_create_from(struct smi_transport *tp_ctx,
			     const struct smi_transport_res *res,
			     struct smi_layout *layout)
{
	struct device *dev;
	resource_size_t lane_len = 0;
	resource_size_t ro_len = 0;
	resource_size_t lane_pa = 0;
	struct gcq_partition_layout gcq_layout = { 0 };
	struct smi_log_view log_view = { 0 };
	int ret;

	/* RPMSG maps its own shared memory via OF, so nothing handed in is used. */
	(void)res;

	if (!tp_ctx || !tp_ctx->dev)
		return -EINVAL;

	dev = tp_ctx->dev;

	if (!dev->of_node) {
		dev_err(dev,
			"No device tree node — cannot map shared memory\n");
		return -ENODEV;
	}

	tp_ctx->payload_base_virt_addr =
		rpmsg_map_region(dev, "lane", 0, &lane_len, &lane_pa);
	if (!tp_ctx->payload_base_virt_addr) {
		dev_err(dev, "Failed to map this lane's shared memory\n");
		return -ENOMEM;
	}
	tp_ctx->payload_len = (uint32_t)lane_len;
	tp_ctx->payload_pa = (uint64_t)lane_pa;

	/* The other mapping this host takes: the region every host of the card
	 * reads and none writes. A device tree that names none leaves this lane
	 * with what its own prefix states, which is the ring and the data. */
	tp_ctx->ro_base = rpmsg_map_region(dev, "ro", -1, &ro_len, NULL);
	tp_ctx->ro_len = (uint32_t)ro_len;

	/* Installed before the read: the bootstrap axis reaches this window
	 * through window_read(). */
	tp_ctx->ops = &rpmsg_ops;

	/* Ask the window what it is. The descriptor is written before the
	 * channel appears: without a channel the firmware is still booting and
	 * the probe goes back to the bus; once one is seen, a second read is
	 * final, so a channel that appeared after the first read is not lost. */
	ret = smi_boot_read(tp_ctx, layout, &gcq_layout, &log_view);
	if (ret) {
		if (!smi_rpmsg_get_current_rpdev())
			return -EPROBE_DEFER;
		memset(layout, 0, sizeof(*layout));
		memset(&gcq_layout, 0, sizeof(gcq_layout));
		ret = smi_boot_read(tp_ctx, layout, &gcq_layout, &log_view);
	}
	if (ret) {
		dev_err(dev, "No descriptor in this window (%d)\n", ret);
		return ret;
	}
	smi_transport_take_layout(tp_ctx, layout);
	ret = smi_transport_take_log_view(tp_ctx, &log_view);
	if (ret)
		return ret;
	ret = fw_if_err_to_errno(ulFW_IF_RPMsg_Create(&tp_ctx->fw_if_cfg,
						      &tp_ctx->backend.rpmsg));
	if (ret)
		return ret;

	/* The channel binds into the config the call above installed, so this
	 * is the earliest a frame can be carried and the latest this backend
	 * may claim it was created. */
	ret = rpmsg_bind_channel(tp_ctx);
	if (ret)
		return ret;

	/* Published last, and only on success: a failed create hands this
	 * transport back to devres, and a global still pointing at it is a
	 * write into freed memory the next unbind would make. */
	current_rpmsg_fal = &tp_ctx->fw_if_cfg;
	return 0;
}

/* The layout is off the stack: it carries a record per verb and per section. */
static int rpmsg_create(struct smi_transport *tp_ctx,
			const struct smi_transport_res *res)
{
	struct smi_layout *layout = kzalloc(sizeof(*layout), GFP_KERNEL);
	int ret;

	if (!layout)
		return -ENOMEM;
	ret = rpmsg_create_from(tp_ctx, res, layout);
	kfree(layout);

	return ret;
}

const struct smi_transport_backend rpmsg_transport_backend = {
	.type = FW_IF_TYPE_RPMSG,
	.ops = &rpmsg_ops,
	.create = rpmsg_create,
};

int smi_rpmsg_bind_channel(FW_IF_CFG *fal, void *rpdev)
{
	FW_IF_RPMSG_CFG *rpmsg_cfg;

	/* Named, not silent: returning without binding is exactly the state
	 * where every frame is refused with no reason in it. */
	if (!fal || !fal->cfg || !rpdev)
		return -EINVAL;

	rpmsg_cfg = (FW_IF_RPMSG_CFG *)fal->cfg;
	rpmsg_cfg->rpdev = rpdev;
	return 0;
}

void smi_rpmsg_unregister_channel(void)
{
	FW_IF_RPMSG_CFG *rpmsg_cfg;

	if (!current_rpmsg_fal || !current_rpmsg_fal->cfg)
		return;

	rpmsg_cfg = (FW_IF_RPMSG_CFG *)current_rpmsg_fal->cfg;
	rpmsg_cfg->rpdev = NULL;
}

FW_IF_CFG *smi_rpmsg_get_fal(void)
{
	return current_rpmsg_fal;
}

#else /* !CONFIG_OF */

int smi_rpmsg_bind_channel(FW_IF_CFG *fal, void *rpdev)
{
	return -ENODEV;
}

void smi_rpmsg_unregister_channel(void)
{
}

FW_IF_CFG *smi_rpmsg_get_fal(void)
{
	return NULL;
}

/* Dead path on non-OF builds, but the descriptor must exist for linking
 * (smi_backends[] references it). */
static int rpmsg_create(struct smi_transport *tp_ctx,
			const struct smi_transport_res *res)
{
	return -ENODEV;
}

const struct smi_transport_backend rpmsg_transport_backend = {
	.type = FW_IF_TYPE_RPMSG,
	.ops = NULL,
	.create = rpmsg_create,
};

#endif /* CONFIG_OF */
