// SPDX-License-Identifier: GPL-2.0
/*
 * smi_boot_v2.c - The SMID dialect: the descriptor a region writes of itself.
 *
 * A fixed block plus records, so a card can say something new without costing an older host the ring: an
 * unknown record is stepped over by its length, and an unwalkable encoding
 * still leaves the fixed block, where the ring, the data region and so a
 * reflash live.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved. */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/stddef.h>

#include "contract/smi/smi_abi.h"

#include "smi_tlv.h" /* the one chain cursor */

#include "smi_boot_v2.h"
#include "smi_boot.h"
#include "smi_log.h"
#include "smi_pub.h"
#include "smi_transport.h"

/* Record types and Region kinds from here are the firmware's and its tools':
 * this driver reads none of them, and INFO passes the records through. */
#define SMI_DESC_TOOLS_TYPE (0x80u)

/* The descriptor sits at the base the medium handed us, and the magic the
 * selector read is its first word. */
#define SMI_BOOT_V2_DESC_OFF (0)

/* The descriptor records attach takes, at their floors; a type with no row is
 * stepped over by its length, and one that does not repeat is taken once. */
static const struct {
	uint16_t min_len;
	bool repeats;
} smi_desc_rule[] = {
	[SmiDesc_Region] = { SMI_DESC_REGION_BASELINE, true },
	[SmiDesc_Log] = { SMI_DESC_LOG_BASELINE, false },
	[SmiDesc_Build] = { SMI_DESC_BUILD_BASELINE, false },
	[SmiDesc_Carve] = { SMI_DESC_CARVE_BASELINE, false },
	[SmiDesc_Map] = { SMI_DESC_MAP_BASELINE, true },
	[SmiDesc_Boot] = { SMI_DESC_BOOT_BASELINE, false },
	[SmiDesc_Verb] = { SMI_DESC_VERB_BASELINE, true },
	[SmiDesc_Limits] = { SMI_DESC_LIMITS_BASELINE, false },
};

/**
 * struct smi_desc_view - The descriptor's records, copied out of the window.
 * @recs: the chain, owned until smi_desc_view_put().
 * @len: bytes of chain.
 *
 * One copy answers every question asked of it: the descriptor is immutable
 * once its magic is there, so re-reading the window per record bought nothing
 * but bus traffic.
 */
struct smi_desc_view {
	const uint8_t *recs;
	int len;
};

static int smi_desc_view_get(struct smi_transport *tp,
			     struct smi_desc_view *out)
{
	struct smi_desc desc = { 0 };
	uint8_t *recs;
	int ret;

	out->recs = NULL;
	out->len = 0;

	ret = smi_transport_ro_read(tp, SMI_BOOT_V2_DESC_OFF, &desc,
				    sizeof(desc));
	if (ret)
		return ret;

	if (desc.rec_fmt != SMI_DESC_REC_FMT || desc.rec_off < sizeof(desc) ||
	    desc.desc_len < desc.rec_off)
		return -ENOENT;

	out->len = desc.desc_len - desc.rec_off;
	if (!out->len)
		return -ENOENT;

	recs = kzalloc(out->len, GFP_KERNEL);
	if (!recs)
		return -ENOMEM;

	ret = smi_transport_ro_read(tp, desc.rec_off, recs, out->len);
	if (ret) {
		kfree(recs);
		return ret;
	}

	out->recs = recs;
	return 0;
}

static void smi_desc_view_put(struct smi_desc_view *v)
{
	kfree(v->recs);
	v->recs = NULL;
}

/**
 * smi_desc_next() - Copy the next record of @type out of the chain.
 * @v: The copied chain.
 * @at: Cursor; 0 starts at the first record and this leaves it past the one it
 *      took, so a caller walks every instance of a type.
 * @type: SmiDesc_*.
 * @body: Destination.
 * @cap: Bytes @body holds.
 *
 * Return: bytes copied, -ENOENT past the last such record, -EPROTO for a chain
 *         that does not end where it says it does.
 */
