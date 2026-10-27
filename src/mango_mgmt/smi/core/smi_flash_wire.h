/* SPDX-License-Identifier: GPL-2.0 */
/*
 * smi_flash_wire.h - A card generation's flash verbs, one table per wire.
 *
 * What the burn service asks of a card. Each generation answers in its own
 * file, and a card gets the table its settled wire takes.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_FLASH_WIRE_H
#define SMI_FLASH_WIRE_H

#include <linux/types.h>

struct pf_dev_struct;

/* One packet of an image: its byte offset, the target, and whether it is the
 * one the stream ends on. @part is unread for the FPT. */
struct smi_chunk_arg {
	uint32_t off;
	uint8_t boot;
	uint8_t part;
	bool fpt;
	bool last;
};

/**
 * struct smi_flash_wire - One card generation's flash verbs.
 * @chunk_bytes: the largest packet an image download sends; 0 where none.
 * @chunk_write: one packet, @len bytes at @buf.
 * @copy_start: copy one partition; @op_id is the background copy to follow,
 *		0 when the copy already ran.
 * @boot_select: the partition the next boot takes.
 * @part_max: the largest partition number its payloads carry.
 */
struct smi_flash_wire {
	uint32_t (*chunk_bytes)(struct pf_dev_struct *pf_dev);
	int (*chunk_write)(struct pf_dev_struct *pf_dev,
			   const struct smi_chunk_arg *arg, const uint8_t *buf,
			   uint32_t len);
	int (*copy_start)(struct pf_dev_struct *pf_dev, uint8_t src_device,
			  uint8_t src_part, uint8_t dest_device,
			  uint8_t dest_part, uint32_t *op_id);
	int (*boot_select)(struct pf_dev_struct *pf_dev, uint32_t partition);
	uint8_t part_max;
};

extern const struct smi_flash_wire smi_flash_wire_v2;

/* The table this card's settled wire takes. */
const struct smi_flash_wire *smi_flash_wire(struct pf_dev_struct *pf_dev);

#endif /* SMI_FLASH_WIRE_H */
