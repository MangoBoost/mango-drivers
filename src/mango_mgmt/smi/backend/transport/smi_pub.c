// SPDX-License-Identifier: GPL-2.0
/*
 * smi_pub.c - One published section, copied out under its own sequence.
 *
 * A reader takes the seq, the body and the seq again, and keeps a copy whose
 * seq held even throughout. The seq is one aligned load each time: a byte copy
 * of it could pair halves of two values.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved. */

#include <asm/barrier.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/processor.h>
#include <linux/stddef.h>
#include <linux/string.h>

#include "smi_pub.h"
#include "smi_pub_sect.h"
#include "smi_transport.h"

/* Re-reads of a section the card is mid-write: odd lasts one copy, so a couple
 * is plenty; the bound keeps a card stuck mid-write from spinning a caller. */
#define SMI_PUB_READ_TRIES (4)

int smi_pub_read(struct smi_transport *tp, const struct smi_span *sec,
		 uint32_t boot, void *dst, size_t len)
{
	const struct smi_pub_sect *h = dst;
	unsigned int tries;
	uint32_t at, s0, s1;
	int ret;

	if (!tp || !sec || !dst)
		return -EINVAL;
	if (!sec->len)
		return -EOPNOTSUPP;
	if ((len < sizeof(*h)) || (len > sec->len))
		return -EINVAL;

	at = sec->start + offsetof(struct smi_pub_sect, seq);
	for (tries = 0; tries < SMI_PUB_READ_TRIES; tries++) {
		ret = smi_transport_ro_read32(tp, at, &s0);
		if (ret)
			return ret;
		if (!(s0 & 1u)) {
			rmb(); /* the body after the first seq */
			ret = smi_transport_ro_read(tp, sec->start, dst, len);
			if (ret)
				return ret;
			rmb(); /* the second seq after the body */
			ret = smi_transport_ro_read32(tp, at, &s1);
			if (ret)
				return ret;
			if ((s1 == s0) && (h->seq == s0)) {
				if (!h->fmt)
					return -ENODATA;
				return (h->boot == boot) ? 0 : -ESTALE;
			}
		}
		cpu_relax();
	}

	return -EAGAIN;
}

int smi_pub_status_head(struct smi_transport *tp,
			const struct smi_pub_layout *pub,
			struct smi_pub_status_head *out)
{
	static const struct smi_span none;
	const struct smi_span *sec;
	int ret;

	if (!pub || !out)
		return -EINVAL;

	sec = smi_pub_find(pub, SmiDescRegion_Status, 0);
	if (!sec)
		sec = &none;

	memset(out, 0, sizeof(*out));
	ret = smi_pub_read(tp, sec, pub->boot, out,
			   min_t(size_t, sizeof(*out), sec->len));
	if (ret)
		return ret;

	return smi_pub_accept(out, sizeof(*out), SMI_PUB_STATUS_FMT,
			      SMI_PUB_STATUS_BASELINE);
}
