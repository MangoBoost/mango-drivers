/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * smi_flash.h - The burn service: one entry for every flash op.
 *
 * Holds the card's flash lock and the reset fence for a whole op, copies an
 * image in only once the lock is held, and runs the op to its end or to a
 * failure that is final. A surface translates its arguments; the card judges
 * the layout it is asked to change.
 *
 * Copyright (c) 2023-present Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_FLASH_H
#define SMI_FLASH_H

#include <linux/compiler.h> /* __user */
#include <linux/types.h>

struct pf_dev_struct;

#define SMI_FLASH_MAX_LEN (152 * 1024 * 1024) /* the most one op stages */

/**
 * enum smi_flash_kind - What one flash op does.
 * @SMI_FLASH_IMAGE: write an image into one partition.
 * @SMI_FLASH_FPT: write the partition tables.
 * @SMI_FLASH_COPY: copy one partition onto another.
 * @SMI_FLASH_BOOT: select the partition the next boot takes.
 */
enum smi_flash_kind {
	SMI_FLASH_IMAGE,
	SMI_FLASH_FPT,
	SMI_FLASH_COPY,
	SMI_FLASH_BOOT,
};

/**
 * struct smi_flash_op - One flash op, as every surface states it.
 * @kind: what it does.
 * @device: the target boot device; COPY's source.
 * @partition: the target partition; COPY's source, unread for FPT.
 * @dst_device: COPY's destination device.
 * @dst_partition: COPY's destination partition.
 * @image: IMAGE and FPT: the caller's image, @len bytes.
 * @len: its length.
 * @progress_efd: an eventfd told once for each chunk the card took, or -1.
 */
struct smi_flash_op {
	enum smi_flash_kind kind;
	uint32_t device;
	uint32_t partition;
	uint32_t dst_device;
	uint32_t dst_partition;
	const void __user *image;
	uint32_t len;
	int progress_efd;
};

/**
 * smi_flash_run() - Run one flash op on a card to its end.
 * @pf_dev: the card.
 * @op: the op.
 *
 * One op at a time per card, and none while a reset runs. A signal stops the
 * op until its first write; after it, the chunks wait through signals.
 *
 * Return: 0, or an errno: -ERESTARTSYS waiting for the lock, -EBUSY a reset
 *         in flight, -EINVAL an op the card cannot take or a value its wire
 *         cannot carry, -EOPNOTSUPP a card with no image verb, -EINTR a signal
 *         before the first write, else the card's or the transport's.
 */
int smi_flash_run(struct pf_dev_struct *pf_dev, const struct smi_flash_op *op);

#endif /* SMI_FLASH_H */
