// SPDX-License-Identifier: GPL-2.0
/*
 * smi_boot.h - Bootstrap axis: what a payload window says about itself.
 *
 * Separate from the medium on purpose: the medium maps the window and moves
 * bytes, this axis reads the descriptor those bytes hold and answers with a
 * dialect-neutral product. Kept together, a second dialect would cost
 * (media x dialects) code paths; apart it costs one file and one table row.
 *
 * Each dialect is one file and one row of the registry in smi_boot.c.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved. */

#ifndef SMI_BOOT_H
#define SMI_BOOT_H

#include <linux/types.h>

#include "contract/smi/smi_abi.h" /* SMI_GEN_BIT_* */
#include "smi_facts.h" /* SMI_BUILD_TAG_BYTES */

/* How long a card may take to start answering after its link is back: the PLM
 * returns the link while the endpoint task starts near the end of firmware's
 * boot. One budget because it is one fact about the card; how often to look is
 * each waiter's own, since a word is free to read and a frame is not. */
#define SMI_BOOT_SERVE_BUDGET_MS (15000)

/* Forward declarations */
struct smi_transport;

/**
 * struct smi_span - A byte range in the payload window, as it is stated.
 * @start: offset from the payload base.
 * @len: byte count. Zero means "no such region", never "unknown".
 */
struct smi_span {
	uint32_t start;
	uint32_t len;
};

/* The most Verb records this driver keeps; a card stating more is read to it. */
#define SMI_VERBS_MAX (32)

/* The most published sections this driver keeps: Status, Sensors, Counters
 * and one per cage; a card stating more is read to it. */
#define SMI_PUB_REGIONS_MAX (24)

/**
 * struct smi_pub_region - One section a card publishes, as its Region record
 * states it.
 * @kind: SmiDescRegion_*.
 * @inst: which one of a repeatable kind -- the cage number; else 0.
 * @span: where it is; a zero length = not read.
 */
struct smi_pub_region {
	uint16_t kind;
	uint16_t inst;
	struct smi_span span;
};

/**
 * struct smi_pub_layout - The sections a card publishes, as its descriptor
 * states them.
 * @boot: the boot the descriptor states; a section another boot wrote is stale.
 *        0 = nothing is published, as no section could be judged.
 * @region: every published section, in record order; the log is not one.
 * @region_cnt: how many.
 */
struct smi_pub_layout {
	uint32_t boot;
	struct smi_pub_region region[SMI_PUB_REGIONS_MAX];
	uint8_t region_cnt;
};

/* The section published as @kind/@inst, or NULL where the card names none. */
static inline const struct smi_span *
smi_pub_find(const struct smi_pub_layout *pub, uint16_t kind, uint16_t inst)
{
	uint8_t i;

	for (i = 0; pub && (i < pub->region_cnt); i++)
		if ((pub->region[i].kind == kind) &&
		    (pub->region[i].inst == inst))
			return &pub->region[i].span;

	return NULL;
}

/**
 * struct smi_layout - The dialect-neutral product of a bootstrap read.
 * @data: the only region a request may address.
 * @pub: the sections firmware publishes unasked; none is a stated fact, not a
 *       missing value.
 * @build: the card's own version, when its window states one. Read before a
 *         frame is sent and on a lane that never negotiates.
 * @build_valid: whether @build was stated.
 * @build_tag: the name the card's tree gave that build, empty when unstated.
 * @gens: generations the card serves (SMI_GEN_BIT_*); 0 where the window has
 *        no field for it, as a block with no descriptor.
 *
 * Values only, and only values a layer above the dialect reads: ring geometry
 * goes straight to the medium that owns it (struct gcq_partition_layout) and
 * the log is an operation. So this type grows only when a consumer needs a
 * fact it cannot already reach.
 */
struct smi_layout {
	struct smi_span data;
	struct smi_pub_layout pub;
	struct smi_identify_rsp build;
	bool build_valid;
	char build_tag[SMI_BUILD_TAG_BYTES];
	uint32_t gens;
	/* Where this lane begins inside what the host mapped. Zero when the
	 * host mapped the lane itself; the lane's place in the carve when it
	 * mapped what the card advertised from the carve's base. */
	uint32_t lane_off;
	/* What the card serves, one record per verb: a verb with no record is
	 * one it does not have. None on a window that states no verbs. */
	struct smi_desc_verb verbs[SMI_VERBS_MAX];
	uint8_t verb_cnt;
	/* What one lane may hold; zero where the card states none. */
	struct smi_desc_limits limits;
};

/**
 * struct gcq_partition_layout - Window values only the GCQ medium consumes.
 * @ring_off: command ring offset from the payload base.
 * @ring_len: command ring length.
 * @sq_slot: submission slot size.
 * @cq_slot: completion slot size.
 *
 * A dialect parses these out of the descriptor and hands them to the medium. A
 * ring inside the window is the medium's business and RPMSG has none, so
 * struct smi_layout would make every medium carry a shape only one has.
 */
struct gcq_partition_layout {
	uint32_t ring_off;
	uint32_t ring_len;
	uint16_t sq_slot;
	uint16_t cq_slot;
};

