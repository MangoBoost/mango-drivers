// SPDX-License-Identifier: GPL-2.0
/*
 * smi_fpt.c - The card's partition tables, as the legacy UAPI keeps them.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include <linux/bitmap.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "contract/smi/smi_abi_v2.h"

#include "backend/smi_smc_control.h" /* smi_smc_repo_read() */
#include "backend/smi_v2_repo.h" /* the one Describe a v2 card answers */

#include "smi_fpt.h"
#include "smi_tlv.h"
#include "smi_top.h"

/* A partition names its slot in a byte, so a device holds at most this many
 * and a chain that offers more cannot number them apart. */
#define FPT_SLOTS (256)

/* What the chain holds per device, counted before anything is allocated. */
struct fpt_census {
	unsigned int parts[SMI_SMC_BOOT_DEVICE_MAX];
	unsigned int hdrs[SMI_SMC_BOOT_DEVICE_MAX];
	unsigned int total_parts;
};

/* Which device a flash record names, or -1 for one this host does not serve --
 * counted nowhere and landed nowhere, like any other record it cannot use. The
 * device byte opens both flash records. */
static int fpt_device(const struct smi_tlv_rec *r)
{
	uint8_t device = 0;

	memcpy(&device, r->body, sizeof(device));

	return (device < SMI_SMC_BOOT_DEVICE_MAX) ? (int)device : -1;
}

/* The same walk judges the chain: a malformed tail, a flash record short of its
 * baseline or another repository's record ends the read. */
static int fpt_count(const uint8_t *buf, size_t len, struct fpt_census *c)
{
	struct smi_tlv_rec r;
	size_t at = 0;
	int rem = (int)len;
	int dev;

	memset(c, 0, sizeof(*c));
	while (smi_tlv_next(buf, &rem, &at, &r)) {
		switch (r.type) {
		case SmiTlv_FptHeader:
			if (r.len < SMI_TLV_FPT_HEADER_BASELINE)
				return -EPROTO;
			dev = fpt_device(&r);
			if (dev >= 0)
				c->hdrs[dev]++;
			break;
		case SmiTlv_Partition:
			if (r.len < SMI_TLV_PARTITION_BASELINE)
				return -EPROTO;
			dev = fpt_device(&r);
			if (dev >= 0) {
				c->parts[dev]++;
				c->total_parts++;
			}
			break;
		case SmiTlv_Sensor:
		case SmiTlv_BoardField:
			return -EPROTO;
		default:
			break; /* a record this build does not know */
		}
	}

	return rem ? -EPROTO : 0;
}

/* One device, one table: a device states its header exactly once, and a
 * partition belongs to a device that stated one. */
static int fpt_shape(const struct fpt_census *c)
{
	unsigned int d;

	for (d = 0; d < SMI_SMC_BOOT_DEVICE_MAX; d++) {
		if (c->hdrs[d] > 1)
			return -EPROTO;
		if (c->parts[d] && !c->hdrs[d])
			return -EPROTO;
		if (c->parts[d] > FPT_SLOTS)
			return -EPROTO;
	}

	return 0;
}

/* Land one flash record in the slot it names: a slot outside its device's
 * table, or one already filled, is no table. */
static int fpt_fill(struct smi_fpt *fpt, const struct fpt_census *c,
		    unsigned long (*seen)[BITS_TO_LONGS(FPT_SLOTS)],
		    const struct smi_tlv_rec *r)
{
	const int dev = fpt_device(r);

	if (dev < 0)
		return 0;

	if (r->type == SmiTlv_FptHeader) {
		struct smi_tlv_fpt_header w = { 0 };

		if (!smi_tlv_take(r, SMI_TLV_FPT_HEADER_BASELINE, &w,
				  sizeof(w)))
			return -EPROTO;

		/* The count a host reports is the one the chain proves. */
		if (w.num_entries != c->parts[dev])
			return -EPROTO;

		fpt->hdr[dev].version = w.version;
		fpt->hdr[dev].header_size = w.hdr_size;
		fpt->hdr[dev].entry_size = w.entry_size;
		fpt->hdr[dev].num_entries = w.num_entries;
		return 0;
	}

	{
		struct smi_tlv_partition w = { 0 };
		struct smi_partition *p;

		if (!smi_tlv_take(r, SMI_TLV_PARTITION_BASELINE, &w, sizeof(w)))
			return -EPROTO;

		if ((w.index >= c->parts[dev]) ||
		    test_and_set_bit(w.index, seen[dev]))
			return -EPROTO;

		p = &fpt->part[dev][w.index];
		p->type = w.type;
		p->base_addr = w.base;
		p->partition_size = w.size;
	}

	return 0;
}

