// SPDX-License-Identifier: GPL-2.0
/*
 * smi_rpc.c - The card's own verbs, as a kernel caller reaches them.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include <linux/build_bug.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h> /* max(), min_t() */
#include <linux/mm.h> /* kvzalloc() */
#include <linux/slab.h>
#include <linux/string.h>

#include "backend/smi_smc_control.h"

#include "smi_pub_sect.h"
#include "smi_rpc.h"
#include "smi_top.h"

int smi_verb_call(struct pf_dev_struct *pf_dev, const struct smi_verb_req *req,
		  struct smi_verb_ans *ans)
{
	if (!pf_dev || !req || !ans)
		return -EINVAL;

	return smi_smc_call(pf_dev->smc_ctx, req, ans);
}

bool smi_verb_record(struct pf_dev_struct *pf_dev, uint16_t opcode,
		     struct smi_desc_verb *out)
{
	return pf_dev && smi_smc_verb(pf_dev->smc_ctx, opcode, out);
}

/* The largest slice one request can claim; 0 without a session. */
static uint32_t smi_verb_slice_bytes(struct pf_dev_struct *pf_dev)
{
	return pf_dev ? smi_smc_slice_bytes(pf_dev->smc_ctx) : 0u;
}

uint32_t smi_verb_cap(struct pf_dev_struct *pf_dev, uint16_t opcode)
{
	struct smi_desc_verb verb;

	if (!smi_verb_record(pf_dev, opcode, &verb))
		return 0;

	return smi_verb_stride(&verb, smi_verb_slice_bytes(pf_dev));
}

int smi_verb_run(struct pf_dev_struct *pf_dev, const struct smi_verb_req *req,
		 struct smi_verb_ans *ans)
{
	int ret = smi_verb_call(pf_dev, req, ans);

	return ret ? ret : smi_v2_status_to_errno(ans->status);
}

void *smi_desc_tool_records(struct pf_dev_struct *pf_dev, size_t *length)
{
	uint8_t *recs = NULL;
	int ret;

	if (!pf_dev || !length)
		return ERR_PTR(-EINVAL);

	ret = smi_smc_tool_records(pf_dev->smc_ctx, &recs, length);
	return ret ? ERR_PTR(ret) : recs;
}

int smi_pub_section_len(struct pf_dev_struct *pf_dev, uint16_t kind,
			uint16_t inst)
{
	const struct smi_span *sec =
		pf_dev ? smi_smc_pub_span(pf_dev->smc_ctx, kind, inst) : NULL;

	return sec ? (int)sec->len : -EOPNOTSUPP;
}

int smi_pub_section_read(struct pf_dev_struct *pf_dev, uint16_t kind,
			 uint16_t inst, uint32_t at, void *buf, size_t len)
{
	const struct smi_span *sec;
	uint8_t *whole;
	int ret;

	if (!pf_dev || !buf)
		return -EINVAL;

	sec = smi_smc_pub_span(pf_dev->smc_ctx, kind, inst);
	if (!sec || !sec->len)
		return -EOPNOTSUPP;
	if (!len || (at > sec->len) || (len > sec->len - at))
		return -EINVAL;

	if (!at && (len >= sizeof(struct smi_pub_sect)))
		return smi_smc_pub_read(pf_dev->smc_ctx, sec, buf, len);

	whole = kmalloc(sec->len, GFP_KERNEL);
	if (!whole)
		return -ENOMEM;
	ret = smi_smc_pub_read(pf_dev->smc_ctx, sec, whole, sec->len);
	if (!ret)
		memcpy(buf, whole + at, len);
	kfree(whole);

	return ret;
}

int smi_pub_status_read(struct pf_dev_struct *pf_dev,
			struct smi_pub_status *out)
{
	int len;
	int ret;

	if (!pf_dev || !out)
		return -EINVAL;

	len = smi_pub_section_len(pf_dev, SmiDescRegion_Status, 0);
	if (len < 0)
		return len;

	memset(out, 0, sizeof(*out));
	ret = smi_pub_section_read(pf_dev, SmiDescRegion_Status, 0, 0, out,
				   min_t(size_t, sizeof(*out), len));
	if (ret)
		return ret;

	return smi_pub_accept(out, sizeof(*out), SMI_PUB_STATUS_FMT,
			      SMI_PUB_STATUS_BASELINE);
}

bool smi_pub_busy(struct pf_dev_struct *pf_dev)
{
	struct smi_pub_status_head head;

	if (!pf_dev || smi_pf_dev_refusal(pf_dev))
		return false;

	return !smi_smc_pub_status_head(pf_dev->smc_ctx, &head) &&
	       (head.state == SmiPubState_Busy);
}