static int smi_desc_next(const struct smi_desc_view *v, size_t *at,
			 uint16_t type, void *body, size_t cap)
{
	struct smi_tlv_rec r;
	int rem = v->len - (int)*at;
	const uint16_t min_len = (type < ARRAY_SIZE(smi_desc_rule))
					 ? smi_desc_rule[type].min_len
					 : 0;

	while (smi_tlv_next(v->recs, &rem, at, &r)) {
		if (r.type != type)
			continue;
		if (!smi_tlv_take(&r, min_len, body, cap))
			return -EPROTO;

		return (int)min_t(size_t, cap, (size_t)r.len);
	}

	/* A tail that is not a whole record: the encoding says how long every
	 * record is, so bytes left over are a chain this host cannot trust. */
	return rem ? -EPROTO : -ENOENT;
}

/* The one record of a type a card states once: a second one is two answers
 * to one question, and neither can be trusted. */
static int smi_desc_one(const struct smi_desc_view *v, uint16_t type,
			void *body, size_t cap)
{
	size_t at = 0;
	int ret = smi_desc_next(v, &at, type, body, cap);

	if ((ret > 0) && (type < ARRAY_SIZE(smi_desc_rule)) &&
	    !smi_desc_rule[type].repeats) {
		uint8_t again[sizeof(uint32_t)];

		if (smi_desc_next(v, &at, type, again, sizeof(again)) !=
		    -ENOENT)
			return -EPROTO;
	}
	return ret;
}

void smi_boot_v2_verbs(struct device *dev, const uint8_t *recs, int len,
		       struct smi_layout *out)
{
	const struct smi_desc_view v = { .recs = recs, .len = len };
	size_t at = 0;
	int ret = -ENOENT;

	out->verb_cnt = 0;
	while ((out->verb_cnt < SMI_VERBS_MAX) &&
	       ((ret = smi_desc_next(&v, &at, SmiDesc_Verb,
				     &out->verbs[out->verb_cnt],
				     sizeof(out->verbs[0]))) > 0))
		out->verb_cnt++;
	if ((out->verb_cnt < SMI_VERBS_MAX) && (ret != -ENOENT)) {
		SMI_DEV_ERR(dev, "descriptor's verb records unreadable (%d)",
			    ret);
		out->verb_cnt = 0;
	}

	if (smi_desc_one(&v, SmiDesc_Limits, &out->limits,
			 sizeof(out->limits)) < 0)
		memset(&out->limits, 0, sizeof(out->limits));
}

/**
 * smi_boot_v2_region() - Where a region of @kind sits, if the card has one.
 * @inst: which one of a repeatable kind -- the cage number; else 0.
 *
 * Return: 0 with @out filled, -ENOENT when this card names no such region.
 */
static int smi_boot_v2_region(const struct smi_desc_view *v, uint16_t kind,
			      uint16_t inst, struct smi_span *out)
{
	struct smi_desc_region r = { 0 };
	size_t at = 0;
	int ret;

	while ((ret = smi_desc_next(v, &at, SmiDesc_Region, &r, sizeof(r))) >
	       0) {
		if ((r.kind != kind) || (r.inst != inst))
			continue;

		out->start = r.off;
		out->len = r.len;

		return 0;
	}

	/* The iterator's answer stands: -ENOENT is a card that named no region,
	 * anything else a chain this host cannot trust. */
	return ret ? ret : -EPROTO;
}

/* The log region and its geometry, both from the card. Where the records are is
 * one record and how wide they are is another, so neither is compiled here. */
static int smi_boot_v2_log_geom(const struct smi_desc_view *v,
				struct smi_span *region,
				struct smi_desc_log *geom)
{
	int ret;

	ret = smi_boot_v2_region(v, SmiDescRegion_Log, 0, region);
	if (ret)
		return ret;

	ret = smi_desc_one(v, SmiDesc_Log, geom, sizeof(*geom));
	if (ret < 0)
		return ret;

	return (geom->rec_bytes && geom->rec_cnt) ? 0 : -ENOENT;
}

