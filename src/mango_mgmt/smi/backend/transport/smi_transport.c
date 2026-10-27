// SPDX-License-Identifier: GPL-2.0
/*
 * smi_transport.c - Backend-agnostic transport core: the lifecycle factory,
 * the data-plane dispatch wrappers, the log dispatch and the FAL/errno helpers.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved. */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/wait.h>

#include "smi_transport.h"
#include "smi_transport_backend.h"

FW_IF_CFG *smi_transport_get_fal(struct smi_transport *tp)
{
	if (!tp)
		return NULL;

	return &tp->fw_if_cfg;
}

struct device *smi_transport_dev(struct smi_transport *tp)
{
	return tp ? tp->dev : NULL;
}

int smi_transport_window_read(struct smi_transport *tp, uint32_t off, void *dst,
			      size_t len)
{
	if (!tp || !dst)
		return -EINVAL;
	if (!tp->ops || !tp->ops->window_read)
		return -EOPNOTSUPP;

	return tp->ops->window_read(tp, off, dst, len);
}

FW_IF_TYPE smi_transport_type(struct smi_transport *tp)
{
	return tp ? tp->type : MAX_FW_IF_TYPE;
}

bool smi_transport_region_pa(struct smi_transport *tp, uint64_t *out)
{
	if (!tp || !out || (0u == tp->payload_pa))
		return false;

	*out = tp->payload_pa;
	return true;
}

/* What a log line calls a section of @kind. */
static const char *smi_pub_kind_word(uint16_t kind)
{
	switch (kind) {
	case SmiDescRegion_Status:
		return "Status";
	case SmiDescRegion_Sensors:
		return "Sensors";
	case SmiDescRegion_Counters:
		return "Counters";
	case SmiDescRegion_QsfpCage:
		return "cage";
	default:
		return "published";
	}
}

/* A section the mapping cannot hold is one this host does not read: dropped
 * here, so no reader sizes itself on a length past what it can reach. */
static void smi_pub_fit(struct smi_transport *tp, struct smi_span *sec,
			const char *what)
{
	const uint32_t room =
		READ_ONCE(tp->ro_base) ? tp->ro_len : tp->payload_len;

	if (sec->len && !smi_span_in(room, sec->start, sec->len)) {
		dev_warn(
			tp->dev,
			"%s section of %u B at +%u is past the %u B mapping: not read\n",
			what, sec->len, sec->start, room);
		sec->len = 0;
	}
}

void smi_transport_take_layout(struct smi_transport *tp,
			       const struct smi_layout *layout)
{
	uint32_t room;
	uint8_t i;

	if (!tp || !layout)
		return;

	/* Every value the probe answered, taken in one place: a backend that
	 * copied field by field left whichever field it forgot at zero, which
	 * reads as a card that stated nothing. */
	tp->bootstrapped = true;
	tp->mem_layout.data = layout->data;
	tp->mem_layout.gens = layout->gens;
	tp->pub = layout->pub;
	for (i = 0; i < tp->pub.region_cnt; i++)
		smi_pub_fit(tp, &tp->pub.region[i].span,
			    smi_pub_kind_word(tp->pub.region[i].kind));
	tp->lane_off = layout->lane_off;
	tp->build = layout->build;
	tp->build_valid = layout->build_valid;
	strscpy(tp->build_tag, layout->build_tag, sizeof(tp->build_tag));
	memcpy(tp->verbs, layout->verbs, sizeof(tp->verbs));
	tp->verb_cnt = layout->verb_cnt;
	tp->limits = layout->limits;

	/* Firmware and the mapping this host arrived at ship on different media,
	 * so a card can describe more than the host reserved. Clamped here
	 * because every backend passes through: a data region is capacity, and
	 * what a request may address is what both sides can reach. */
	room = smi_lane_room(tp->payload_len, tp->lane_off);
	if (((uint64_t)tp->mem_layout.data.start + tp->mem_layout.data.len) >
	    room) {
		dev_warn(
			tp->dev,
			"window claims %u B at +%u, the lane has %u B: clamping\n",
			tp->mem_layout.data.len, tp->mem_layout.data.start,
			room);
		tp->mem_layout.data.len =
			(tp->mem_layout.data.start < room)
				? (room - tp->mem_layout.data.start)
				: 0u;
	}
}

