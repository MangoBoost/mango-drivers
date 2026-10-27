/* SPDX-License-Identifier: GPL-2.0 */
/*
 * legacy_pub.h - The published facts UAPI v1 reads: Status's tail and the
 * counters.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_LEGACY_PUB_H
#define SMI_LEGACY_PUB_H

#include <linux/types.h>

#include "contract/smi/smi_abi.h" /* struct smi_pub_status, smi_desc_counter */

struct pf_dev_struct;

/* The most counter slots this surface reports. */
#define SMI_LEGACY_COUNTERS_MAX (16)

/**
 * struct smi_legacy_counters - The card's names for its counter slots.
 * @cnt: how many Counter records it states, read to SMI_LEGACY_COUNTERS_MAX.
 * @rec: each, as far as this build knows the record.
 */
struct smi_legacy_counters {
	uint8_t cnt;
	struct smi_desc_counter rec[SMI_LEGACY_COUNTERS_MAX];
};

/* One published counter slot: its value, and the card's name for it -- empty
 * where the card names none. */
struct published_counter {
	uint32_t value;
	char name[sizeof(((struct smi_desc_counter *)0)->name) + 1];
};

/**
 * read_published_counters() - The counter slots, as one observation.
 * @pf_dev: Pointer to PCI device data.
 * @out: receives each slot, in slot order.
 * @cap: how many @out holds.
 *
 * Return: how many slots were read, or an errno.
 */
int read_published_counters(struct pf_dev_struct *pf_dev,
			    struct published_counter *out, unsigned int cap);

/**
 * smi_legacy_counters_parse() - The Counter records of a descriptor chain.
 * @recs: the records, @len bytes.
 * @len: their length.
 * @out: receives them; empty where the chain has none it can trust.
 *
 * Return: 0, or -EPROTO for a Counter record short of its floor.
 */
int smi_legacy_counters_parse(const uint8_t *recs, size_t len,
			      struct smi_legacy_counters *out);

/* Read the card's counter names once, before any file reads them. */
void smi_legacy_counters_load(struct pf_dev_struct *pf_dev);

#endif /* SMI_LEGACY_PUB_H */