/* The widest record this driver will snapshot -- its own bound, not the card's.
 * A card stating a wider one is refused at bootstrap, not truncated per read. */
#define SMI_LOG_SNAPSHOT_BYTES (256)

/**
 * struct smi_log_view - The debug-log ring, normalised once at bootstrap.
 * @rec_bytes: bytes per record.
 * @rec_cnt: how many records the ring holds.
 * @rec_base: offset of record 0, in the space @in_ro selects.
 * @idx_off: offset of the card's write-index word, in that same space.
 * @in_ro: the log lives in the read-only region every host of this card maps,
 *         rather than in this lane's own window.
 *
 * Derived from the window once per device, so a dump's count, index and
 * records come from one snapshot. @rec_cnt 0 = this window declares no log.
 */
struct smi_log_view {
	uint32_t rec_bytes;
	uint32_t rec_cnt;
	uint32_t rec_base;
	uint32_t idx_off;
	bool in_ro;
};

/**
 * struct smi_boot_dialect - One descriptor dialect.
 * @magic: the word at window offset 0 that selects this dialect.
 * @name: dialect name, for logs.
 * @probe: parse the descriptor into @out (+ the medium's out-param). Reads a
 *         window, never waits: the layout is there as soon as the descriptor
 *         is, which is long before anything answers a frame.
 * @serving: does the task that answers frames exist yet? A window is described
 *           by firmware's boot, and the service that reads it starts later.
 *           NULL where the dialect
 *           cannot tell, which is read as "assume it does".
 * @log_view: fill in where this dialect's log ring is and how it is shaped.
 *            NULL, or a non-zero return, where the dialect declares none.
 * @records: the descriptor records a tool reads, allocated into @out with
 *           their length; NULL where the dialect states none.
 */
struct smi_boot_dialect {
	uint32_t magic;
	/* Where this dialect's magic lives: the first froze a table per window,
	 * the second declares itself once in the region every host reads. */
	bool shared_magic;
	const char *name;
	int (*probe)(struct smi_transport *tp, struct smi_layout *out,
		     struct gcq_partition_layout *out_gcq_layout);
	bool (*serving)(struct smi_transport *tp, const struct smi_layout *l);
	int (*log_view)(struct smi_transport *tp, struct smi_log_view *out);
	int (*records)(struct smi_transport *tp, uint8_t **out, size_t *len);
};

/**
 * smi_boot_window_answers() - Is the window still stating a magic we know?
 * @tp: the transport.
 *
 * A window whose memory decode was turned off reads as all-ones and one whose
 * card is booting reads as zeroes; neither is a magic, so nothing read from it
 * is data. Cheap enough to ask before believing a read.
 *
 * Return: true when some dialect claims what the window states.
 */
bool smi_boot_window_answers(struct smi_transport *tp);

/**
 * smi_boot_tool_records() - The descriptor records a tool reads, as stated.
 * @tp: the transport.
 * @out: a kmalloc'd copy of them, in chain order; the caller frees it.
 * @len: its length; 0, with @out a zero-size allocation, where none are.
 *
 * Return: 0, -EOPNOTSUPP when no dialect claims the window, or the read's error.
 */
int smi_boot_tool_records(struct smi_transport *tp, uint8_t **out, size_t *len);

/**
 * smi_boot_read() - One pass: what does this window say it is?
 * @tp: transport whose window is read (through smi_transport_window_read()).
 * @out_layout: receives the dialect-neutral product.
 * @out_gcq_layout: receives the values only the GCQ medium consumes.
 * @out_log_view: receives the log's geometry; zeroed where there is none.
 *
 * For a medium whose window is written long before it is mapped. A window
 * firmware never wrote into answers -EOPNOTSUPP, which reads as "this card
 * says nothing here" rather than as a failure.
 *
 * Return: 0 with the outputs filled, -EINVAL on bad args, -EOPNOTSUPP when no
 * dialect claims the magic, or the dialect's error.
 */
int smi_boot_read(struct smi_transport *tp, struct smi_layout *out_layout,
		  struct gcq_partition_layout *out_gcq_layout,
		  struct smi_log_view *out_log_view);

/**
 * smi_boot_probe() - Read a window's descriptor and answer its layout.
 * @tp: transport whose window is read (through smi_transport_window_read()).
 * @out_layout: receives the dialect-neutral product.
 * @out_gcq_layout: receives the values only the GCQ medium consumes.
 * @out_log_view: receives the log's geometry; zeroed where there is none.
 *
 * Polls because the descriptor is written by firmware that may still be
 * booting: the magic appears first and the window only serves once its status
 * word is set.
 *
 * Return: 0 with the outputs filled, -EINVAL on bad args, -ETIMEDOUT where the
 * window stayed empty or its dialect never became ready, or -EOPNOTSUPP where
 * the magic at offset 0 is one no dialect claims.
 */
int smi_boot_probe(struct smi_transport *tp, struct smi_layout *out_layout,
		   struct gcq_partition_layout *out_gcq_layout,
		   struct smi_log_view *out_log_view);

#endif /* SMI_BOOT_H */
