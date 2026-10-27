/* SPDX-License-Identifier: GPL-2.0 */
/*
 * smi_verb.h - A submission as a kernel caller and the engine share it.
 *
 * A verb the caller names, bounded by the card's Verb record, and the card's
 * answer head, its status read as an errno where the caller wants one.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_VERB_H
#define SMI_VERB_H

#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/kernel.h> /* min(), min_t(), U32_MAX */
#include <linux/string.h>
#include <linux/types.h>

#include "contract/smi/smi_abi.h" /* struct smi_desc_verb */
#include "contract/smi/smi_abi_v2.h" /* SMI_V2_PAY_MAX, SMI_V2_STATUS_LIST */

/**
 * struct smi_verb_req - A verb the caller names.
 * @opcode: the card's own verb; its Verb record bounds all below.
 * @pay_len: payload bytes in @pay; the card is sent as much as it understands.
 * @pay: the payload, as the contract lays it out.
 * @in: bytes staged into the slice, @in_len of them.
 * @in_len: 0 when nothing travels to the card.
 * @out: where the slice is read back, up to @out_cap bytes.
 * @out_cap: 0 when nothing comes back. The slice is the larger of the two.
 * @name: what the log calls it.
 * @refusal_is_answer: a refusal logs at debug, as the caller decides on it.
 * @ctx: the host context it is sent as; 0, the kernel, for every kernel caller.
 */
struct smi_verb_req {
	uint16_t opcode;
	uint16_t pay_len;
	uint8_t pay[SMI_V2_PAY_MAX];
	const void *in;
	uint32_t in_len;
	void *out;
	uint32_t out_cap;
	const char *name;
	bool refusal_is_answer;
	uint8_t ctx;
};

/**
 * struct smi_verb_ans - The card's answer head, filled whether it refused or not.
 * @status: SmiV2Status_*, not an errno.
 * @val: the verb's own scalar.
 * @detail: the card's syndrome for a refusal.
 * @win_used: the bytes the card wrote into the slice.
 */
struct smi_verb_ans {
	uint16_t status;
	uint32_t val;
	uint32_t detail;
	uint32_t win_used;
};

/* The payload a verb takes, copied into the request that carries it. */
#define SMI_VERB_PAY(req, pay)                                                 \
	do {                                                                   \
		static_assert(sizeof(pay) <= SMI_V2_PAY_MAX,                   \
			      "a payload fits the frame");                     \
		memcpy((req)->pay, &(pay), sizeof(pay));                       \
		(req)->pay_len = sizeof(pay);                                  \
	} while (0)

/* The slice a verb may claim: its Verb's window, within the slot. */
static inline uint32_t smi_verb_window(const struct smi_desc_verb *verb,
				       uint32_t slot)
{
	return (U32_MAX == verb->win_max) ? slot : min(verb->win_max, slot);
}

/* The most requests that follow each other may each take: the window in whole
 * units of the Verb's offset alignment, so every next offset stays aligned. */
static inline uint32_t smi_verb_stride(const struct smi_desc_verb *verb,
				       uint32_t slot)
{
	const uint32_t win = smi_verb_window(verb, slot);

	return verb->off_align ? (win / verb->off_align) * verb->off_align
			       : win;
}

/**
 * smi_v2_pay_fit() - How much of a payload goes to the card.
 * @pay: the payload a builder filled.
 * @pay_len: its length.
 * @card_bytes: the payload bytes the card's Verb record says it understands.
 *
 * A field past what the card understands may only be zero -- the card reads the
 * frame without it -- so a call that needs one is refused.
 *
 * Return: the bytes to send, or -EOPNOTSUPP when one past @card_bytes is set.
 */
static inline int smi_v2_pay_fit(const void *pay, uint16_t pay_len,
				 uint16_t card_bytes)
{
	const uint8_t *p = pay;
	uint16_t i;

	for (i = card_bytes; i < pay_len; i++)
		if (p[i])
			return -EOPNOTSUPP;

	return min_t(uint16_t, pay_len, card_bytes);
}

/**
 * smi_v2_status_to_errno() - The Linux errno for the card's status.
 * @status: SmiV2Status_* as the answer carried it.
 *
 * The shared list's errno column; a code this build was never taught answers
 * its class's errno.
 *
 * Return: the column's errno, 0 for Success.
 */
static inline int smi_v2_status_to_errno(uint16_t status)
{
#define SMI_X(name, v, err)                                                    \
	case (v):                                                              \
		return (err);
	switch (status) {
		SMI_V2_STATUS_LIST(SMI_X)
	}
#undef SMI_X

	if (status > 0xFF)
		return -EPROTO;
	switch (SMI_V2_STATUS_CLASS(status)) {
	case SmiV2Class_Refused:
		return -EINVAL;
	case SmiV2Class_Retry:
		return -EAGAIN;
	case SmiV2Class_Failed:
		return -EIO;
	default:
		return -EPROTO;
	}
}

#endif /* SMI_VERB_H */
