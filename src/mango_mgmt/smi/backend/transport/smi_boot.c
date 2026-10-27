// SPDX-License-Identifier: GPL-2.0
/*
 * smi_boot.c - Bootstrap selector: the magic at offset 0 picks a dialect.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved. */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "smi_boot.h"
#include "smi_boot_v2.h"
#include "smi_log.h"
#include "smi_transport.h"

/* Firmware writes the descriptor while it boots, erasing its magic first and
 * writing it last, so nothing is there until the carve completes -- wait as
 * long as for serving, which the same boot settles. */
#define SMI_BOOT_POLL_INTERVAL_MS (100)
#define SMI_BOOT_POLL_RETRIES (300) /* 30 s */

/* Then wait for the card to start SERVING, later than having a descriptor. A
 * window read, so it may look often. */
#define SMI_BOOT_SERVE_INTERVAL_MS (200)
#define SMI_BOOT_SERVE_RETRIES                                                 \
	(SMI_BOOT_SERVE_BUDGET_MS / SMI_BOOT_SERVE_INTERVAL_MS)

/* Dialect registry: a new descriptor generation is one more row here. */
static const struct smi_boot_dialect *const smi_boot_dialects[] = {
	&smi_boot_dialect_v2,
};

/* The dialect whose magic the window states, or NULL, with both magics read
 * into @own and @shared: a dialect names which is its own. All-ones (decode
 * off) and zeroes (still booting) are no magic. */
static const struct smi_boot_dialect *
smi_boot_dialect_of(struct smi_transport *tp, uint32_t *own, uint32_t *shared)
{
	size_t i;

	if (smi_transport_window_read(tp, 0, own, sizeof(*own)))
		*own = 0;
	if (smi_transport_ro_read(tp, 0, shared, sizeof(*shared)))
		*shared = 0;

	for (i = 0; i < ARRAY_SIZE(smi_boot_dialects); i++) {
		const struct smi_boot_dialect *d = smi_boot_dialects[i];

		if (d->magic == (d->shared_magic ? *shared : *own))
			return d;
	}

	return NULL;
}

bool smi_boot_window_answers(struct smi_transport *tp)
{
	uint32_t own, shared;

	if (!tp)
		return false;

	/* A window not yet claimed at attach cannot be held to a magic. */
	if (!smi_transport_bootstrapped(tp))
		return true;

	return smi_boot_dialect_of(tp, &own, &shared) != NULL;
}

int smi_boot_tool_records(struct smi_transport *tp, uint8_t **out, size_t *len)
{
	const struct smi_boot_dialect *d;
	uint32_t own, shared;

	if (!tp || !out || !len)
		return -EINVAL;

	d = smi_boot_dialect_of(tp, &own, &shared);
	if (!d)
		return -EOPNOTSUPP;
	if (d->records)
		return d->records(tp, out, len);

	*len = 0;
	*out = kmalloc(0, GFP_KERNEL);
	return 0;
}

/**
 * smi_boot_try() - One pass: read the magic, hand the window to its dialect.
 *
 * Return: 0 when a dialect answered, -EOPNOTSUPP when no dialect claims the
 * magic, or the dialect's error (-EAGAIN = present but not serving yet).
 */
static int smi_boot_try(struct smi_transport *tp, struct smi_layout *out_layout,
			struct gcq_partition_layout *out_gcq_layout,
			struct smi_log_view *out_log_view,
			const struct smi_boot_dialect **out_dialect)
{
	struct device *dev = smi_transport_dev(tp);
	const struct smi_boot_dialect *dialect;
	uint32_t own, shared;
	int ret;

	dialect = smi_boot_dialect_of(tp, &own, &shared);
	if (!dialect) {
		SMI_DEV_VDBG(
			dev,
			"No dialect claims magic 0x%x (own) / 0x%x (shared)",
			own, shared);
		return -EOPNOTSUPP;
	}

	ret = dialect->probe(tp, out_layout, out_gcq_layout);
	if (!ret) {
		/* A log this dialect cannot describe is a log this host does
		 * not read: zeroed rather than half-stated. */
		memset(out_log_view, 0, sizeof(*out_log_view));
		if (dialect->log_view && dialect->log_view(tp, out_log_view))
			memset(out_log_view, 0, sizeof(*out_log_view));
		if (out_dialect)
			*out_dialect = dialect;
		SMI_DEV_VDBG(
			dev,
			"%s window: data 0x%x+0x%x, boot %u, %u section(s), generations 0x%x",
			dialect->name, out_layout->data.start,
			out_layout->data.len, out_layout->pub.boot,
			out_layout->pub.region_cnt, out_layout->gens);
	}
	return ret;
}

