// SPDX-License-Identifier: GPL-2.0
/**
 * @file legacy_qsfp.c
 * @brief The published QSFP cage plane (contract: legacy_qsfp.h)
 *
 * Copyright (c) 2026 MangoBoost Inc.
 */
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "contract/smi/smi_abi.h"

#include "core/smi_rpc.h"

#include "legacy_card.h"
#include "legacy_prims.h"
#include "legacy_qsfp.h"
#include "smi_log.h"
#include "smi_top.h"

/* The contract's names for a cage's state, an op and a power policy: a value
 * this build has no name for prints as its number rather than as a guess. */
#define SMI_X(name, v) [v] = #name,
static const char *const qsfp_state_words[] = { SMI_QSFP_STATE_LIST(SMI_X) };
static const char *const qsfp_op_words[] = { SMI_QSFP_OP_LIST(SMI_X) };
static const char *const qsfp_power_words[] = { SMI_QSFP_POWER_LIST(SMI_X) };
#undef SMI_X

static const char *qsfp_word(const char *const *words, size_t cnt, uint8_t v,
			     char *tmp, size_t n)
{
	if ((v < cnt) && words[v])
		return words[v];

	scnprintf(tmp, n, "%u", v);
	return tmp;
}

#define QSFP_WORD(words, v, tmp, n)                                            \
	qsfp_word(words, ARRAY_SIZE(words), v, tmp, n)

const char *smi_qsfp_state_word(uint8_t state, char *tmp, size_t n)
{
	return QSFP_WORD(qsfp_state_words, state, tmp, n);
}

const char *smi_qsfp_op_word(uint8_t op, char *tmp, size_t n)
{
	return QSFP_WORD(qsfp_op_words, op, tmp, n);
}

const char *smi_qsfp_power_word(uint8_t policy, char *tmp, size_t n)
{
	return QSFP_WORD(qsfp_power_words, policy, tmp, n);
}

bool smi_qsfp_len_ok(uint32_t len)
{
	return (len >= SMI_QSFP_HDR_BASELINE) && (len <= SMI_QSFP_BLK_MAX);
}

/* The bytes stay opaque here, so an unknown format still reads. */
int smi_qsfp_block(struct pf_dev_struct *pf_dev, uint8_t cage, uint32_t at,
		   void *buf, size_t len)
{
	struct smi_qsfp_store *s = &smi_legacy_card(pf_dev)->qsfp;

	if (!s->cnt)
		return -ENODEV; /* the plane is gone: detached at teardown */
	if (cage >= s->cnt)
		return -EINVAL;
	if (!len || (at > s->len[cage]) || (len > s->len[cage] - at))
		return -EINVAL;

	return smi_pub_section_read(pf_dev, SmiDescRegion_QsfpCage, cage, at,
				    buf, len);
}

int smi_qsfp_rw(struct pf_dev_struct *pf_dev, const struct smi_qsfp_rw_arg *arg,
		uint8_t *buf, uint32_t *val, uint32_t *used)
{
	if (!pf_dev || !arg || !buf || !val || !used)
		return -EINVAL;

	return smi_legacy_prims(pf_dev)->qsfp_rw(pf_dev, arg, buf, val, used);
}

/* The same session check the EEPROM pair makes, before a card that has no
 * module verb says so. */
static int module_rw(struct pf_dev_struct *pf_dev, bool write,
		     uint8_t device_id, uint8_t page, uint8_t offset,
		     uint8_t *buf, uint8_t buf_len)
{
	if (!pf_dev || !buf)
		return -EINVAL;
	if (!smi_pf_dev_ready(pf_dev) || !pf_dev->smc_ctx)
		return -ENODEV;

	return smi_legacy_prims(pf_dev)->module_rw(pf_dev, write, device_id,
						   page, offset, buf, buf_len);
}

int smi_pf_dev_module_read(struct pf_dev_struct *pf_dev, uint8_t device_id,
			   uint8_t page, uint8_t offset, uint8_t *buf,
			   uint8_t buf_len)
{
	return module_rw(pf_dev, false, device_id, page, offset, buf, buf_len);
}

int smi_pf_dev_module_write(struct pf_dev_struct *pf_dev, uint8_t device_id,
			    uint8_t page, uint8_t offset, uint8_t *buf,
			    uint8_t buf_len)
{
	return module_rw(pf_dev, true, device_id, page, offset, buf, buf_len);
}

void smi_qsfp_attach(struct pf_dev_struct *pf_dev)
{
	struct smi_qsfp_store *s = &smi_legacy_card(pf_dev)->qsfp;
	struct smi_pub_qsfp_cage hdr = { 0 };
	uint8_t i;
	int ret;

	memset(s, 0, sizeof(*s));

	if (!pf_dev->facts.publishes)
		return;

	/* Cages are numbered from 0; no cage is a zero count, not a failure. */
	for (i = 0; i < SMI_PUB_CAGES_MAX; i++) {
		ret = smi_pub_section_len(pf_dev, SmiDescRegion_QsfpCage, i);
		if (ret < 0)
			break;
		if (!smi_qsfp_len_ok(ret)) {
			SMI_DEV_WARN(pf_dev->dev,
				     "qsfp: refusing cage %u of %d B", i, ret);
			memset(s, 0, sizeof(*s));
			return;
		}
		s->len[i] = ret;
	}
	s->cnt = i;
	if (!s->cnt)
		return;

	/* Cage 0 speaks for every cage's format: an unknown one, or a block its
	 * header does not fit in, still reads raw but is not decoded. */
	ret = smi_qsfp_block(pf_dev, 0, 0, &hdr, SMI_QSFP_HDR_BASELINE);
	if (ret)
		goto fail;
	s->decoded = (hdr.sect.fmt == SMI_QSFP_FMT) &&
		     smi_qsfp_block_fits(&hdr, s->len[0]);
	if (!s->decoded)
		SMI_DEV_WARN(pf_dev->dev,
			     "qsfp: block format %u, header %u B: raw only",
			     hdr.sect.fmt, hdr.sect.hdr_len);

	SMI_DEV_INFO(pf_dev->dev, "qsfp: %u cage(s) of %u bytes (fmt %u)",
		     s->cnt, s->len[0], hdr.sect.fmt);
	return;

fail:
	SMI_DEV_WARN(pf_dev->dev, "qsfp: no cage plane (%d)", ret);
	memset(s, 0, sizeof(*s));
}

void smi_qsfp_detach(struct pf_dev_struct *pf_dev)
{
	struct smi_qsfp_store *s = &smi_legacy_card(pf_dev)->qsfp;

	memset(s, 0, sizeof(*s));
}
