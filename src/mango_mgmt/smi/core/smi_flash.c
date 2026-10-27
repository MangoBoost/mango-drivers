// SPDX-License-Identifier: GPL-2.0-only
/*
 * smi_flash.c - The burn service: every flash op a card is asked for.
 *
 * Copyright (c) 2023 - 2025 Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/eventfd.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include <mango_compat/eventfd.h>

#include "contract/smi/smi_abi.h" /* the Status tail a background copy reports in */
#include "smi_cmd_abort.h" /* smi_cmd_aborted_after_submit() */
#include "smi_flash.h"
#include "smi_flash_wire.h"
#include "smi_log.h"
#include "smi_rpc.h" /* smi_pub_status_read() */
#include "smi_top.h"

/* For invalidating the first PDI chunk in case the download fails */
#define INVALID_BOOT_TAG (0xFFFFFFFF)

/* How long a background copy may show no progress, and how often to look. The
 * verdict is NO PROGRESS, not total time: a total depends on partition size and
 * flash speed, while a percentage that has not moved means the same
 * everywhere. */
#define COPY_NO_PROGRESS_MS (120000)
#define COPY_POLL_MS (500)

/* Each op's name in the log, and what a stop can leave on the card. */
static const struct {
	const char *name;
	const char *left;
} smi_flash_kinds[] = {
	[SMI_FLASH_IMAGE] = { "PDI download",
			      "partition contents may be incomplete" },
	[SMI_FLASH_FPT] = { "FPT update",
			    "the partition tables may be incomplete" },
	[SMI_FLASH_COPY] = { "Partition copy",
			     "destination contents are undefined" },
	[SMI_FLASH_BOOT] = { "Boot partition select",
			     "selection may not have taken effect" },
};

const struct smi_flash_wire *smi_flash_wire(struct pf_dev_struct *pf_dev)
{
	return &smi_flash_wire_v2;
}

/* Every refusal of an op, on the card that will run it: a value its wire
 * cannot carry is refused, not cut, and so is an op it does not serve. */
static int smi_flash_check(struct pf_dev_struct *pf_dev,
			   const struct smi_flash_op *op)
{
	const struct smi_flash_wire *p = smi_flash_wire(pf_dev);

	switch (op->kind) {
	case SMI_FLASH_IMAGE:
	case SMI_FLASH_FPT:
		if ((op->device >= SMI_SMC_BOOT_DEVICE_MAX) ||
		    ((op->kind == SMI_FLASH_IMAGE) &&
		     (op->partition > p->part_max)))
			return -EINVAL;
		if (!op->len || (op->len > SMI_FLASH_MAX_LEN) || !op->image ||
		    !pf_dev->smc_ctx)
			return -EINVAL;
		return p->chunk_bytes(pf_dev) ? 0 : -EOPNOTSUPP;
	case SMI_FLASH_COPY:
		if ((op->device >= SMI_SMC_BOOT_DEVICE_MAX) ||
		    (op->dst_device >= SMI_SMC_BOOT_DEVICE_MAX) ||
		    (op->partition > p->part_max) ||
		    (op->dst_partition > p->part_max))
			return -EINVAL;
		return 0;
	case SMI_FLASH_BOOT:
		return (op->partition > p->part_max) ? -EINVAL : 0;
	}

	return -EINVAL;
}

/* Copy the image in, now the lock is held, and write it by offset: the boot
 * tag goes invalid first and the first chunk last, so a stopped burn never
 * boots. @written: how far it got, the first chunk counted from its tag. */