int smi_transport_ro_read(struct smi_transport *tp, uint32_t off, void *dst,
			  size_t len)
{
	const void *ro;

	if (!tp || !dst)
		return -EINVAL;

	/* A medium that hands out one base names the same bytes both ways, and
	 * reads them with its own accessor -- which for a BAR is not a memcpy.
	 * Only a lane that mapped the region itself reads it here. Snapshot it:
	 * a teardown NULLs the pointer under a cyclic reader. */
	ro = READ_ONCE(tp->ro_base);
	if (!ro)
		return smi_transport_window_read(tp, off, dst, len);
	if (!smi_span_in(tp->ro_len, off, len))
		return -ERANGE;

	memcpy(dst, (const uint8_t *)ro + off, len);

	return 0;
}

int smi_transport_ro_read32(struct smi_transport *tp, uint32_t off,
			    uint32_t *out)
{
	const void *ro;

	if (!tp || !out || !IS_ALIGNED(off, sizeof(*out)))
		return -EINVAL;

	/* One load either way: the medium's own for a window it hands out, a
	 * plain one for memory this host mapped. */
	ro = READ_ONCE(tp->ro_base);
	if (!ro) {
		if (!tp->ops || !tp->ops->window_read32)
			return -EOPNOTSUPP;
		return tp->ops->window_read32(tp, off, out);
	}
	if (!smi_span_in(tp->ro_len, off, sizeof(*out)))
		return -ERANGE;

	*out = READ_ONCE(*(const uint32_t *)((const uint8_t *)ro + off));

	return 0;
}

int fw_if_err_to_errno(uint32_t err)
{
	switch (err) {
	case FW_IF_ERRORS_NONE:
		return 0;
	case FW_IF_ERRORS_PARAMS:
	case FW_IF_ERRORS_INVALID_HANDLE:
	case FW_IF_ERRORS_INVALID_CFG:
	case FW_IF_ERRORS_UNRECOGNISED_OPTION:
		return -EINVAL;
	case FW_IF_ERRORS_TIMEOUT:
		return -ETIMEDOUT;
	case FW_IF_ERRORS_DRIVER_IN_USE:
		return -EBUSY;
	case FW_IF_ERRORS_DRIVER_NOT_INITIALISED:
		return -ENODEV;
	case FW_IF_ERRORS_DRIVER_RX_MODE:
		return -EOPNOTSUPP;
	case FW_IF_ERRORS_BINDING:
		return -ENOLINK;
	default:
		/* OPEN/CLOSE/WRITE/READ/IOCTRL and any unmapped code */
		return -EIO;
	}
}

void smi_transport_set_halted(struct smi_transport *tp, bool halted)
{
	if (!tp)
		return;

	mutex_lock(&tp->cmd_lock);
	WRITE_ONCE(tp->halted, halted);
	mutex_unlock(&tp->cmd_lock);
}

int smi_transport_submit_begin(struct smi_transport *tp)
{
	if (!tp)
		return -EINVAL;

	mutex_lock(&tp->cmd_lock);
	if (READ_ONCE(tp->halted)) {
		mutex_unlock(&tp->cmd_lock);
		return -ECANCELED;
	}

	return 0;
}

void smi_transport_submit_end(struct smi_transport *tp)
{
	mutex_unlock(&tp->cmd_lock);
}

int smi_transport_take_log_view(struct smi_transport *tp,
				const struct smi_log_view *view)
{
	uint64_t span;
	uint32_t room;

	if (!tp || !view)
		return -EINVAL;

	memset(&tp->log_view, 0, sizeof(tp->log_view));
	if (!view->rec_cnt)
		return 0; /* this window declares no log */

	/* A lane that mapped no read-only region reads its own window, so that
	 * is where the bound comes from. */
	room = (view->in_ro && READ_ONCE(tp->ro_base)) ? tp->ro_len
						       : tp->payload_len;
	span = (uint64_t)view->rec_base +
	       (uint64_t)view->rec_bytes * view->rec_cnt;

	if (!view->rec_bytes || view->rec_bytes > SMI_LOG_SNAPSHOT_BYTES ||
	    span > room) {
		TP_ERR(tp, "window states %u x %u B log records at +%u of %u B",
		       view->rec_cnt, view->rec_bytes, view->rec_base, room);
		return -EPROTO;
	}

	tp->log_view = *view;

	return 0;
}

