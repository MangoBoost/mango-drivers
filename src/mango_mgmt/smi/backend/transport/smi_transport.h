// SPDX-License-Identifier: GPL-2.0
/*
 * smi_transport.h - Backend-agnostic transport helpers (kernel-only).
 *
 * The control-plane-facing API: opaque handle, create, dispatch wrappers. The
 * struct body, ops vtable and backend descriptors live in
 * smi_transport_backend.h.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved. */

#ifndef _SMI_TRANSPORT_H_
#define _SMI_TRANSPORT_H_

#include <linux/types.h>

#include "fw_if.h" /* FW_IF_CFG, FW_IF_ERRORS */
#include "smi_boot.h" /* struct smi_log_view: the log's shape, once */

/* Forward declarations */
struct smi_transport;

/**
 * struct smi_transport_res - What a medium was handed: the pre-mapped bases and
 *                            their extents.
 *
 * A backend that maps its own memory ignores this. One that is handed a mapping
 * needs the extent, because the bootstrap reads a window before any layout
 * bounds it -- the mapping's own length is the only bound there is then.
 */
struct smi_transport_res {
	void __iomem *queue_base;
	uint32_t queue_len;
	void __iomem *payload_base;
	uint32_t payload_len;
};
struct device;
struct smi_identify_rsp;

/**
 * enum smi_region_type - Payload region selector for get_region/put_region.
 * Public: the message layer names a region; the backend body stays opaque.
 */
enum smi_region_type {
	SMI_REGION_DATA,
};

/**
 * smi_transport_create() - Allocate and initialise a transport for @type.
 * @dev: device for devm scope (becomes the transport's own dev).
 * @type: backend discriminant (FW_IF_TYPE_GCQ / FW_IF_TYPE_RPMSG).
 * @res: the pre-mapped bases and their extents; NULL for a backend that maps
 *       its own memory.
 *
 * Dispatches to the backend, which probes the device and opens the FAL. The
 * instance is devm-allocated (lifetime = @dev); no explicit close frees it.
 *
 * Return: the transport, or an ERR_PTR() errno on failure (use IS_ERR()).
 */
struct smi_transport *smi_transport_create(struct device *dev, FW_IF_TYPE type,
					   const struct smi_transport_res *res);

/**
 * smi_transport_get_fal() - Borrow the backend-owned FAL handle.
 * @tp: transport context.
 *
 * The FAL handle belongs to the active backend; the message layer and smc_proxy
 * borrow it here instead of reaching into struct smi_transport.
 *
 * Return: pointer to the FAL config, or NULL if @tp is NULL.
 */
FW_IF_CFG *smi_transport_get_fal(struct smi_transport *tp);

/**
 * smi_transport_dev() - The device the transport was created on.
 * @tp: transport context.
 *
 * Lets the bootstrap axis log on the medium's device without seeing the body.
 *
 * Return: the device, or NULL if @tp is NULL.
 */
struct device *smi_transport_dev(struct smi_transport *tp);

/**
 * smi_transport_type() - Which medium this transport is.
 * @tp: the transport.
 *
 * The route a host arrived by, which is what says which region of the card's
 * carve is its own -- a headless region carries nothing that would say so.
 *
 * Return: the backend discriminant, or MAX_FW_IF_TYPE where there is none.
 */
FW_IF_TYPE smi_transport_type(struct smi_transport *tp);

/**
 * smi_transport_region_pa() - Where this host mapped its own region, if it knows.
 * @tp: the transport.
 * @out: receives the physical address.
 *
 * Only a host that resolved the address itself can answer -- which is what makes
 * the card's map checkable rather than merely readable.
 *
 * Return: true when @out was filled.
 */
bool smi_transport_region_pa(struct smi_transport *tp, uint64_t *out);

/**
 * fw_if_err_to_errno() - Translate a FAL FW_IF_ERRORS code to a Linux errno.
 * @err: FW_IF_ERRORS value returned by a FAL operation.
 *
 * Keeps FW_IF_ERRORS at the FAL boundary: callers outside fal/ see errno only.
 *
 * Return: 0 for FW_IF_ERRORS_NONE, negative errno otherwise.
 */
int fw_if_err_to_errno(uint32_t err);

/**
 * smi_transport_set_halted() - Backend-only: publish the halted state.
 * @tp: transport context.
 * @halted: new state.
 *
 * Serialises against the submit section with the transport's lock. Backends
 * only.
 */
void smi_transport_set_halted(struct smi_transport *tp, bool halted);

/**
 * smi_transport_submit_begin() - Enter the command submit section.
 * @tp: transport context.
 *
 * Serialises request builders against each other and against halted-state
 * changes; on success the lock stays held for smi_transport_submit_end().
 *
 * Return: 0 with the submit lock held, -EINVAL for a NULL transport, or
 * -ECANCELED if the transport was quiesced before the request reached hardware.
 */
int smi_transport_submit_begin(struct smi_transport *tp);

/**
 * smi_transport_submit_end() - Leave the command submit section.
 * @tp: transport context.
 */
void smi_transport_submit_end(struct smi_transport *tp);