static int smi_boot_v2_log_view(struct smi_transport *tp,
				struct smi_log_view *out)
{
	struct smi_span region = { 0 };
	struct smi_desc_log geom = { 0 };
	struct smi_desc_view v;
	int ret;

	ret = smi_desc_view_get(tp, &v);
	if (ret)
		return ret;

	ret = smi_boot_v2_log_geom(&v, &region, &geom);
	smi_desc_view_put(&v);
	if (ret)
		return ret;

	/* The head carries the write index and the records follow it, so the
	 * region's own start is what both offsets are taken from. */
	out->rec_bytes = geom.rec_bytes;
	out->rec_cnt = geom.rec_cnt;
	out->rec_base = region.start + sizeof(struct smi_log_head);
	out->idx_off = region.start + offsetof(struct smi_log_head, write_idx);
	/* Described in the region every host of this card maps, not in a lane. */
	out->in_ro = true;

	return 0;
}

/**
 * smi_boot_v2_lane() - This host's own row of the carve map.
 *
 * A headless region carries no descriptor, so its geometry lives in the map and
 * nowhere else. Which row is this host's is not in the map either -- it is the
 * route this host arrived by, which is what the medium says.
 *
 * Return: 0 with @out filled, or -ENOENT when the card names no such row.
 */
static int smi_boot_v2_lane(struct smi_transport *tp,
			    const struct smi_desc_view *v,
			    struct smi_desc_map *out)
{
	const uint16_t want = (FW_IF_TYPE_RPMSG == smi_transport_type(tp))
				      ? SmiDescCarve_LaneApu
				      : SmiDescCarve_LaneHost;
	struct smi_desc_map row = { 0 };
	size_t at = 0;
	int ret;

	while ((ret = smi_desc_next(v, &at, SmiDesc_Map, &row, sizeof(row))) >
	       0) {
		if (row.kind != want)
			continue;

		*out = row;
		return 0;
	}

	/* The iterator's answer stands: -ENOENT is a card that named no row,
	 * anything else a chain this host cannot trust. */
	return ret ? ret : -EPROTO;
}

void smi_boot_v2_pub(struct device *dev, const uint8_t *recs, int len,
		     struct smi_pub_layout *out)
{
	const struct smi_desc_view v = { .recs = recs, .len = len };
	struct smi_desc_boot boot = { 0 };
	struct smi_desc_region r = { 0 };
	size_t at = 0;
	int ret = -ENOENT;

	memset(out, 0, sizeof(*out));
	if ((smi_desc_one(&v, SmiDesc_Boot, &boot, sizeof(boot)) <= 0) ||
	    !boot.boot) {
		/* An empty chain was reported where it was read */
		if (len)
			SMI_DEV_WARN(
				dev,
				"descriptor states no boot: published sections not read");
		return;
	}
	out->boot = boot.boot;

	/* Every published section below the tools' kinds: what each one is, is
	 * its reader's to know. */
	while ((ret = smi_desc_next(&v, &at, SmiDesc_Region, &r, sizeof(r))) >
	       0) {
		if ((SmiDescRegion_Log == r.kind) ||
		    (r.kind >= SMI_DESC_TOOLS_TYPE))
			continue;
		if (out->region_cnt == SMI_PUB_REGIONS_MAX) {
			SMI_DEV_WARN(dev,
				     "descriptor states more than %u regions",
				     SMI_PUB_REGIONS_MAX);
			break;
		}
		out->region[out->region_cnt++] = (struct smi_pub_region){
			.kind = r.kind,
			.inst = r.inst,
			.span = { .start = r.off, .len = r.len },
		};
	}
}

static int smi_boot_v2_probe(struct smi_transport *tp, struct smi_layout *out,
			     struct gcq_partition_layout *out_gcq_layout)
{
	struct device *dev = smi_transport_dev(tp);
	struct smi_desc lane = { 0 };
	struct smi_desc ro = { 0 };
	struct smi_desc_view v = { 0 };
	uint64_t pa = 0;
	int ret;

	/* Two reads for two regions: this lane's prefix says where its ring and
	 * data are, the read-only region's head what the card serves and where
	 * its records are. One base answers both, read twice. */
	ret = smi_transport_window_read(tp, SMI_BOOT_V2_DESC_OFF, &lane,
					sizeof(lane));
	if (ret)
		return ret;