/* Both offsets are in whichever space the dialect said its log lives in. */
static int smi_log_read(struct smi_transport *tp, uint32_t off, void *dst,
			size_t len)
{
	return tp->log_view.in_ro
		       ? smi_transport_ro_read(tp, off, dst, len)
		       : smi_transport_window_read(tp, off, dst, len);
}

uint32_t smi_transport_log_records(struct smi_transport *tp)
{
	return tp ? tp->log_view.rec_cnt : 0;
}

int smi_transport_log_index(struct smi_transport *tp, uint32_t *out_index)
{
	uint32_t raw = 0;
	int ret;

	if (!tp || !out_index)
		return -EINVAL;
	if (!tp->log_view.rec_cnt)
		return -EOPNOTSUPP;

	ret = smi_log_read(tp, tp->log_view.idx_off, &raw, sizeof(raw));
	if (ret)
		return ret;

	/* A slot, not a count: one dialect wraps its index, the other counts. */
	*out_index = raw % tp->log_view.rec_cnt;

	return 0;
}

int smi_transport_log_read_record(struct smi_transport *tp, uint32_t slot,
				  void *dst, size_t cap)
{
	if (!tp || !dst)
		return -EINVAL;
	if (!tp->log_view.rec_cnt)
		return -EOPNOTSUPP;
	if (slot >= tp->log_view.rec_cnt)
		return -ERANGE;
	if (cap < tp->log_view.rec_bytes)
		return -ENOSPC;

	return smi_log_read(
		tp, tp->log_view.rec_base + (slot * tp->log_view.rec_bytes),
		dst, tp->log_view.rec_bytes);
}

struct smi_transport *smi_transport_alloc(struct device *dev, FW_IF_TYPE type)
{
	struct smi_transport *tp;

	tp = devm_kzalloc(dev, sizeof(*tp), GFP_KERNEL);
	if (!tp)
		return NULL;

	tp->dev = dev;
	tp->type = type;
	mutex_init(&tp->cmd_lock);
	spin_lock_init(&tp->bulk.lock);
	init_waitqueue_head(&tp->bulk.wq);
	WRITE_ONCE(tp->halted, true);

	return tp;
}

/* Backend registry: add a backend by appending its descriptor here. */
static const struct smi_transport_backend *const smi_backends[] = {
	&gcq_transport_backend,
	&rpmsg_transport_backend,
};

struct smi_transport *smi_transport_create(struct device *dev, FW_IF_TYPE type,
					   const struct smi_transport_res *res)
{
	const struct smi_transport_backend *be = NULL;
	struct smi_transport *tp;
	size_t i;
	int ret;

	if (!dev)
		return ERR_PTR(-EINVAL);

	for (i = 0; i < ARRAY_SIZE(smi_backends); i++) {
		if (smi_backends[i]->type == type) {
			be = smi_backends[i];
			break;
		}
	}
	if (!be)
		return ERR_PTR(-EINVAL);

	tp = smi_transport_alloc(dev, type);
	if (!tp)
		return ERR_PTR(-ENOMEM);

	/* Propagate the backend's errno (bad view -EINVAL, not-ready
	 * -ETIMEDOUT, FAL code) so the caller keeps them. On failure the devm
	 * allocation is reclaimed at detach, like smc_proxy_init(). */
	ret = be->create(tp, res);
	if (ret)
		return ERR_PTR(ret);

	/* The medium has now said where its window is, so the slots are cut
	 * here rather than in each create(): a backend that forgot would have a
	 * transport whose every staged request is refused. */
	ret = smi_transport_slots_init(tp);
	if (ret) {
		dev_err(dev,
			"the window's data region cannot hold the staging slots\n");

		return ERR_PTR(ret);
	}

	return tp;
}

