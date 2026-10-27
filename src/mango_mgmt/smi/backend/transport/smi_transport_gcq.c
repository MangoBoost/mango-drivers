// SPDX-License-Identifier: GPL-2.0
/*
 * smi_transport_gcq.c - GCQ Transport Backend (Architecture 2A)
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved. */

#include <linux/device.h>
#include <linux/io.h>
#include <linux/slab.h>

#include "fw_if_gcq.h"
#include "gcq.h"
#include "smi.h"
#include "smi_boot.h"
#include "smi_transport_backend.h"

/*****************************************************************************/
/* Private functions                                                         */
/*****************************************************************************/
/**
 * gcq_start_services() - start the service running.
 * @tp_ctx: SMC data struct instance.
 *
 * Start service to allow incoming requests to be handled
 *
 * Return: None.
 */
static int gcq_start_services(struct smi_transport *tp_ctx)
{
	if (!tp_ctx)
		return -EINVAL;

	TP_VDBG(tp_ctx, "Starting GCQ services");

	/* Start receiving incoming commands */
	smi_transport_set_halted(tp_ctx, false);

	TP_VDBG(tp_ctx, "Successfully started GCQ service");

	return SUCCESS;
}

/**
 * memcpy_gcq_payload_from_device() - copy data from shared memory.
 * @tp_ctx: SMC data struct instance.
 * @offset: the offset.
 * @dst: the destination address.
 * @len: the length.
 *
 * Return: None.
 */
static int memcpy_gcq_payload_from_device(struct smi_transport *tp_ctx,
					  uint32_t offset, void *dst,
					  size_t len)
{
	if (!tp_ctx || !dst)
		return -EINVAL;
	if (!smi_payload_off_ok(&tp_ctx->mem_layout, offset, len))
		return -EINVAL;

	memcpy_fromio(dst,
		      (void __iomem *)(tp_ctx->payload_base_virt_addr +
				       tp_ctx->lane_off + offset),
		      len);

	return SUCCESS;
}

/**
 * memcpy_gcq_payload_to_device() - copy data to shared memory.
 * @tp_ctx: SMC data struct instance.
 * @offset: the offset.
 * @data: the data pointer.
 * @len: the length.
 *
 * Return: None.
 */
static int memcpy_gcq_payload_to_device(struct smi_transport *tp_ctx,
					uint32_t offset, const void *data,
					size_t len)
{
	if (!tp_ctx || !data)
		return -EINVAL;
	if (!smi_payload_off_ok(&tp_ctx->mem_layout, offset, len))
		return -EINVAL;

	memcpy_toio((void __iomem *)(tp_ctx->payload_base_virt_addr +
				     tp_ctx->lane_off + offset),
		    data, len);

	return 0;
}

/**
 * gcq_window_read() - copy raw window bytes for the bootstrap axis.
 * @tp_ctx: SMC data struct instance.
 * @off: offset from the payload base.
 * @dst: the destination address.
 * @len: the length.
 *
 * Unbounded: it runs before a layout exists. The base is re-snapshotted so a
 * teardown that NULLs it is seen as -ENODEV, like the other window readers.
 *
 * Return: 0 on success, negative errno otherwise.
 */
static int gcq_window_read(struct smi_transport *tp_ctx, uint32_t off,
			   void *dst, size_t len)
{
	void __iomem *base;

	if (!tp_ctx || !dst)
		return -EINVAL;

	base = READ_ONCE(tp_ctx->payload_base_virt_addr);
	if (!base)
		return -ENODEV;
	if (!smi_span_in(tp_ctx->payload_len, off, len))
		return -ERANGE;

	memcpy_fromio(dst, base + off, len);

	return SUCCESS;
}

/* One readl: a word the card rewrites is read whole or not at all. */
static int gcq_window_read32(struct smi_transport *tp_ctx, uint32_t off,
			     uint32_t *out)
{
	void __iomem *base;

	if (!tp_ctx || !out)
		return -EINVAL;

	base = READ_ONCE(tp_ctx->payload_base_virt_addr);
	if (!base)
		return -ENODEV;
	if (!smi_span_in(tp_ctx->payload_len, off, sizeof(*out)))
		return -ERANGE;

	*out = readl(base + off);

	return SUCCESS;
}

/**
 * gcq_stop_services() - stop the service running.
 * @tp_ctx: SMI message context.
 *
 * Stop service will be called from driver remove if timeout cmd is found
 *
 * Return: None.
 */
static void gcq_stop_services(struct smi_transport *tp_ctx)
{
	/* stop receiving incoming commands (idempotent; NULL-safe) */
	smi_transport_set_halted(tp_ctx, true);
}

static int gcq_check_compatibility(struct smi_transport *tp_ctx, uint16_t major,
				   uint16_t minor)
{
	GCQ_VERSION_TYPE ver = { 0 };

	if (!tp_ctx)
		return -EINVAL;

	if (iGCQGetVersion(&ver) == SUCCESS) {
		if ((ver.ucVerMajor == major) && (ver.ucVerMinor == minor)) {
			TP_DBG(tp_ctx,
			       "Message Layer Supported Version : %d.%d", major,
			       minor);
			return SUCCESS;
		}
	}

	/* Telemetry, not a gate: the library's version describes a library, not
	 * this wire, so a mismatch costs a log line. */
	TP_WARN(tp_ctx,
		"Message Layer version differs (this build %d.%d, SMC %d.%d)",
		ver.ucVerMajor, ver.ucVerMinor, major, minor);

	return -ENOTSUPP;
}