static int smi_flash_image(struct pf_dev_struct *pf_dev,
			   const struct smi_flash_op *op, uint32_t *written)
{
	const struct smi_flash_wire *p = smi_flash_wire(pf_dev);
	const uint32_t chunk = p->chunk_bytes(pf_dev);
	const uint32_t size = op->len;
	struct smi_chunk_arg arg = { .boot = (uint8_t)op->device,
				     .part = (uint8_t)op->partition,
				     .fpt = (op->kind == SMI_FLASH_FPT) };
	struct eventfd_ctx *progress = NULL;
	uint32_t bytes_written = 0;
	bool rewrite_boot_tag = false;
	uint8_t *buf;
	int ret = SUCCESS;

	buf = vmalloc(size);
	if (!buf)
		return -ENOMEM;

	/* An fd that is not an eventfd yields an error pointer, and both the
	 * progress signal and the exit path below take any non-NULL as a
	 * context: refuse here so neither dereferences it. */
	if (op->progress_efd >= 0) {
		progress = eventfd_ctx_fdget(op->progress_efd);
		if (IS_ERR(progress)) {
			ret = PTR_ERR(progress);
			progress = NULL;
			goto out;
		}
	}

	if (copy_from_user(buf, op->image, size)) {
		ret = -EFAULT;
		goto out;
	}

	if (arg.fpt)
		arg.part = 0;

	SMI_DEV_VDBG(
		pf_dev->dev,
		"Attempting to download PDI bitstream with image size %u to partition %u in %u-byte chunks",
		size, op->partition, chunk);

	/* The last point a signal stops the burn: past the first write the
	 * chunks wait through signals, and the image is written to its end. */
	if (signal_pending(current)) {
		ret = -EINTR;
		goto out;
	}

	while (bytes_written < size) {
		const uint32_t bytes_to_write =
			min(chunk, size - bytes_written);

		arg.off = bytes_written;
		/* Keep the boot tag when updating the FPT or when there is
		 * only one chunk. */
		if (arg.fpt || (size <= chunk) || bytes_written) {
			arg.last = !rewrite_boot_tag &&
				   (bytes_written + bytes_to_write >= size);
			ret = p->chunk_write(pf_dev, &arg, &buf[bytes_written],
					     bytes_to_write);
			if (ret)
				break;

			if (progress)
				mango_compat_eventfd_signal(progress);
		} else {
			uint32_t boot_tag = INVALID_BOOT_TAG;

			SMI_DEV_VDBG(pf_dev->dev,
				     "Invalidating the PDI boot tag");

			/* More than one chunk: invalidate the boot tag and
			 * write the first chunk last. */
			arg.last = false;
			ret = p->chunk_write(pf_dev, &arg,
					     (const uint8_t *)&boot_tag,
					     sizeof(uint32_t));

			/* No user signal here, but the byte count still
			 * advances so the loop continues as normal. */
			if (ret)
				break;

			rewrite_boot_tag = true;
		}

		SMI_DEV_VDBG(pf_dev->dev, "Done with the chunk at 0x%x",
			     bytes_written);

		bytes_written += bytes_to_write;
	}

	/* Check if we need to re-write the first chunk */
	if (!ret && rewrite_boot_tag) {
		SMI_DEV_VDBG(pf_dev->dev, "Rewriting first chunk");

		/* With more than one chunk the first is always full size. */
		arg.off = 0;
		arg.last = true;
		ret = p->chunk_write(pf_dev, &arg, buf, chunk);

		if (!ret && progress)
			mango_compat_eventfd_signal(progress);
	}
	*written = bytes_written;

out:
	if (progress)
		eventfd_ctx_put(progress);
	vfree(buf);
	return ret;
}

/* The published plane's own words, as a Linux caller hears them: its own fold
 * because errno is Linux's and the shared contract stays leaf. */
static int pub_op_to_errno(uint32_t status)
{
	switch (status) {
	case SmiPubOp_Success:
		return 0;
	case SmiPubOp_Busy:
		return -EALREADY;
	case SmiPubOp_InvalidValue:
		return -EINVAL;
	case SmiPubOp_VerifyFailed:
		return -EBADMSG;
	default:
		return -EIO;
	}
}

/*
 * Follow a copy the card took for the background.
 *
 * Return: 0 when it finished, or an errno. -ETIMEDOUT means progress stopped
 *         moving, not that the copy failed. */
static int copy_follow_published(struct pf_dev_struct *pf_dev, uint32_t op_id)
{
	unsigned int quiet_ms = 0;
	uint16_t last_percent = 0xffff;

	while (quiet_ms < COPY_NO_PROGRESS_MS) {
		struct smi_pub_status blk;
		int ret = smi_pub_status_read(pf_dev, &blk);

		if (ret)
			return ret;

		/* A newer copy was taken since: ours ended one seat back. */
		if ((blk.bg.op_id != (uint16_t)op_id) &&
		    (blk.bg_prev.op_id == (uint16_t)op_id))
			return pub_op_to_errno(blk.bg_prev.status);

		/* Somebody else's op, or the card forgot ours: either way this
		 * host is not the one being reported to any more. */
		if (blk.bg.op_id != (uint16_t)op_id)
			return -ECANCELED;

		if (SmiPubOp_Running != blk.bg.status)
			return pub_op_to_errno(blk.bg.status);

		if (blk.bg.percent != last_percent) {
			last_percent = blk.bg.percent;
			quiet_ms = 0;
			SMI_DEV_VDBG(pf_dev->dev, "copy %u%%", blk.bg.percent);
		} else {
			quiet_ms += COPY_POLL_MS;
		}

		msleep(COPY_POLL_MS);
	}

	SMI_DEV_ERR(pf_dev->dev, "partition copy stopped progressing at %u%%",
		    last_percent);
	return -ETIMEDOUT;
}