	ret = smi_transport_ro_read(tp, SMI_BOOT_V2_DESC_OFF, &ro, sizeof(ro));
	if (ret)
		return ret;

	/* An encoding this build cannot walk leaves the view empty, and every
	 * record then reads as one the card does not state. A read that failed
	 * or an allocation that did is a different answer and is not swallowed. */
	ret = smi_desc_view_get(tp, &v);
	if (ret && (ret != -ENOENT))
		return ret;

	/* A lane whose host mapped no such region, or a card that wrote none:
	 * this lane's head is then all there is, and it still states the
	 * generations. Only the records go missing, and the recovery path does
	 * not need them. */
	if (ro.magic != SMI_MAGIC_SMID) {
		SMI_DEV_WARN(
			dev,
			"no read-only region for this lane: log and published block unavailable");
		ro = lane;
	}

	SMI_DEV_VDBG(dev, "SMID records %u at 0x%x of 0x%x, generations 0x%x",
		     ro.rec_fmt, ro.rec_off, ro.desc_len, ro.gens);
	SMI_DEV_VDBG(dev, "Ring Buffer  : Offset 0x%x, Length 0x%x",
		     lane.lane.ring_off, lane.lane.ring_len);
	SMI_DEV_VDBG(dev, "Data Buffer  : Offset 0x%x, Length 0x%x",
		     lane.lane.data_off, lane.lane.data_len);

	/* The magic says the descriptor is whole -- written last, after the
	 * body it promises -- not that the card is serving: the window is laid
	 * out in firmware's boot and the task that answers frames starts
	 * later, which smi_boot_v2_serving() asks. */
	out->gens = ro.gens;

	/* A prefix that names no lane is a card whose regions are headless: its
	 * geometry is in the map, read by the route this host came in by. A
	 * card that still writes a lane block at its own base is answered from
	 * that block, which is what lets one driver serve both. */
	if ((0u == lane.lane.data_len) && (0u == lane.lane.ring_len)) {
		struct smi_desc_map row = { 0 };
		struct smi_desc_carve carve = { 0 };
		bool mapped_lane = false;

		if (smi_boot_v2_lane(tp, &v, &row)) {
			SMI_DEV_ERR(
				dev,
				"no map row for this host: the card named no region it serves");
			ret = -ENOENT;
			goto out;
		}

		/* A host that mapped its region by its own means -- a
		 * device-tree reservation -- can compare, and a mismatch is
		 * firmware and that tree having shipped on different media. */
		mapped_lane = smi_transport_region_pa(tp, &pa);

		if (mapped_lane &&
		    (smi_desc_one(&v, SmiDesc_Carve, &carve, sizeof(carve)) >
		     0) &&
		    ((carve.base_pa + row.off) != pa)) {
			SMI_DEV_ERR(
				dev,
				"the card puts this region at 0x%llx, this host mapped 0x%llx",
				(unsigned long long)(carve.base_pa + row.off),
				(unsigned long long)pa);
			ret = -EPROTO;
			goto out;
		}

		lane.lane = row.lane;

		/* The row states two things and they are consumed differently:
		 * how the lane is shaped is what goes on the wire, and where it
		 * sits is how this host reaches those bytes. Keeping the shape
		 * lane-relative is what lets both sides mean one offset. */
		out->lane_off = mapped_lane ? 0u : row.off;
	}

	out->data.start = lane.lane.data_off;
	out->data.len = lane.lane.data_len;

	out_gcq_layout->ring_off = lane.lane.ring_off;
	out_gcq_layout->ring_len = lane.lane.ring_len;
	out_gcq_layout->sq_slot = lane.lane.sq_slot;
	out_gcq_layout->cq_slot = lane.lane.cq_slot;

