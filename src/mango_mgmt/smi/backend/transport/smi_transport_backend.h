// SPDX-License-Identifier: GPL-2.0
/*
 * smi_transport_backend.h - Data-plane backend contract + transport body.
 *
 * Backend-only: the GCQ/RPMSG backends and smi_transport.c include this for the
 * full struct smi_transport, the ops vtable and the backend descriptor. The
 * control plane sees only the opaque handle and smi_transport.h.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved. */

#ifndef SMI_TRANSPORT_BACKEND_H
#define SMI_TRANSPORT_BACKEND_H

#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>

#include "fw_if.h"
#include "fw_if_gcq.h"
#include "fw_if_rpmsg.h"
#include "smi_boot.h" /* struct smi_span, struct gcq_partition_layout */
#include "smi_transport.h" /* opaque handle, the resource view, public API */

/* One class of the staging area, cut into slots a request can hold one of.
 *
 * Slots and not one range with a mutex: a host holding one slice at a time has
 * one request in flight whatever the card allows. Fixed offsets and a bitmap,
 * because the request sizes are clusters, not a spread.
 *
 * Every slot is cache-line aligned and a whole number of lines long: cache
 * maintenance here is by line, so two slots sharing one line means one owner's
 * flush writing back another's bytes. */
struct smi_slot_class {
	uint32_t base; /* first slot, from the payload base */
	uint32_t size; /* bytes per slot */
	uint8_t count; /* slots carved */
	uint8_t cap; /* how many may be out at once (parked budget) */
	uint32_t free_map; /* bit set = free */
	uint32_t out; /* slots currently held */
	uint32_t retired_map; /* bit set = a timeout's slot, held back */
	uint32_t waiters; /* queued for a slot; a fresh taker joins them */
	spinlock_t lock;
	wait_queue_head_t wq;
};

/* The line this window's maintenance works in. 32 on the card's R5; 64 is a
 * whole number of those and of every line a host is likely to have. */
#define SMI_SLOT_ALIGN (64u)

/* Bulk is ONE slot, because the device behind it is one: the card refuses a
 * second flash op whatever this side does. */
#define SMI_SLOTS_BULK (1u)

/* Payloads stage past the first MiB of the data region. */
#define SMC_DATA_ADDR_OFF (1024 * 1024)

/* Transport-local logging on the transport's own device: backends emit messages
 * without reaching into the control context, with SMI_*-shaped prefixes for
 * grep-compat. */