/* The card copies the whole source partition and stages it in its own memory.
 * An op id means it took the copy for the background; 0 means it ran
 * synchronously and this call already waited. */
static int smi_flash_copy(struct pf_dev_struct *pf_dev,
			  const struct smi_flash_op *op)
{
	uint32_t op_id = 0;
	int ret;

	SMI_DEV_VDBG(
		pf_dev->dev,
		"Attempting to copy from device %u partition %u to device %u partition %u",
		op->device, op->partition, op->dst_device, op->dst_partition);

	ret = smi_flash_wire(pf_dev)->copy_start(
		pf_dev, (uint8_t)op->device, (uint8_t)op->partition,
		(uint8_t)op->dst_device, (uint8_t)op->dst_partition, &op_id);
	if ((0 == ret) && (0 != op_id))
		ret = copy_follow_published(pf_dev, op_id);

	return ret;
}

/* What the op leaves said: a card that may hold part of what was asked is
 * warned about once bytes landed, or once a request drained after submit may
 * have run; the other codes never submitted it. */
static void smi_flash_said(struct pf_dev_struct *pf_dev,
			   const struct smi_flash_op *op, int ret,
			   uint32_t written)
{
	const char *name = smi_flash_kinds[op->kind].name;
	const char *left = smi_flash_kinds[op->kind].left;

	if (op->len && (ret == -EBADMSG))
		SMI_DEV_ERR(pf_dev->dev,
			    "FW update failed: image verify failed");
	else if (ret && op->len &&
		 (written || smi_cmd_aborted_after_submit(ret)))
		SMI_DEV_WARN(pf_dev->dev,
			     "%s stopped (%d) after %u of %u bytes; %s", name,
			     ret, written, op->len, left);
	else if (ret && smi_cmd_aborted_after_submit(ret))
		SMI_DEV_WARN(pf_dev->dev, "%s aborted (%d); %s", name, ret,
			     left);
	else if (!ret && (op->kind == SMI_FLASH_IMAGE))
		SMI_DEV_INFO(pf_dev->dev,
			     "FW update complete: %u bytes to partition %u",
			     op->len, op->partition);
}

int smi_flash_run(struct pf_dev_struct *pf_dev, const struct smi_flash_op *op)
{
	uint32_t written = 0;
	void *fence;
	int ret;

	if (!pf_dev || !op || (op->kind >= ARRAY_SIZE(smi_flash_kinds)))
		return -EINVAL;

	/* Before the image buffer, not after: a caller that has to wait for the
	 * card should not hold 150 MiB while it does. */
	if (mutex_lock_interruptible(&pf_dev->flash_lock))
		return -ERESTARTSYS;

	fence = smi_reset_fence(pf_dev);
	if (IS_ERR(fence)) {
		ret = PTR_ERR(fence);
		SMI_DEV_WARN(
			pf_dev->dev,
			"update refused (%d): a reset of this unit is in flight",
			ret);
		mutex_unlock(&pf_dev->flash_lock);
		return ret;
	}

	/* Judged under the fence: no reset, so no new card, until it drops. */
	ret = smi_flash_check(pf_dev, op);
	if (ret)
		goto out;

	switch (op->kind) {
	case SMI_FLASH_IMAGE:
	case SMI_FLASH_FPT:
		ret = smi_flash_image(pf_dev, op, &written);
		break;
	case SMI_FLASH_COPY:
		ret = smi_flash_copy(pf_dev, op);
		break;
	case SMI_FLASH_BOOT:
		SMI_DEV_VDBG(pf_dev->dev,
			     "Attempting to select device boot partition (%u)",
			     op->partition);
		ret = smi_flash_wire(pf_dev)->boot_select(pf_dev,
							  op->partition);
		break;
	}
	smi_flash_said(pf_dev, op, ret, written);

out:
	smi_reset_unfence(pf_dev, fence);
	mutex_unlock(&pf_dev->flash_lock);
	return ret;
}