/**
 * smi_transport_window_read() - Read raw window bytes, before any layout.
 * @tp: transport context.
 * @off: offset from the payload base.
 * @dst: destination buffer.
 * @len: byte count.
 *
 * Unbounded by design: the descriptor states the layout, so the read that finds
 * it cannot be checked against one. Bootstrap axis and dialects only; every
 * other reader goes through the bounded read()/write().
 *
 * Return: 0 on success, -EINVAL on bad args, -ENODEV if torn down,
 * -EOPNOTSUPP if the medium's window carries no descriptor.
 */
int smi_transport_window_read(struct smi_transport *tp, uint32_t off, void *dst,
			      size_t len);

/**
 * smi_transport_ro_read() - Read the read-only region this host maps.
 * @tp: transport context.
 * @off: offset from the region's base.
 * @dst: destination buffer.
 * @len: byte count.
 *
 * Records, debug log and published sections live in one region every host reads
 * and none writes. Where the medium hands out one base it is the lane's own
 * window; where a host maps it separately, this reads that mapping. Unbounded
 * like window_read(): the records it walks state the bounds.
 *
 * Return: 0 on success, -EINVAL on bad args, or the window read's error.
 */
int smi_transport_ro_read(struct smi_transport *tp, uint32_t off, void *dst,
			  size_t len);

/**
 * smi_transport_ro_read32() - One aligned 32-bit load of the read-only region.
 * @tp: transport context.
 * @off: offset from the region's base, a multiple of 4.
 * @out: receives the word.
 *
 * For a word the card rewrites while a host reads it, such as a section's seq:
 * smi_transport_ro_read() copies bytes and could return halves of two values.
 *
 * Return: 0 on success, -EINVAL on bad args or an unaligned offset, -ERANGE
 * past the mapping, -EOPNOTSUPP where the medium has no such load.
 */
int smi_transport_ro_read32(struct smi_transport *tp, uint32_t off,
			    uint32_t *out);

/**
 * smi_transport_log_index() - Read the device debug-log write index.
 * @tp: transport context.
 * @out_index: receives the current log record index.
 *
 * Dispatched to the dialect that read this window: where the index lives is its
 * knowledge, not a shared constant. The read re-snapshots the payload base, so
 * a concurrent teardown shows up as -ENODEV, not a stale dereference.
 *
 * Return: 0 on success, -EINVAL on bad args, -ENODEV if torn down,
 * -EOPNOTSUPP if this window declares no log.
 */
/**
 * smi_transport_take_log_view() - accept a dialect's log geometry, once.
 * @tp: the transport.
 * @view: what the dialect read out of the window.
 *
 * Validated here because a geometry that does not fit is a card this host
 * cannot read a log from, not a read that fails every time.
 *
 * Return: 0 (also for a window with no log), -EPROTO for one that does not fit.
 */
int smi_transport_take_log_view(struct smi_transport *tp,
				const struct smi_log_view *view);

/**
 * smi_transport_log_records() - how many records this card's log ring holds.
 * @tp: the transport. 0 when the window declares no log.
 */
uint32_t smi_transport_log_records(struct smi_transport *tp);

int smi_transport_log_index(struct smi_transport *tp, uint32_t *out_index);

/**
 * smi_transport_log_read_record() - Copy one debug-log record from the device.
 * @tp: transport context.
 * @slot: record slot index.
 * @dst: destination buffer.
 * @cap: room in @dst. The record size and the slot stride are the dialect's.
 *
 * Return: 0 on success, -EINVAL on bad args, -ENODEV if torn down, -ENOSPC if
 * a record does not fit @cap, -EOPNOTSUPP if this window declares no log.
 */
int smi_transport_log_read_record(struct smi_transport *tp, uint32_t slot,
				  void *dst, size_t cap);

/* ---- Data-plane dispatch (thin wrappers over the backend ops vtable) ---- */

/**
 * smi_transport_get_region() - Acquire a payload region (log or data).
 * @timeout: the most jiffies to wait for a slot of the class.
 * @uninterruptible: a signal does not end the wait (a burn's chunk).
 *
 * Return: 0 on success, -ETIMEDOUT when no slot came back in time, -EINTR on
 *         a signal, else a negative errno.
 */
int smi_transport_get_region(struct smi_transport *tp,
			     enum smi_region_type type, uint32_t *addr,
			     uint32_t *len, unsigned long timeout,
			     bool uninterruptible);

/**
 * smi_transport_slot_bytes() - The largest slice one request can claim.
 * @tp: Transport context.
 * @type: Which region's slots.
 *
 * An answer rides one slot, so this is what a caller checks a card-stated
 * size against before allocating for it.
 *
 * Return: the slot size, or 0 where this region has no slots.
 */
uint32_t smi_transport_slot_bytes(struct smi_transport *tp,
				  enum smi_region_type type);

/**
 * smi_transport_data_start() - Where the data region begins in the window.
 *
 * The partition table states it, so no negotiation is needed: v2 measures a
 * slice from the data region, and the negotiation's own answer is written
 * there.
 *
 * Return: the offset, or 0 when no layout has been read yet.
 */