/**
 * smi_boot_await_service() - Hold until the card answers frames, or give up.
 *
 * The descriptor being there is not the card being ready: the task that reads
 * frames starts after the window is laid out, and sending before then costs a
 * timeout per command and a heartbeat warning to hot-reset a booting card.
 *
 * Never fatal: a card past the budget is reported and attached anyway.
 */
static void smi_boot_await_service(struct smi_transport *tp,
				   const struct smi_boot_dialect *dialect,
				   const struct smi_layout *layout)
{
	struct device *dev = smi_transport_dev(tp);
	int i;

	if (!dialect || !dialect->serving)
		return;

	for (i = 0; i < SMI_BOOT_SERVE_RETRIES; i++) {
		if (dialect->serving(tp, layout)) {
			if (i)
				SMI_DEV_INFO(dev, "card serving after %d ms",
					     SMI_BOOT_SERVE_INTERVAL_MS * i);
			return;
		}
		msleep(SMI_BOOT_SERVE_INTERVAL_MS);
	}

	SMI_DEV_WARN(dev, "card not serving after %d ms; attaching anyway",
		     SMI_BOOT_SERVE_INTERVAL_MS * SMI_BOOT_SERVE_RETRIES);
}

/**
 * smi_boot_no_descriptor() - Why a run of passes ended, in one line.
 *
 * @last is smi_boot_try()'s final verdict. -EOPNOTSUPP is "no dialect claims
 * the magic", which waiting cannot change, so it is not answered as a timeout.
 */
static int smi_boot_no_descriptor(struct smi_transport *tp, int last)
{
	const int ms = SMI_BOOT_POLL_INTERVAL_MS * SMI_BOOT_POLL_RETRIES;
	struct device *dev = smi_transport_dev(tp);
	uint32_t magic = 0;
	int ret;

	if (last != -EOPNOTSUPP) {
		SMI_DEV_ERR(dev, "No descriptor after %d ms: not ready (%d)",
			    ms, last);
		return -ETIMEDOUT;
	}

	/* Read here rather than carried out of the loop: the run has ended and
	 * this is the only place that reports the word. */
	ret = smi_transport_window_read(tp, 0, &magic, sizeof(magic));
	if (ret || !magic) {
		SMI_DEV_ERR(dev, "No descriptor after %d ms: offset 0 %s", ms,
			    ret ? "unreadable" : "is zero");
		return -ETIMEDOUT;
	}

	/* The magic marks a bootstrap break, not a generation -- generations
	 * ride gens -- so an unknown one is rare and the value is the whole
	 * diagnosis. The dialects this build reads are the table above. */
	SMI_DEV_ERR(dev, "No descriptor after %d ms: magic 0x%08x unknown", ms,
		    magic);

	return -EOPNOTSUPP;
}

int smi_boot_read(struct smi_transport *tp, struct smi_layout *out_layout,
		  struct gcq_partition_layout *out_gcq_layout,
		  struct smi_log_view *out_log_view)
{
	const struct smi_boot_dialect *dialect = NULL;
	int ret;

	if (!tp || !out_layout || !out_gcq_layout || !out_log_view)
		return -EINVAL;

	ret = smi_boot_try(tp, out_layout, out_gcq_layout, out_log_view,
			   &dialect);
	if (!ret)
		smi_boot_await_service(tp, dialect, out_layout);

	return ret;
}

int smi_boot_probe(struct smi_transport *tp, struct smi_layout *out_layout,
		   struct gcq_partition_layout *out_gcq_layout,
		   struct smi_log_view *out_log_view)
{
	const struct smi_boot_dialect *dialect = NULL;
	int ret = -EOPNOTSUPP;
	int i;

	if (!tp || !out_layout || !out_gcq_layout || !out_log_view)
		return -EINVAL;

	/* Asked before the first sleep, so a card already up costs no interval
	 * and the line below reports the time actually spent. */
	for (i = 0; i < SMI_BOOT_POLL_RETRIES; i++) {
		ret = smi_boot_try(tp, out_layout, out_gcq_layout, out_log_view,
				   &dialect);
		if (!ret) {
			SMI_DEV_VDBG(smi_transport_dev(tp),
				     "Window descriptor ready after %d ms",
				     SMI_BOOT_POLL_INTERVAL_MS * i);
			smi_boot_await_service(tp, dialect, out_layout);
			return 0;
		}
		msleep(SMI_BOOT_POLL_INTERVAL_MS);
	}

	return smi_boot_no_descriptor(tp, ret);
}
