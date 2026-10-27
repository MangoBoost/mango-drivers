/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * smi_tlv.h - Walk a chain of {type,len} records.
 *
 * Every chain a card answers is the same encoding, so one cursor walks them
 * all. Liberal in semantics, strict in syntax: an unknown type is stepped over
 * by its length and a known record may be longer than this build compiles, but
 * a header fragment, a body past the end or a record short of its baseline is
 * a malformed chain, not an older card.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_TLV_H
#define SMI_TLV_H

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/types.h>

#include "contract/smi/smi_abi.h"

/**
 * struct smi_tlv_rec - One record: its header, and where its body is.
 * @type: the list its chain belongs to names the value.
 * @len: body bytes, which may exceed what this build knows of that type.
 * @body: the body, inside the caller's buffer.
 */
struct smi_tlv_rec {
	uint16_t type;
	uint16_t len;
	const uint8_t *body;
};

/**
 * smi_tlv_next() - Take the next record of the chain.
 * @buf: the chain's first byte.
 * @rem: bytes left; signed, so an overrun is caught rather than wrapped.
 * @at: cursor into @buf; 0 starts, left past the record taken.
 * @out: the record taken.
 *
 * Silent: it stops at anything it cannot take, and the caller judges the chain
 * once by @rem afterwards -- zero is a clean end, anything else a broken tail.
 *
 * Return: true while a record was taken.
 */
static inline bool smi_tlv_next(const uint8_t *buf, int *rem, size_t *at,
				struct smi_tlv_rec *out)
{
	struct smi_tlv_hdr hdr;

	if (*rem < (int)sizeof(hdr))
		return false;

	memcpy(&hdr, buf + *at, sizeof(hdr));
	if ((int)hdr.len > (*rem - (int)sizeof(hdr)))
		return false;

	out->type = hdr.type;
	out->len = hdr.len;
	out->body = buf + *at + sizeof(hdr);
	*at += sizeof(hdr) + hdr.len;
	*rem -= (int)sizeof(hdr) + (int)hdr.len;

	return true;
}

/**
 * smi_tlv_take() - Copy a record's body into a build's own struct.
 *
 * Shorter than its baseline is a body no reader can trust; longer is read as
 * far as this build knows it, which is how a card grows a record.
 *
 * Return: true when the body was taken.
 */
static inline bool smi_tlv_take(const struct smi_tlv_rec *r, uint16_t min,
				void *dst, size_t cap)
{
	if (r->len < min)
		return false;

	memset(dst, 0, cap);
	memcpy(dst, r->body, min_t(size_t, cap, (size_t)r->len));

	return true;
}

#endif /* SMI_TLV_H */
