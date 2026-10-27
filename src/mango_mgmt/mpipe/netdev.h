/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MangoBoost Management Module - MPIPE Virtual Network Device
 *
 * Data link layer: per-frame XOR framing with sentinel delimiters
 * over the MPIPE word-oriented hardware FIFO.  The sentinel above
 * hw.c is just `0`; hw.c folds the wire-XOR (0xDEADBEEF) so on-wire
 * empty-FIFO reads decode as boundaries.  The per-frame XOR key is
 * picked by a bitset gap-search (see mpipe_pick_frame_key in netdev.c).
 *
 * Architecture (SPLIT_ARCH): softirq does protocol (NAT, frame-key
 * selection, GRO, stack delivery), kthreads do MMIO.  Joined by
 * lockless llist queues; one kthread per direction.
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_MPIPE_NETDEV_H_
#define _MANGO_MGMT_MPIPE_NETDEV_H_

#include <linux/atomic.h>
#include <linux/llist.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "nat.h"

struct mango_mgmt_dev;

#define MANGO_MGMT_MPIPE_NETDEV_MTU 9000

/* RX skb pre-allocation pool — kthread refills when idle to keep the
 * decoder hot path allocation-free. */
#define MPIPE_RX_SKB_POOL_SIZE 8

/* Bitset width for frame-key selection: 2^ENCODE_M_MAX > ceil(MTU/4) for
 * any supported MTU.  Per-frame the actual M is computed from nwords. */
#define ENCODE_M_MAX 15

struct mango_mgmt_mpipe_netdev_priv {
	struct mango_mgmt_dev *mgmt_dev;

	/* TX: xmit picks frame key (lockless), kthread does MMIO */
	struct llist_head tx_llist; /* lockless SPSC skb queue */
	struct task_struct *tx_thread;
	wait_queue_head_t tx_wq;
	unsigned long __percpu *encode_bitset_pcpu; /* per-CPU, NUMA-local */
	size_t tx_avail; /* soft cache, TX kthread only */

	/* TX kthread's pending-softirq flag — set after each
	 * netdev_completed_queue (can reopen BQL → NET_TX), consumed by
	 * the matching mpipe_drain_pending_softirqs call.  Touched only
	 * from TX kthread context; plain bool. */
	bool tx_softirq_dirty;

	/* RX: kthread does MMIO + decode, NAPI does NAT + GRO */
	struct llist_head rx_llist; /* lockless SPSC skb queue */
	struct llist_node *rx_pending; /* NAPI-only carry-over from
					* previous poll (budget exhausted).
					* Drain before pulling rx_llist to
					* preserve FIFO order. */
	struct task_struct *rx_thread;
	wait_queue_head_t rx_wq;
	struct napi_struct rx_napi;
	size_t rx_avail; /* soft cache, RX kthread only */

	/* RX kthread's pending-softirq flag — set after napi_schedule
	 * (NET_RX), consumed by the drain site in the RX kthread loop.
	 * Touched only from RX kthread context; plain bool. */
	bool rx_softirq_dirty;

	/* RX frame reassembly state (kthread only — no locking) */
	struct sk_buff *rx_skb; /* in-progress frame, NULL = idle/draining */
	u32 rx_frame_key; /* XOR key for current frame */
	size_t rx_payload_words; /* words appended to rx_skb so far */
	bool rx_draining; /* oversized frame: drain to sentinel */

	/* RX skb pre-allocation pool (kthread only) */
	struct sk_buff *rx_skb_pool[MPIPE_RX_SKB_POOL_SIZE];
	int rx_skb_pool_count;

	/* Counters */
	atomic_long_t tx_words;
	atomic_long_t rx_words;
	atomic_long_t rx_resyncs;

	/* Host-side NAT state (inactive on SoC) */
	bool is_soc; /* cached from transport at alloc time */
	struct mango_mgmt_mpipe_nat nat;
	struct work_struct nat_ip_change_work;
};

static inline bool mango_mgmt_link_ready(struct mango_mgmt_mpipe_nat *nat,
					 struct net_device *ndev)
{
	return netif_running(ndev) && mango_mgmt_nat_ready(nat);
}

/* Lifecycle — init brings up the MPIPE HW layer, its IRQ sources and the
 * netdev (down, no IP).  exit tears down in reverse. */
int mango_mgmt_mpipe_init(struct mango_mgmt_dev *dev);
void mango_mgmt_mpipe_exit(struct mango_mgmt_dev *dev);

/* Around a bus reset: the IRQ sources go and come back, the netdev stays
 * registered (detached in between). No-ops without an MPIPE HW context. */
void mango_mgmt_mpipe_quiesce(struct mango_mgmt_dev *dev);
int mango_mgmt_mpipe_resume(struct mango_mgmt_dev *dev);

/* Accessors — return NULL before mango_mgmt_mpipe_init() completes.
 * mango_mgmt_mpipe_get_nat() is declared in nat.h so libsmi-tainted
 * callers (api.c) can reach it without pulling in netdevice.h. */
struct net_device *mango_mgmt_mpipe_get_netdev(struct mango_mgmt_dev *dev);

/* Override the auto-derived SoC external IP with a user-supplied value
 * (sysfs setter for `soc_ip`).  Validated against the host_ip's subnet;
 * see implementation for the full error matrix. */
int mango_mgmt_mpipe_nat_set_soc_ip(struct mango_mgmt_dev *dev,
				    __be32 new_soc_ext);

/* Per-driver notifier registration (call once from mod.c) */
int mango_mgmt_mpipe_netdev_init_notifiers(void);
void mango_mgmt_mpipe_netdev_exit_notifiers(void);

#endif /* _MANGO_MGMT_MPIPE_NETDEV_H_ */