	if (ro.rec_fmt != SMI_DESC_REC_FMT) {
		/* An encoding this build cannot walk. Membership, not order:
		 * a newer number is one this build was never taught. The fixed
		 * block still stands, so the ring, the data region and a
		 * reflash stay reachable. */
		SMI_DEV_WARN(
			dev,
			"descriptor encoding %u unknown: log and published sections unavailable",
			ro.rec_fmt);

		ret = 0;
		goto out;
	}

	smi_boot_v2_pub(dev, v.recs, v.len, &out->pub);

	{
		/* The record grew a name at its end; a card that states only
		 * the eight-byte answer is read as far as it goes. */
		struct smi_desc_build build = { 0 };
		int n = smi_desc_one(&v, SmiDesc_Build, &build, sizeof(build));

		out->build_valid = n >= (int)sizeof(build.id);
		if (out->build_valid)
			out->build = build.id;
		if (n >= (int)sizeof(build))
			strscpy(out->build_tag, build.tag,
				sizeof(out->build_tag));
	}

	smi_boot_v2_verbs(dev, v.recs, v.len, out);

	ret = 0;
out:
	smi_desc_view_put(&v);
	return ret;
}

/* A Status tick off zero is the frame-serving task saying it exists; a card
 * with no Status this host reads cannot be asked, and is taken at its word. */
static bool smi_boot_v2_serving(struct smi_transport *tp,
				const struct smi_layout *l)
{
	struct smi_pub_status_head st;
	int ret;

	const struct smi_span *status =
		l ? smi_pub_find(&l->pub, SmiDescRegion_Status, 0) : NULL;

	if (!l || !l->pub.boot || !status || !status->len)
		return true;

	ret = smi_pub_status_head(tp, &l->pub, &st);
	if ((ret == -ENODATA) || (ret == -ESTALE))
		return false; /* not written yet this boot, or written by another */

	return ret || (0 != st.tick);
}

/* What a tool reads of the descriptor. The carve, the map and the log are this
 * transport's own, so a record type not named here stays in the kernel. */
static bool smi_desc_for_tools(uint16_t type)
{
	switch (type) {
	case SmiDesc_Region:
	case SmiDesc_Build:
	case SmiDesc_Boot:
	case SmiDesc_Verb:
	case SmiDesc_Limits:
	case SmiDesc_Counter:
		return true;
	default:
		return type >= SMI_DESC_TOOLS_TYPE;
	}
}

int smi_boot_v2_tool_copy(const uint8_t *recs, int len, uint8_t *out)
{
	struct smi_tlv_rec r;
	size_t at = 0;
	int rem = len, n = 0;

	/* Each kept record whole, prefix and body, as the card wrote it. */
	while (smi_tlv_next(recs, &rem, &at, &r)) {
		const size_t whole = sizeof(struct smi_tlv_hdr) + r.len;

		if (!smi_desc_for_tools(r.type))
			continue;
		memcpy(out + n, r.body - sizeof(struct smi_tlv_hdr), whole);
		n += whole;
	}

	return rem ? -EPROTO : n;
}

static int smi_boot_v2_records(struct smi_transport *tp, uint8_t **out,
			       size_t *len)
{
	struct smi_desc_view v;
	uint8_t *buf;
	int ret;

	ret = smi_desc_view_get(tp, &v);
	if (ret == -ENOENT) {
		*len = 0;
		*out = kmalloc(0, GFP_KERNEL);
		return 0;
	}
	if (ret)
		return ret;

	buf = kmalloc(v.len, GFP_KERNEL);
	if (!buf) {
		smi_desc_view_put(&v);
		return -ENOMEM;
	}

	ret = smi_boot_v2_tool_copy(v.recs, v.len, buf);
	smi_desc_view_put(&v);
	if (ret < 0) {
		kfree(buf);
		return ret;
	}

	*len = ret;
	*out = buf;
	return 0;
}

const struct smi_boot_dialect smi_boot_dialect_v2 = {
	.magic = SMI_MAGIC_SMID,
	.shared_magic = true,
	.name = "SMID",
	.probe = smi_boot_v2_probe,
	.serving = smi_boot_v2_serving,
	.log_view = smi_boot_v2_log_view,
	.records = smi_boot_v2_records,
};