/* ---- Data-plane dispatch (thin wrappers over the backend ops vtable) ----
 * The optional-op NULL checks live here, so callers name a verb and never see
 * that an op may be absent. */

/*---- The staging slots ----------------------------------------------------
 * One class per cluster of request sizes, each cut from the region the window
 * declared. A caller gets a slot, not the region, so two requests can be in
 * flight at once, which the frame header's slice is what allows.
 *--------------------------------------------------------------------------*/

static struct smi_slot_class *slot_class(struct smi_transport *tp,
					 enum smi_region_type type)
{
	if (type == SMI_REGION_DATA)
		return &tp->bulk;

	return NULL;
}

/* Cut @count slots out of [start, start+len), aligned and whole lines long. */
static int slot_class_init(struct smi_transport *tp, struct smi_slot_class *c,
			   uint32_t start, uint32_t len, uint8_t count)
{
	const uint32_t base = ALIGN(start, SMI_SLOT_ALIGN);
	uint32_t size;

	BUILD_BUG_ON(SMI_SLOTS_BULK > 32);
	BUILD_BUG_ON(SMI_SLOT_ALIGN & (SMI_SLOT_ALIGN - 1));

	if (base < start || (base - start) >= len)
		return -ENOSPC;

	size = ALIGN_DOWN((len - (base - start)) / count, SMI_SLOT_ALIGN);
	if (!size)
		return -ENOSPC;

	c->base = base;
	c->size = size;
	c->count = count;
	/* Above the card's own table every request is refused at a round trip's
	 * cost, so the wait happens here instead; 0 is a window that states none. */
	c->cap = (tp->limits.max_inflight && tp->limits.max_inflight < count)
			 ? (uint8_t)tp->limits.max_inflight
			 : count;
	c->free_map = (count >= 32) ? ~0u : ((1u << count) - 1u);
	c->out = 0;
	c->retired_map = 0;

	dev_dbg(tp->dev, "staging: %u slots of %u bytes at 0x%x\n", count, size,
		base);

	return 0;
}

/* Cut the staging classes out of the window. Called once per transport, from
 * the create path above, with the layout the medium just filled. */
int smi_transport_slots_init(struct smi_transport *tp)
{
	uint32_t addr = 0;
	uint32_t len = 0;

	if (!tp)
		return -EINVAL;

	smi_mem_payload(&tp->mem_layout, &addr, &len);

	return slot_class_init(tp, &tp->bulk, addr, len, SMI_SLOTS_BULK);
}

/* Take a slot, waiting at most @timeout jiffies while the class is full. FIFO:
 * the flash's release-and-retake loop queues behind the waiter it woke. An
 * @uninterruptible wait does not end on a signal: it keeps its turn. */
static int slot_take(struct smi_slot_class *c, uint32_t *addr, uint32_t *len,
		     unsigned long timeout, bool uninterruptible)
{
	const unsigned long end = jiffies + timeout;
	DEFINE_WAIT(wait);
	unsigned long flags;
	bool queued = false;
	bool signalled;
	int bit;

	spin_lock_irqsave(&c->lock, flags);
	for (;;) {
		bit = ((queued || !c->waiters) && (c->out < c->cap))
			      ? ffs((int)c->free_map) - 1
			      : -1;
		if (bit >= 0) {
			c->free_map &= ~(1u << bit);
			c->out++;
			if (queued) {
				c->waiters--;
				/* Under the lock: a wake aimed at us once we
				 * stopped waiting is spent on nobody. */
				finish_wait(&c->wq, &wait);
			}
			spin_unlock_irqrestore(&c->lock, flags);
			*addr = c->base + ((uint32_t)bit * c->size);
			*len = c->size;
			return 0;
		}

		/* -EINTR is the region semaphore's, which callers tell from a
		 * device error; a slot still held at the deadline times out. */
		signalled = !uninterruptible && signal_pending(current);
		if (queued && (signalled || !time_before(jiffies, end))) {
			const int ret = signalled ? -EINTR : -ETIMEDOUT;

			c->waiters--;
			finish_wait(&c->wq, &wait);
			spin_unlock_irqrestore(&c->lock, flags);
			/* The turn we were given passes to whoever is next. */
			wake_up(&c->wq);
			return ret;
		}

		if (!queued) {
			c->waiters++;
			queued = true;
		}

		/* Exclusive, so one release wakes one waiter, in arrival order. */
		prepare_to_wait_exclusive(&c->wq, &wait,
					  uninterruptible ? TASK_UNINTERRUPTIBLE
							  : TASK_INTERRUPTIBLE);
		spin_unlock_irqrestore(&c->lock, flags);
		schedule_timeout(time_before(jiffies, end) ? end - jiffies : 1);
		spin_lock_irqsave(&c->lock, flags);
	}
}