static const struct smi_transport_ops gcq_ops = {
	.read = memcpy_gcq_payload_from_device,
	.write = memcpy_gcq_payload_to_device,
	.window_read = gcq_window_read,
	.window_read32 = gcq_window_read32,
	.start = gcq_start_services,
	.stop = gcq_stop_services,
	.check_compatibility = gcq_check_compatibility,
};

static int gcq_create_from(struct smi_transport *tp_ctx,
			   const struct smi_transport_res *res,
			   struct smi_layout *layout)
{
	struct gcq_partition_layout gcq_layout = { 0 };
	struct smi_log_view log_view = { 0 };
	int ret;

	/* Names what is missing: a card whose table listed no GCQ pair and one
	 * whose BARs are unmapped both arrive here, and they are different
	 * problems. */
	if (!res || !res->queue_base || !res->payload_base ||
	    !res->payload_len) {
		TP_ERR(tp_ctx,
		       "GCQ needs a queue and a payload region: queue %s, payload %s of %u bytes",
		       (res && res->queue_base) ? "mapped" : "absent",
		       (res && res->payload_base) ? "mapped" : "absent",
		       res ? res->payload_len : 0u);
		return -EINVAL;
	}

	tp_ctx->base_virt_addr = res->queue_base;
	tp_ctx->payload_base_virt_addr = res->payload_base;
	tp_ctx->payload_len = res->payload_len;
	/* Installed before the probe: the bootstrap axis reads this window
	 * through window_read(). */
	tp_ctx->ops = &gcq_ops;

	/* What the window says about itself. The medium reads no descriptor of
	 * its own; it takes the data region from the neutral product and the
	 * ring geometry it alone consumes from the out-param. */
	ret = smi_boot_probe(tp_ctx, layout, &gcq_layout, &log_view);
	if (ret)
		return ret;

	/* This medium's own terms: a completion is one envelope in every
	 * generation, and a slot cannot exceed the ring it is laid out in --
	 * which also keeps the library's slot x count inside 32 bits. Whether a
	 * request fits is the settled generation's question, asked at submit. */
	if ((0u == gcq_layout.sq_slot) ||
	    (gcq_layout.cq_slot < SMI_COMPLETION_FRAME_BYTES) ||
	    (gcq_layout.sq_slot > gcq_layout.ring_len) ||
	    (gcq_layout.cq_slot > gcq_layout.ring_len)) {
		TP_ERR(tp_ctx,
		       "window states %u B / %u B ring slots in a %u B ring",
		       gcq_layout.sq_slot, gcq_layout.cq_slot,
		       gcq_layout.ring_len);
		return -EPROTO;
	}
	/* Refused rather than clamped, unlike the data region: the ring's extent
	 * is what its slots are laid out by, so a shorter one is not less
	 * capacity but a different geometry from the card's. */
	if (!smi_span_in(smi_lane_room(tp_ctx->payload_len, layout->lane_off),
			 gcq_layout.ring_off, gcq_layout.ring_len) ||
	    (0u == gcq_layout.ring_len)) {
		TP_ERR(tp_ctx,
		       "window states a %u B ring at +%u of a lane at +%u, mapping is %u B",
		       gcq_layout.ring_len, gcq_layout.ring_off,
		       layout->lane_off, tp_ctx->payload_len);
		return -EPROTO;
	}

	smi_transport_take_layout(tp_ctx, layout);

	tp_ctx->mem_layout.ring.start = gcq_layout.ring_off;
	tp_ctx->mem_layout.ring.len = gcq_layout.ring_len;

	ret = smi_transport_take_log_view(tp_ctx, &log_view);
	if (ret)
		return ret;

	/* Set ring buffer virtual address */
	tp_ctx->ring_buf_base_virt_addr =
		(void __iomem *)((u8 *)tp_ctx->payload_base_virt_addr +
				 tp_ctx->lane_off +
				 tp_ctx->mem_layout.ring.start);

	tp_ctx->backend.gcq.ullBaseAddress = (uint64_t)tp_ctx->base_virt_addr;
	tp_ctx->backend.gcq.xInterruptMode = FW_IF_GCQ_INTERRUPT_MODE_NONE;
	tp_ctx->backend.gcq.xMode = FW_IF_GCQ_MODE_CONSUMER;
	tp_ctx->backend.gcq.ullRingAddress =
		(uint64_t)tp_ctx->ring_buf_base_virt_addr;
	tp_ctx->backend.gcq.ulRingLength =
		(uint64_t)tp_ctx->mem_layout.ring.len;
	/* The card's numbers, not this build's: a slot size compiled on both
	 * sides is two answers to one question, and the ring is built out of
	 * them. */
	tp_ctx->backend.gcq.ulSubmissionQueueSlotSize = gcq_layout.sq_slot;
	tp_ctx->backend.gcq.ulCompletionQueueSlotSize = gcq_layout.cq_slot;
	/* A frame rides one submission slot. */
	tp_ctx->tx_capacity = gcq_layout.sq_slot;

	return fw_if_err_to_errno(
		ulFW_IF_GCQ_Create(&tp_ctx->fw_if_cfg, &tp_ctx->backend.gcq));
}

/* The layout is off the stack: it carries a record per verb and per section. */
static int gcq_create(struct smi_transport *tp_ctx,
		      const struct smi_transport_res *res)
{
	struct smi_layout *layout = kzalloc(sizeof(*layout), GFP_KERNEL);
	int ret;

	if (!layout)
		return -ENOMEM;
	ret = gcq_create_from(tp_ctx, res, layout);
	kfree(layout);

	return ret;
}

const struct smi_transport_backend gcq_transport_backend = {
	.type = FW_IF_TYPE_GCQ,
	.ops = &gcq_ops,
	.create = gcq_create,
};