int smi_fpt_parse(struct device *dev, const uint8_t *buf, size_t len,
		  struct smi_fpt **out)
{
	unsigned long seen[SMI_SMC_BOOT_DEVICE_MAX][BITS_TO_LONGS(FPT_SLOTS)] = {
		0
	};
	struct smi_partition *at;
	struct fpt_census c;
	struct smi_tlv_rec r;
	struct smi_fpt *fpt;
	size_t pos = 0;
	unsigned int d;
	int rem = (int)len;
	int ret;

	ret = fpt_count(buf, len, &c);
	if (!ret)
		ret = fpt_shape(&c);
	if (ret)
		return ret;

	/* The census sized the block, so a device's segment is fixed before a
	 * record lands; a device with no entries keeps a NULL. */
	fpt = devm_kzalloc(dev, sizeof(*fpt) + c.total_parts * sizeof(*at),
			   GFP_KERNEL);
	if (!fpt)
		return -ENOMEM;
	at = (struct smi_partition *)(fpt + 1);
	for (d = 0; d < SMI_SMC_BOOT_DEVICE_MAX; d++) {
		if (!c.parts[d])
			continue;
		fpt->part[d] = at;
		at += c.parts[d];
	}

	while (smi_tlv_next(buf, &rem, &pos, &r)) {
		if ((r.type != SmiTlv_FptHeader) &&
		    (r.type != SmiTlv_Partition))
			continue;
		ret = fpt_fill(fpt, &c, seen, &r);
		if (ret) {
			devm_kfree(dev, fpt);
			return ret;
		}
	}

	*out = fpt;
	return 0;
}

/* The tables as the card describes them, sized by the backend's one ask. */
static int fpt_read_v2(struct pf_dev_struct *pf_dev, struct smi_fpt **fpt,
		       uint8_t *num_records)
{
	uint8_t *chain = NULL;
	uint32_t len = 0;
	int ret;

	ret = smi_v2_describe(pf_dev->smc_ctx, SmiRepo_Fpt, &chain, &len);
	if (ret)
		return ret;

	/* A table with no entries is still a table this card has. */
	ret = smi_fpt_parse(pf_dev->dev, chain, len, fpt);
	if (!ret)
		*num_records = 1;

	vfree(chain);
	return ret;
}

int smi_fpt_load(struct pf_dev_struct *pf_dev, struct smi_fpt_copy *f)
{
	struct smi_fpt *fpt = NULL;
	uint8_t n = 0;
	int ret;

	ret = fpt_read_v2(pf_dev, &fpt, &n);

	mutex_lock(&f->lock);
	if (!ret) {
		if (f->fpt)
			devm_kfree(pf_dev->dev, f->fpt);
		f->fpt = fpt;
		f->num_records = n;
		f->state = SMI_REPO_PRESENT;
		f->last_error = 0;
	} else if (!f->fpt) {
		f->state = (ret == -ENOMSG) ? SMI_REPO_EMPTY : SMI_REPO_ABSENT;
		f->last_error = ret;
	}
	mutex_unlock(&f->lock);

	return ret;
}

void smi_fpt_summary(struct smi_fpt_copy *f, struct smi_repo_summary *row)
{
	mutex_lock(&f->lock);
	row->num_records = f->num_records;
	row->state = f->state;
	row->last_error = f->last_error;
	mutex_unlock(&f->lock);
}

int smi_fpt_header(struct smi_fpt_copy *f, uint8_t boot_device,
		   struct smi_fpt_header *hdr)
{
	int ret = 0;

	if (!f || !hdr || (boot_device >= SMI_SMC_BOOT_DEVICE_MAX))
		return -EINVAL;

	mutex_lock(&f->lock);
	if (!f->fpt)
		ret = -ENODATA;
	else
		*hdr = f->fpt->hdr[boot_device];
	mutex_unlock(&f->lock);

	return ret;
}

int smi_fpt_partition(struct smi_fpt_copy *f, uint8_t boot_device,
		      uint32_t partition_id, struct smi_partition *partition)
{
	int ret = 0;

	if (!f || !partition || (boot_device >= SMI_SMC_BOOT_DEVICE_MAX))
		return -EINVAL;

	mutex_lock(&f->lock);
	if (!f->fpt)
		ret = -ENODATA;
	else if (partition_id >= (uint32_t)f->fpt->hdr[boot_device].num_entries)
		ret = -EINVAL;
	else if (!f->fpt->part[boot_device])
		ret = -ENODATA;
	else
		*partition = f->fpt->part[boot_device][partition_id];
	mutex_unlock(&f->lock);

	return ret;
}