/*
 * Give a slot back. @retire keeps it out of the pool -- see
 * smi_transport_retire_region().
 *
 * Retiring stops one short of emptying the class: a class with no slot left
 * cannot talk to the card at all, which is worse than reusing a slice whose
 * work has almost certainly ended. The one-slot class (the flash) never
 * retires -- there is no other slot to prefer.
 *
 * Return: true when the slot was retired rather than freed. */
static bool slot_give(struct smi_slot_class *c, uint32_t addr, bool retire)
{
	unsigned long flags;
	uint32_t bit;
	bool retired = false;

	if (!c->size || addr < c->base)
		return false;

	bit = (addr - c->base) / c->size;
	if (bit >= c->count)
		return false;

	spin_lock_irqsave(&c->lock, flags);
	if (c->out)
		c->out--;
	retired = retire && ((hweight32(c->retired_map) + 1u) < c->count);
	if (retired)
		c->retired_map |= (1u << bit);
	else
		c->free_map |= (1u << bit);
	spin_unlock_irqrestore(&c->lock, flags);

	wake_up(&c->wq);

	return retired;
}

int smi_transport_get_region(struct smi_transport *tp,
			     enum smi_region_type type, uint32_t *addr,
			     uint32_t *len, unsigned long timeout,
			     bool uninterruptible)
{
	struct smi_slot_class *c;

	if (!tp || !addr || !len)
		return -EINVAL;

	c = slot_class(tp, type);
	if (!c || !c->size)
		return -EINVAL;

	return slot_take(c, addr, len, timeout, uninterruptible);
}

uint32_t smi_transport_data_start(struct smi_transport *tp)
{
	return tp ? tp->mem_layout.data.start : 0u;
}

uint32_t smi_transport_slot_bytes(struct smi_transport *tp,
				  enum smi_region_type type)
{
	struct smi_slot_class *c = tp ? slot_class(tp, type) : NULL;

	return c ? c->size : 0u;
}

void smi_transport_put_region(struct smi_transport *tp,
			      enum smi_region_type type, uint32_t addr)
{
	struct smi_slot_class *c = tp ? slot_class(tp, type) : NULL;

	if (c)
		(void)slot_give(c, addr, false);
}

void smi_transport_retire_region(struct smi_transport *tp,
				 enum smi_region_type type, uint32_t addr)
{
	struct smi_slot_class *c = tp ? slot_class(tp, type) : NULL;

	if (!c)
		return;

	/* Unanswered, the work may still write INTO this slot: out of the pool
	 * until the transport returns or a card owing the answer sends it. */
	if (slot_give(c, addr, true))
		dev_warn(
			tp->dev,
			"staging slot 0x%x retired: no answer came for it (%u retired of %u)\n",
			addr, hweight32(c->retired_map), c->count);
	else
		dev_warn(
			tp->dev,
			"staging slot 0x%x reused unanswered: nothing else left in its class\n",
			addr);
}

uint32_t smi_transport_tx_capacity(struct smi_transport *tp)
{
	return tp ? tp->tx_capacity : 0;
}

const char *smi_transport_build_tag(struct smi_transport *tp)
{
	return tp ? tp->build_tag : "";
}