#define TP_ERR(tp, fmt, arg...)                                                \
	dev_err((tp)->dev, "ERROR           : " fmt "\n", ##arg)
#define TP_WARN(tp, fmt, arg...)                                               \
	dev_warn((tp)->dev, "WARNING         : " fmt "\n", ##arg)
#define TP_DBG(tp, fmt, arg...)                                                \
	dev_dbg((tp)->dev, "DEBUG           : " fmt "\n", ##arg)
#define TP_VDBG(tp, fmt, arg...)                                               \
	dev_vdbg((tp)->dev, "VERBOSE DEBUG           : " fmt "\n", ##arg)

/**
 * smi_span_in() - Does [off, off+len) lie inside a mapping of @maplen bytes?
 * @maplen: the mapping's length, 0 for a mapping whose length is unknown.
 * @off: offset from the mapping's base.
 * @len: byte count.
 *
 * The bound the bootstrap has before any layout exists. Sums are 64-bit so an
 * offset near the top of the 32-bit space cannot wrap into a pass. A mapping
 * that never stated its length refuses everything rather than guessing: a raw
 * read off a region of unknown size is what this exists to stop.
 *
 * Return: true when the range fits.
 */
static inline bool smi_span_in(uint32_t maplen, uint32_t off, size_t len)
{
	return (0u != maplen) &&
	       (((uint64_t)off + (uint64_t)len) <= (uint64_t)maplen);
}

/**
 * smi_lane_room() - how much of a mapping of @maplen lies at or past @lane_off.
 *
 * The extent every lane-relative offset is bounded by: a lane starting past the
 * mapping leaves nothing, which refuses rather than wrapping.
 *
 * Return: the bytes available to the lane.
 */
static inline uint32_t smi_lane_room(uint32_t maplen, uint32_t lane_off)
{
	return (lane_off < maplen) ? (maplen - lane_off) : 0u;
}

/**
 * smi_span_ok() - Does [off, off+len) lie inside @r?
 * @r: the span.
 * @off: offset from the payload base - absolute, not region-relative.
 * @len: byte count.
 *
 * get_region() answers start + a sub-offset, so the range is measured against
 * [start, start + size), not a bare length: comparing an absolute offset with a
 * bare length refuses the top start bytes of the window. Sums are 64-bit so a
 * region near the top of the 32-bit space cannot wrap into a pass. Both bounds
 * are tested -- with only the ceiling, an offset BELOW the region passes.
 *
 * Return: true when the range fits.
 */
static inline bool smi_span_ok(const struct smi_span *r, uint32_t off,
			       size_t len)
{
	if (!r)
		return false;

	return ((uint64_t)off >= (uint64_t)r->start) &&
	       (((uint64_t)off + (uint64_t)len) <=
		((uint64_t)r->start + (uint64_t)r->len));
}

/**
 * struct smi_mem_layout - Window layout the medium keeps.
 * @ring: command ring; empty on a medium that carries frames elsewhere.
 * @data: the region requests address, from struct smi_layout.
 *
 * Filled by the backend's create path from the bootstrap answer (GCQ) or from
 * what the medium states (RPMSG). The debug-log ring has its own view.
 */
struct smi_mem_layout {
	struct smi_span ring;
	struct smi_span data;
	/* The generations the window said the card serves, or 0 when it said
	 * nothing -- which is a dialect older than the field, not a card that
	 * serves none. */
	uint32_t gens;
};

/* The create path's two halves around the medium's own create: a transport
 * with its locks set up, and the staging slots cut from the layout it filled. */
struct smi_transport *smi_transport_alloc(struct device *dev, FW_IF_TYPE type);
int smi_transport_slots_init(struct smi_transport *tp);

/**
 * smi_transport_take_layout() - Take everything the bootstrap probe answered.
 * @tp: the transport.
 * @layout: what the dialect stated about this card.
 *
 * For the create paths: one call instead of a field-by-field copy per backend,
 * so a value a dialect learns to state reaches every medium at once.
 */
void smi_transport_take_layout(struct smi_transport *tp,
			       const struct smi_layout *layout);

/**
 * smi_payload_off_ok() - Is [off, off+len) inside a region callers may address?
 * @l: the backend's layout.
 * @off: offset from the payload base - absolute, as get_region() answers it.
 * @len: byte count.
 *
 * The data region alone, which the staging slots carve on both media; the
 * published sections have their own region, and the rings are the transport's.
 *
 * Return: true when the range fits a region the caller may address.
 */
static inline bool smi_payload_off_ok(const struct smi_mem_layout *l,
				      uint32_t off, size_t len)
{
	if (!l)
		return false;

	return smi_span_ok(&l->data, off, len);
}

/**
 * smi_mem_payload() - The payload area of the data region.
 * @l: the medium's layout.
 * @addr: receives the offset from the payload base.
 * @len: receives the length, 0 for a window too small to hold one.
 */
static inline void smi_mem_payload(const struct smi_mem_layout *l,
				   uint32_t *addr, uint32_t *len)
{
	*addr = l->data.start + SMC_DATA_ADDR_OFF;
	*len = (l->data.len >= SMC_DATA_ADDR_OFF)
		       ? (l->data.len - SMC_DATA_ADDR_OFF)
		       : 0;
}

/**
 * struct smi_transport_ops - Data-plane backend vtable.
 *
 * DATA plane only: payload read/write and service start/stop. The command plane (smi_cmd.c -> smc_proxy -> FAL) does not route
 * through this vtable; a submission stages its payload here but issues the
 * request through smc_proxy.
 *
 * Staging slots are NOT here: both media carve the same region the same way, so
 * the pool is the transport's and neither backend answers for it.
 *
 * int = Linux errno (0 success, negative error); void = no-fail teardown.
 * Optional ops (check_compatibility) may be NULL, and the smi_transport_*()
 * wrappers hide the NULL checks.
 */
struct smi_transport_ops {
	int (*read)(struct smi_transport *tp, uint32_t offset, void *dst,
		    size_t len);
	int (*write)(struct smi_transport *tp, uint32_t offset, const void *src,
		     size_t len);

	/* Raw window read for the bootstrap axis, bounded by nothing: it runs
	 * before a layout exists. NULL on a medium whose window carries no
	 * descriptor. */
	int (*window_read)(struct smi_transport *tp, uint32_t off, void *dst,
			   size_t len);
	/* One aligned 32-bit load of the window, for a word the card rewrites
	 * while a host reads it: a byte copy could tear it. */
	int (*window_read32)(struct smi_transport *tp, uint32_t off,
			     uint32_t *out);

	int (*start)(struct smi_transport *tp);
	void (*stop)(struct smi_transport *tp);

	int (*check_compatibility)(struct smi_transport *tp, uint16_t major,
				   uint16_t minor);
};

/**
 * struct smi_transport - Data-plane transport context (devm lifetime).
 *
 * Data-plane state only: device ref, FAL/backend config, layout and iomem
 * bases, ops vtable, submit gate (cmd_lock + halted) and the staging slots.
 * No pointer to the control plane, which reaches it only through
 * smi_transport_*(). Tags: [backend] set by the backend's create(), [gcq-only]
 * meaningful only for GCQ.
 */
struct smi_transport {
	struct device *dev; /* [backend] own device ref */
	struct smi_mem_layout mem_layout; /* [backend] filled by create path */
	/* [core] a dialect claimed this window's magic at attach. */
	bool bootstrapped;
	void __iomem *payload_base_virt_addr; /* [backend] payload base */
	/* [backend] how much of each mapping exists. The bootstrap reads a
	 * window before any layout bounds it, so the mapping's own length is
	 * the only bound there is -- and a card stating an offset past its own
	 * region must be refused, not followed off the end. */
	uint32_t payload_len;
	/* Where this lane begins inside the mapped window (see smi_layout). */
	uint32_t lane_off;
	uint32_t ro_len;
	/* [backend] where this host's region is, when a device-tree reservation
	 * states it. 0 where the base arrived translated, as a PCIe host's does. */
	uint64_t payload_pa;
	/* [backend] the read-only region (records, debug log, sections) every
	 * host maps; NULL where the medium hands out one base, as PCIe does. */
	const void *ro_base;
	/* [backend] the card's own version, as the read-only region states it.
	 * A window that states none leaves @build_valid false and the verb is
	 * what is left. */
	struct smi_identify_rsp build;
	char build_tag[SMI_BUILD_TAG_BYTES];
	bool build_valid;
	/* [backend] the verbs the card states and what one lane may hold. */
	struct smi_desc_verb verbs[SMI_VERBS_MAX];
	uint8_t verb_cnt;
	struct smi_desc_limits limits;
	/* [backend] the sections the card publishes, as offsets into the
	 * read-only region. */
	struct smi_pub_layout pub;
	/* [backend] the largest frame this medium will carry, as the medium's
	 * own answer: the ring slot the window states, or the channel's MTU.
	 * The command plane bounds a request by this and by nothing else. */
	uint32_t tx_capacity;
	void __iomem *base_virt_addr; /* [gcq-only] queue BAR base */
	void __iomem *ring_buf_base_virt_addr; /* [gcq-only] ring buffer base */
	FW_IF_CFG fw_if_cfg; /* [backend] FAL config; borrowed via get_fal() */
	FW_IF_TYPE type; /* [backend] backend discriminant */
	union {
		FW_IF_GCQ_CFG gcq;
		FW_IF_RPMSG_CFG rpmsg;
	} backend; /* [backend] only the @type arm is populated */
	const struct smi_transport_ops *ops; /* [backend] data-plane vtable */
	/* [backend] the log ring, as the dialect described it once. rec_cnt 0
	 * means this window declares none. */
	struct smi_log_view log_view;
	struct mutex cmd_lock; /* [backend] guards @halted + build() */
	bool halted; /* [backend] set under @cmd_lock, read lockless */
	/* The staging slots, carved from the data region the window declared;
	 * both media use them. */
	struct smi_slot_class bulk;
};

/**
 * struct smi_transport_backend - Pluggable data-plane backend descriptor.
 * @type: backend discriminant matched by smi_transport_create().
 * @ops: data-plane vtable installed on the transport.
 * @create: probe the device, fill the transport and open the FAL. The
 *          transport is already devm-allocated; @create must not free it.
 *
 * One descriptor per backend, registered in smi_backends[] (smi_transport.c).
 */
struct smi_transport_backend {
	FW_IF_TYPE type;
	const struct smi_transport_ops *ops;
	int (*create)(struct smi_transport *tp,
		      const struct smi_transport_res *res);
};

/* Backend descriptors (one per *_create() implementation). */
extern const struct smi_transport_backend gcq_transport_backend;
extern const struct smi_transport_backend rpmsg_transport_backend;

#endif /* SMI_TRANSPORT_BACKEND_H */