uint32_t smi_transport_data_start(struct smi_transport *tp);

/** smi_transport_put_region() - Release a region acquired with get_region(). */
void smi_transport_put_region(struct smi_transport *tp,
			      enum smi_region_type type, uint32_t addr);

/**
 * smi_transport_retire_region() - Give a slot back that was never answered.
 * @tp: the transport.
 * @type: the class it came from.
 * @addr: the slot, as get_region() handed it out.
 *
 * The work may still be writing into that slot, so it is NOT returned to the
 * pool -- handing it out again is the corruption a timeout would cause. It
 * comes back at the next smi_transport_start(), the one event that ends it.
 */
void smi_transport_retire_region(struct smi_transport *tp,
				 enum smi_region_type type, uint32_t addr);

/**
 * smi_transport_build() - The card's version, as its window states it.
 * @tp: the transport.
 * @out: receives the version.
 *
 * There before a frame is sent, and on a lane that never negotiates. A window
 * that states none answers -ENOENT and the asking verb is what is left.
 *
 * Return: 0 with @out filled, -ENOENT when the window stated no build.
 */
int smi_transport_build(struct smi_transport *tp, struct smi_identify_rsp *out);

/**
 * smi_transport_build_tag() - The name the card's tree gave its build.
 * @tp: Transport.
 *
 * Return: A NUL terminated name, empty where the card states none.
 */
const char *smi_transport_build_tag(struct smi_transport *tp);

/**
 * smi_transport_tx_capacity() - the largest frame this medium will carry.
 * @tp: the transport.
 *
 * The medium's own limit, settled when the backend was created: a GCQ ring's
 * submission slot, an rpmsg channel's MTU. Zero for a transport that never
 * created one, which the command plane refuses to build on.
 */
uint32_t smi_transport_tx_capacity(struct smi_transport *tp);

/**
 * smi_transport_verb() - What the window states about one verb.
 *
 * Return: its Verb record, or NULL when the card states none for @opcode --
 *         which is a verb this card does not have.
 */
const struct smi_desc_verb *smi_transport_verb(struct smi_transport *tp,
					       uint16_t opcode);

/* What one lane may hold, as the window states it; zero where it states none. */
struct smi_desc_limits smi_transport_limits(struct smi_transport *tp);

/* Whether the window states the card's verbs: firmware that does answers every
 * request it takes, late or not. */
bool smi_transport_states_verbs(struct smi_transport *tp);

/* The longest any stated verb may take: how long an earlier session's requests
 * can outlive the host that sent them. 0 where none is stated. */
uint32_t smi_transport_verb_longest_ms(struct smi_transport *tp);

/**
 * smi_transport_gens() - Which generations the window said this card serves.
 *
 * SMI_GEN_BIT_* bits, or 0 when the window said nothing -- an older dialect
 * with no field for it, where asking is the only way to find out.
 */
uint32_t smi_transport_gens(struct smi_transport *tp);

/**
 * smi_transport_publishes() - Does this card publish sections unasked?
 *
 * The window's own answer, read at attach: a card that states no boot or no
 * Status section publishes nothing a host can judge.
 */
bool smi_transport_publishes(struct smi_transport *tp);

/**
 * smi_transport_pub() - The sections this card publishes, as it stated them.
 * @tp: Transport.
 *
 * Return: The layout; an empty one where the card publishes nothing.
 */
const struct smi_pub_layout *smi_transport_pub(struct smi_transport *tp);

/**
 * smi_transport_bootstrapped() - Did a dialect claim this window at attach?
 * @tp: Transport.
 *
 * Return: true when a bootstrap read produced this lane's layout.
 */
bool smi_transport_bootstrapped(struct smi_transport *tp);

/** smi_transport_read() - Read @len bytes from the payload at @offset. */
int smi_transport_read(struct smi_transport *tp, uint32_t offset, void *dst,
		       size_t len);

/** smi_transport_write() - Write @len bytes to the payload at @offset. */
int smi_transport_write(struct smi_transport *tp, uint32_t offset,
			const void *src, size_t len);

/** smi_transport_start() - Start the service. Return: 0 on success or no-op. */
int smi_transport_start(struct smi_transport *tp);

/** smi_transport_stop() - Stop the service (no-op if unsupported). */
void smi_transport_stop(struct smi_transport *tp);

/**
 * smi_transport_check_compatibility() - Optional firmware version check.
 * Return: the backend's result, or -ENOTSUPP if the backend has no check.
 */
int smi_transport_check_compatibility(struct smi_transport *tp, uint16_t major,
				      uint16_t minor);

/**
 * smi_transport_invalidate_io() - Publish NULL for the iomem bases (teardown).
 * @tp: transport context.
 *
 * WRITE_ONCE NULL so cyclic READ_ONCE readers (smi_cmd_dump_log) see a clean
 * transition. Idempotent; call after the response thread is reaped.
 */
void smi_transport_invalidate_io(struct smi_transport *tp);

#endif /* _SMI_TRANSPORT_H_ */
