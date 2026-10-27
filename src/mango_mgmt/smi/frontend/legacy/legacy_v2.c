// SPDX-License-Identifier: GPL-2.0
/*
 * legacy_v2.c - UAPI v1's requests, as a v2 card's own verbs.
 *
 * Each fills the contract's payload, names its slice and reads the card's
 * answer, as a caller in user space would.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "contract/smi/smi_abi_v2.h"
#include "core/smi_rpc.h"

#include "legacy_prims.h"
#include "smi_log.h"
#include "smi_top.h"

/* The window carries the bytes (read answer, write data, ack mask, power byte,
 * CDB page), so the payload holds only cage, range and expected generations. */
static int v2_qsfp_rw(struct pf_dev_struct *pf_dev,
		      const struct smi_qsfp_rw_arg *arg, uint8_t *buf,
		      uint32_t *val, uint32_t *used)
{
	struct smi_verb_req req = { .opcode = SmiV2Opcode_QsfpRw,
				    .in = buf,
				    .in_len = arg->len,
				    .out = buf,
				    .out_cap = arg->len,
				    .name = "qsfp_rw",
				    /* A refusal is the card's answer for the
				     * caller to act on */
				    .refusal_is_answer = true };
	struct smi_v2_qsfp_pay pay = { .cage = arg->cage,
				       .bank = arg->bank,
				       .page = arg->page,
				       .offset = arg->offset,
				       .len = arg->len,
				       .op = arg->op,
				       .expected_control_gen = arg->control_gen,
				       .expected_module_epoch = arg->epoch };
	struct smi_verb_ans ans = { 0 };
	int ret;

	SMI_VERB_PAY(&req, pay);
	ret = smi_verb_run(pf_dev, &req, &ans);

	/* An answer fills the whole window or none of it (smi_abi_qsfp.h) */
	if (!ret && ans.win_used && (ans.win_used != arg->len))
		ret = -EPROTO;
	*val = ans.val;
	*used = ret ? 0 : ans.win_used;

	return ret;
}

/* The line and its answer share one slice: the line goes in at its head and the
 * answer overwrites it. A truncated answer is not a failure: val says what did
 * not fit, and asking again re-runs the line rather than resuming it. */
static int v2_shell_exec(struct pf_dev_struct *pf_dev, uint8_t *buf,
			 uint32_t cap, uint32_t line_len, uint32_t *out_len,
			 uint32_t *dropped)
{
	struct smi_verb_req req = { .opcode = SmiV2Opcode_ShellExec,
				    .in = buf,
				    .in_len = line_len,
				    .out = buf,
				    .name = "shell_exec" };
	struct smi_v2_shex_pay pay = { .cmd_len = (uint16_t)line_len };
	const uint32_t room = smi_verb_cap(pf_dev, SmiV2Opcode_ShellExec);
	struct smi_verb_ans ans = { 0 };
	int ret;

	if (!room)
		return -EOPNOTSUPP; /* the card has no such verb */

	/* As much answer as the slot holds, as a slice cut to it answered. */
	req.out_cap = min(cap, room);
	if (line_len > req.out_cap)
		return -EINVAL;

	SMI_VERB_PAY(&req, pay);
	ret = smi_verb_run(pf_dev, &req, &ans);
	if (ret)
		return ret;

	*out_len = ans.win_used;
	*dropped = ans.val;
	return 0;
}

static int v2_log_level(struct pf_dev_struct *pf_dev, uint8_t level)
{
	struct smi_verb_req req = { .opcode = SmiV2Opcode_LogLevel,
				    .name = "log_level" };
	struct smi_v2_lvl_pay pay = { 0 };
	struct smi_verb_ans ans = { 0 };

	/* Hash 0 is the global gate: naming one module needs its tag, and no
	 * caller upstream of the ioctl has one to give. */
	pay.level = level;
	SMI_VERB_PAY(&req, pay);
	return smi_verb_run(pf_dev, &req, &ans);
}

static int v2_led(struct pf_dev_struct *pf_dev, const uint8_t *payload,
		  uint32_t len)
{
	struct smi_verb_req req = { .opcode = SmiV2Opcode_LedCtl,
				    .name = "led" };
	struct smi_v2_led_pay pay;
	struct smi_verb_ans ans = { 0 };

	if (len < sizeof(pay))
		return -EINVAL;
	memcpy(&pay, payload, sizeof(pay));
	pay.rsvd = 0;

	SMI_VERB_PAY(&req, pay);
	return smi_verb_run(pf_dev, &req, &ans);
}

/* No module verb: the cages are the QSFP window's. */
static int v2_module_rw(struct pf_dev_struct *pf_dev, bool write,
			uint8_t device_id, uint8_t page, uint8_t offset,
			uint8_t *buf, uint8_t len)
{
	return -EOPNOTSUPP;
}

const struct smi_legacy_prims smi_legacy_v2_prims = {
	.qsfp_rw = v2_qsfp_rw,
	.shell_exec = v2_shell_exec,
	.log_level = v2_log_level,
	.led = v2_led,
	.module_rw = v2_module_rw,
};
