// SPDX-License-Identifier: GPL-2.0
/*
 * smi_flash_v2.c - The flash verbs as a v2 card's own.
 *
 * Each fills the contract's payload, names its slice and reads the card's
 * answer, a refusal as its status's errno.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include <linux/errno.h>
#include <linux/kernel.h>

#include "contract/smi/smi_abi_v2.h"

#include "smi_flash_wire.h"
#include "smi_log.h"
#include "smi_rpc.h"
#include "smi_top.h"

static uint32_t v2_chunk_bytes(struct pf_dev_struct *pf_dev)
{
	return smi_verb_cap(pf_dev, SmiV2Opcode_FlashWrite);
}

/* FlashWrite's last answer carries the verify verdict: VerifyFailed as the
 * status, and a skipped verify as a bit in val beside Success. */
static int v2_chunk_write(struct pf_dev_struct *pf_dev,
			  const struct smi_chunk_arg *arg, const uint8_t *buf,
			  uint32_t len)
{
	struct smi_verb_req req = { .opcode = SmiV2Opcode_FlashWrite,
				    .in = buf,
				    .in_len = len,
				    .name = "download_pdi" };
	struct smi_v2_wr_pay pay = { 0 };
	struct smi_verb_ans ans = { 0 };
	int ret;

	/* The byte offset rides the wire as it is; the card checks it against
	 * its Verb's alignment. A partition table has a bit of its own. */
	pay.off = arg->off;
	pay.partition = arg->fpt ? 0 : arg->part;
	pay.device = arg->boot;
	pay.flags = (arg->last ? SMI_WR_LAST_PACKET : 0) |
		    (arg->fpt ? SMI_WR_UPDATE_FPT : 0);
	SMI_VERB_PAY(&req, pay);

	ret = smi_verb_run(pf_dev, &req, &ans);
	if (!ret && arg->last && (ans.val & SMI_WR_ANS_UNVERIFIED))
		SMI_DEV_WARN(pf_dev->dev, "image not verified by the card");

	return ret;
}

/* A copy is always the background's: Accepted carries the op id its progress
 * is read against. */
static int v2_copy_start(struct pf_dev_struct *pf_dev, uint8_t src_device,
			 uint8_t src_part, uint8_t dest_device,
			 uint8_t dest_part, uint32_t *op_id)
{
	struct smi_verb_req req = { .opcode = SmiV2Opcode_FlashCopy,
				    .name = "copy_partition" };
	struct smi_v2_cp_pay pay = { .src_device = src_device,
				     .src_partition = src_part,
				     .dst_device = dest_device,
				     .dst_partition = dest_part };
	struct smi_verb_ans ans = { 0 };
	int ret;

	/* No window: the card stages the packet in its own memory and the copy
	 * is the whole source partition. */
	SMI_VERB_PAY(&req, pay);
	ret = smi_verb_call(pf_dev, &req, &ans);
	if (ret)
		return ret;

	if ((SmiV2Status_Accepted == ans.status) && ans.val) {
		*op_id = ans.val;
		return 0;
	}
	if ((SmiV2Status_Accepted == ans.status) ||
	    (SmiV2Status_Success == ans.status)) {
		/* Taken with no op id to follow. */
		SMI_DEV_ERR_RATELIMITED(pf_dev->dev,
					"%s response failed (%d, detail %u)",
					req.name, -EPROTO, ans.detail);
		return -EPROTO;
	}

	return smi_v2_status_to_errno(ans.status);
}

static int v2_boot_select(struct pf_dev_struct *pf_dev, uint32_t partition)
{
	struct smi_verb_req req = { .opcode = SmiV2Opcode_BootSelect,
				    .name = "device_boot" };
	struct smi_v2_boot_pay pay = { .partition = (uint8_t)partition };
	struct smi_verb_ans ans = { 0 };

	SMI_VERB_PAY(&req, pay);
	return smi_verb_run(pf_dev, &req, &ans);
}

const struct smi_flash_wire smi_flash_wire_v2 = {
	.chunk_bytes = v2_chunk_bytes,
	.chunk_write = v2_chunk_write,
	.copy_start = v2_copy_start,
	.boot_select = v2_boot_select,
	.part_max = U8_MAX, /* a byte in every flash payload */
};
