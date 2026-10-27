// SPDX-License-Identifier: GPL-2.0
/*
 * legacy_pub.c - The published facts UAPI v1 reads: Status's tail and the
 * counters.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "core/smi_rpc.h"

#include "legacy_card.h"
#include "legacy_pub.h"
#include "smi_log.h"
#include "smi_pub_sect.h"
#include "smi_tlv.h"
#include "smi_top.h"

/* The record naming slot @slot, or NULL where the card names none. */
static const struct smi_desc_counter *
counter_named(const struct smi_legacy_counters *c, uint16_t slot)
{
	uint8_t i;

	for (i = 0; i < c->cnt; i++)
		if (c->rec[i].slot == slot)
			return &c->rec[i];
	return NULL;
}

int read_published_counters(struct pf_dev_struct *pf_dev,
			    struct published_counter *out, unsigned int cap)
{
	uint8_t buf[sizeof(struct smi_pub_counters) +
		    (SMI_LEGACY_COUNTERS_MAX * sizeof(uint32_t))] = { 0 };
	const struct smi_desc_counter *rec;
	struct smi_pub_counters hdr;
	unsigned int i, n;
	uint32_t len;
	int ret;

	if (!pf_dev || !out)
		return -EINVAL;

	ret = smi_pub_section_len(pf_dev, SmiDescRegion_Counters, 0);
	if (ret < 0)
		return ret;
	len = min_t(uint32_t, sizeof(buf), ret);
	ret = smi_pub_section_read(pf_dev, SmiDescRegion_Counters, 0, 0, buf,
				   len);
	if (ret)
		return ret;

	memcpy(&hdr, buf, sizeof(hdr));
	ret = smi_pub_accept(&hdr, sizeof(hdr), SMI_PUB_COUNTERS_FMT,
			     SMI_PUB_COUNTERS_BASELINE);
	if (ret)
		return ret;

	/* As many slots as the section states and this copy of it holds, each
	 * named by its Counter record where the card gives one. */
	n = min_t(unsigned int, hdr.cnt, cap);
	for (i = 0; (i < n) && (((uint64_t)hdr.val_off +
				 (uint64_t)(i + 1) * sizeof(uint32_t)) <= len);
	     i++) {
		memcpy(&out[i].value,
		       buf + hdr.val_off + (i * sizeof(uint32_t)),
		       sizeof(uint32_t));
		out[i].name[0] = '\0';
		rec = counter_named(&smi_legacy_card(pf_dev)->counters, i);
		if (rec)
			scnprintf(out[i].name, sizeof(out[i].name), "%.*s",
				  (int)sizeof(rec->name), rec->name);
	}

	return (int)i;
}

int smi_legacy_counters_parse(const uint8_t *recs, size_t len,
			      struct smi_legacy_counters *out)
{
	struct smi_tlv_rec r;
	size_t at = 0;
	int rem = (int)len;

	memset(out, 0, sizeof(*out));
	while ((out->cnt < SMI_LEGACY_COUNTERS_MAX) &&
	       smi_tlv_next(recs, &rem, &at, &r)) {
		if (r.type != SmiDesc_Counter)
			continue;
		/* A body short of its floor names no slot, not half of one. */
		if (!smi_tlv_take(&r, SMI_DESC_COUNTER_BASELINE,
				  &out->rec[out->cnt], sizeof(out->rec[0]))) {
			out->cnt = 0;
			return -EPROTO;
		}
		out->cnt++;
	}

	return 0;
}

void smi_legacy_counters_load(struct pf_dev_struct *pf_dev)
{
	struct smi_legacy_counters *c = &smi_legacy_card(pf_dev)->counters;
	size_t len = 0;
	uint8_t *recs;
	int ret;

	/* A card whose descriptor cannot be read names no slot. */
	recs = smi_desc_tool_records(pf_dev, &len);
	if (IS_ERR(recs))
		return;

	ret = smi_legacy_counters_parse(recs, len, c);
	kfree(recs);
	if (ret)
		SMI_DEV_WARN(pf_dev->dev,
			     "descriptor's counter records unreadable (%d)",
			     ret);
}