int smi_transport_build(struct smi_transport *tp, struct smi_identify_rsp *out)
{
	if (!tp || !out)
		return -EINVAL;
	if (!tp->build_valid)
		return -ENOENT;

	*out = tp->build;

	return 0;
}

const struct smi_desc_verb *smi_transport_verb(struct smi_transport *tp,
					       uint16_t opcode)
{
	uint8_t i;

	for (i = 0; tp && (i < tp->verb_cnt); i++)
		if (tp->verbs[i].opcode == opcode)
			return &tp->verbs[i];

	return NULL;
}

bool smi_transport_states_verbs(struct smi_transport *tp)
{
	return tp && tp->verb_cnt;
}

uint32_t smi_transport_verb_longest_ms(struct smi_transport *tp)
{
	uint32_t ms = 0;
	uint8_t i;

	for (i = 0; tp && (i < tp->verb_cnt); i++)
		ms = max(ms, tp->verbs[i].deadline_ms);

	return ms;
}

struct smi_desc_limits smi_transport_limits(struct smi_transport *tp)
{
	return tp ? tp->limits : (struct smi_desc_limits){ 0 };
}

uint32_t smi_transport_gens(struct smi_transport *tp)
{
	return tp ? tp->mem_layout.gens : 0u;
}

bool smi_transport_publishes(struct smi_transport *tp)
{
	const struct smi_span *status =
		tp ? smi_pub_find(&tp->pub, SmiDescRegion_Status, 0) : NULL;

	return tp && tp->pub.boot && status && status->len;
}

const struct smi_pub_layout *smi_transport_pub(struct smi_transport *tp)
{
	static const struct smi_pub_layout none;

	return tp ? &tp->pub : &none;
}

bool smi_transport_bootstrapped(struct smi_transport *tp)
{
	return tp && tp->bootstrapped;
}

int smi_transport_read(struct smi_transport *tp, uint32_t offset, void *dst,
		       size_t len)
{
	if (!tp || !tp->ops || !tp->ops->read)
		return -EINVAL;
	return tp->ops->read(tp, offset, dst, len);
}

int smi_transport_write(struct smi_transport *tp, uint32_t offset,
			const void *src, size_t len)
{
	if (!tp || !tp->ops || !tp->ops->write)
		return -EINVAL;
	return tp->ops->write(tp, offset, src, len);
}

/* Give the retired slots back. Only a start may do this: the work that might
 * still have been writing into them did not survive the stop, which is the one
 * event that makes those bytes the host's again.
 */
static void slot_class_reclaim(struct smi_slot_class *c)
{
	unsigned long flags;

	spin_lock_irqsave(&c->lock, flags);
	c->free_map |= c->retired_map;
	c->retired_map = 0;
	spin_unlock_irqrestore(&c->lock, flags);

	wake_up(&c->wq);
}

int smi_transport_start(struct smi_transport *tp)
{
	if (!tp)
		return -EINVAL;

	slot_class_reclaim(&tp->bulk);

	if (!tp->ops || !tp->ops->start)
		return 0; /* no start op: nothing to do */
	return tp->ops->start(tp);
}

void smi_transport_stop(struct smi_transport *tp)
{
	if (tp && tp->ops && tp->ops->stop)
		tp->ops->stop(tp);
}

int smi_transport_check_compatibility(struct smi_transport *tp, uint16_t major,
				      uint16_t minor)
{
	if (!tp || !tp->ops || !tp->ops->check_compatibility)
		return -ENOTSUPP;
	return tp->ops->check_compatibility(tp, major, minor);
}

void smi_transport_invalidate_io(struct smi_transport *tp)
{
	if (!tp)
		return;

	WRITE_ONCE(tp->base_virt_addr, NULL);
	WRITE_ONCE(tp->payload_base_virt_addr, NULL);
	WRITE_ONCE(tp->ring_buf_base_virt_addr, NULL);
	/* The read-only region is read by the same cyclic readers, so it is a
	 * teardown pointer like the others. */
	WRITE_ONCE(tp->ro_base, NULL);
}
