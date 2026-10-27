// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - MPIPE Virtual Network Device
 *
 * SPLIT_ARCH design: softirq does protocol (NAT, frame-key
 * selection, GRO), kthreads do MMIO.  Joined by lockless llist
 * queues; one kthread per direction.
 *
 * Wire format (in the discovery domain — hw.c layers WIRE_XOR
 * underneath, see mpipe/hw.c):
 *
 *   [0...]  [frame_key]  [w0^key  w1^key  ...]  [0]
 *   sentinels  header         payload          sentinel
 *
 * The encoder picks `frame_key` such that K != 0 and K != W for any
 * payload word W, so no internal word equals 0.  Decoder skips 0s,
 * reads the first non-zero word as the key, XOR-decodes the
 * payload, stops on the next 0.
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/inetdevice.h>
#include <linux/ip.h>
#include <linux/kthread.h>
#include <linux/llist.h>
#include <linux/netdevice.h>
#include <linux/nmi.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/tcp.h>
#include <linux/topology.h>
#include <linux/udp.h>

#include <linux/wait.h>
#include <net/checksum.h>
#if __has_include(<linux/unaligned.h>)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif

#include <mango_compat/netdev.h>

#include "../dev.h"
#include "../transport/transport.h"
#include "hw.h"
#include "netdev.h"

/* TX wake threshold: arm the IRQ to fire when the FIFO has at least this
 * many free slots — i.e. enough room to make a non-trivial batch of
 * progress before re-blocking. */
#define MPIPE_TX_WAKE_FREE_SLOTS (MANGO_MGMT_MPIPE_QUEUE_SIZE / 4)

/* RX wake threshold: arm the IRQ to fire on the first available word.
 * Asymmetric with TX on purpose — RX latency matters and the kthread
 * drains opportunistically anyway. */
#define MPIPE_RX_WAKE_AVAIL_WORDS 1

/* TX kthread idle-wait budget — bound missed-wake-up latency.
 * Use max_t() to avoid msecs_to_jiffies(1) rounding to 0 on HZ=250
 * (Ubuntu server default), which would degenerate into busy-poll. */
#define MPIPE_TX_WAKE_TIMEOUT_JIFFIES max_t(long, 1, msecs_to_jiffies(1))

/* If the TX kthread can't make progress for this long while it has
 * skbs to send, declare the link dead (netif_carrier_off).  The
 * qdisc then stops trying, the kernel's dev_watchdog stops firing,
 * and dmesg stays quiet.  Carrier is restored as soon as either
 * direction sees real silicon traffic again. */
#define MPIPE_CARRIER_DOWN_JIFFIES (2 * HZ)

/* ===== skb private storage =====
 *
 * skb->cb is 48 bytes of driver-private space; we stash the per-frame
 * key (discovery-domain) plus the llist_node used to thread the skb
 * through tx_llist / rx_llist.  Reverse helper recovers the skb pointer
 * from any llist_node we issued. */
struct mpipe_skb_cb {
	u32 frame_key; /* per-frame discovery-domain key (sentinel = 0) */
	struct llist_node ll; /* lockless queue linkage */
};

#define MPIPE_SKB_CB(skb) ((struct mpipe_skb_cb *)(skb)->cb)

static inline struct sk_buff *mpipe_skb_from_ll_node(struct llist_node *node)
{
	struct mpipe_skb_cb *cb = container_of(node, struct mpipe_skb_cb, ll);

	return container_of((void *)cb, struct sk_buff, cb);
}

/* Wire-encoded byte count: [key] + [N words] + [sentinel].
 * Single source of truth — used by xmit (netdev_sent_queue),
 * TX kthread (netdev_completed_queue), and shutdown drain. */
static inline size_t mpipe_encoded_size(struct sk_buff *skb)
{
	return (DIV_ROUND_UP(skb->len, 4) + 2) * sizeof(u32);
}

/* ===== Frame-key selection (bitset gap search) =====
 *
 * Need K != 0 and K != W for every payload W (so no internal word
 * is the discovery-domain sentinel = 0).  Build a bitset of
 * W & mask for some mask = 2^M - 1 where 2^M > N (pigeonhole
 * guarantees a gap).  The gap index gives K's lower-M bits;
 * fix-up if gap == 0 to ensure K != 0. */
static void mpipe_pick_frame_key(struct mango_mgmt_mpipe_netdev_priv *priv,
				 struct sk_buff *skb)
{
	size_t nwords = DIV_ROUND_UP(skb->len, 4);
	size_t full = skb->len / 4;
	unsigned long *used = this_cpu_ptr(priv->encode_bitset_pcpu);
	size_t m, bits;
	u32 mask, gap;
	size_t i;
	bool aligned;

	if (unlikely(nwords == 0)) {
		MPIPE_SKB_CB(skb)->frame_key = 1;
		return;
	}

	m = fls(nwords); /* smallest M s.t. 2^M > nwords */
	bits = 1UL << m;
	mask = (u32)(bits - 1);

	bitmap_zero(used, bits);

	aligned = !((unsigned long)skb->data & 3);
	if (likely(aligned)) {
		const u32 *words = (const u32 *)skb->data;

		for (i = 0; i < full; i++)
			__set_bit(words[i] & mask, used);
	} else {
		for (i = 0; i < full; i++)
			__set_bit(get_unaligned(
					  (const u32 *)(skb->data + i * 4)) &
					  mask,
				  used);
	}

	if (skb->len & 3) {
		u32 w = 0;

		memcpy(&w, skb->data + full * 4, skb->len & 3);
		__set_bit(w & mask, used);
	}

	gap = find_first_zero_bit(used, bits);

	/* gap == 0 means no payload word has lower-m bits == 0, so K = 0
	 * would satisfy K != W for all W — but K = 0 is itself forbidden
	 * (the header would equal the sentinel).  Pick K = 1<<m: lower-m
	 * bits stay 0 (so K & mask still maps to "no W" via the same
	 * pigeonhole argument), bit m makes K nonzero. */
	MPIPE_SKB_CB(skb)->frame_key = (gap == 0) ? (u32)(1u << m) : gap;
}

/* ===== NAT helpers ===== */

static void mango_mgmt_mpipe_nat_fix_l4_csum(struct sk_buff *skb,
					     struct iphdr *iph, __be32 old_ip,
					     __be32 new_ip)
{
	__sum16 *csum;

	if (iph->protocol == IPPROTO_TCP) {
		csum = &tcp_hdr(skb)->check;
	} else if (iph->protocol == IPPROTO_UDP) {
		csum = &udp_hdr(skb)->check;
		if (!*csum) /* UDP checksum 0 = not computed */
			return;
	} else {
		return; /* ICMP etc. — no pseudo-header */
	}

	inet_proto_csum_replace4(csum, skb, old_ip, new_ip, true);
}

/*
 * Returns true if the packet should proceed to the wire, false if the
 * caller should drop it.  When NAT is active and the destination is not
 * the SoC's external IP, drop — the only valid peer on this
 * point-to-point link is the SoC, addressed via its external alias.
 *
 * Letting non-soc_ext packets through would silently rewrite their dst
 * to the SoC's internal IP and bridge them to the SoC anyway, producing
 * confusing replies (`from <soc_ext>: DIFFERENT ADDRESS!`) that leak
 * the internal/external split.  Such packets are dropped in xmit and
 * counted as TX drops instead.
 */
static bool mpipe_tx_nat(struct mango_mgmt_mpipe_netdev_priv *priv,
			 struct sk_buff *skb)
{
	long ext;
	struct iphdr *iph;
	__be32 soc_int, soc_ext, old_dst;

	if (priv->is_soc)
		return true;

	ext = atomic_long_read(&priv->nat.external);
	if (ext == MANGO_MGMT_IP_PAIR_UNSET ||
	    skb->protocol != htons(ETH_P_IP) || skb->len < sizeof(struct iphdr))
		return true;

	iph = (struct iphdr *)skb->data;
	soc_ext = nat_soc(ext);
	old_dst = iph->daddr;

	if (old_dst != soc_ext)
		return false;

	soc_int = (__force __be32)atomic_read(&priv->nat.soc_internal_ip);
	iph->daddr = soc_int;
	inet_proto_csum_replace4(&iph->check, skb, old_dst, soc_int, false);
	mango_mgmt_mpipe_nat_fix_l4_csum(skb, iph, old_dst, soc_int);
	return true;
}

static void mpipe_rx_nat(struct mango_mgmt_mpipe_netdev_priv *priv,
			 struct sk_buff *skb)
{
	long ext;
	struct iphdr *iph;
	__be32 soc_ext, old_src;

	if (priv->is_soc)
		return;

	ext = atomic_long_read(&priv->nat.external);
	if (ext == MANGO_MGMT_IP_PAIR_UNSET || skb->len < sizeof(struct iphdr))
		return;

	/* Caller (mpipe_rx_napi_poll) has already filtered out IPv6;
	 * anything else parses as an IPv4 header here.  If the frame
	 * really isn't IPv4 we just do one useless saddr/daddr rewrite
	 * on non-IP bytes — the kernel stack rejects it downstream. */
	iph = (struct iphdr *)skb->data;
	soc_ext = nat_soc(ext);
	old_src = iph->saddr;

	if (old_src != (__force __be32)atomic_read(&priv->nat.soc_internal_ip))
		atomic_set(&priv->nat.soc_internal_ip, (__force int)old_src);

	skb_reset_network_header(skb);
	skb_set_transport_header(skb, iph->ihl * 4);

	iph->saddr = soc_ext;
	inet_proto_csum_replace4(&iph->check, skb, old_src, soc_ext, false);
	mango_mgmt_mpipe_nat_fix_l4_csum(skb, iph, old_src, soc_ext);
}

/* ===== ndo_start_xmit ===== */

static netdev_tx_t mpipe_netdev_xmit(struct sk_buff *skb,
				     struct net_device *ndev)
{
	struct mango_mgmt_mpipe_netdev_priv *priv = netdev_priv(ndev);

	/* Drop frames we can't encode — silent like a wire drop.
	 * Also drop IPv6: the NAT layer parses iphdr from skb->data, so
	 * an IPv6 header (e.g. link-up RS/NS) would be mis-read as IPv4.
	 * Any other protocol is passed through unchanged — tx_nat's
	 * own skb->protocol check keeps it out of the NAT path.
	 * Orphan first so socket wmem releases immediately, matching
	 * what the TX kthread does on the success path. */
	if (unlikely(!skb->len || skb->len > MANGO_MGMT_MPIPE_NETDEV_MTU ||
		     skb->protocol == htons(ETH_P_IPV6)))
		goto drop;

	/* NAT (Host only) — rewrite dst IP to SoC's internal address.
	 * Returns false if the destination isn't the SoC's external IP;
	 * drop those rather than silently bridging them to the SoC. */
	if (!mpipe_tx_nat(priv, skb))
		goto drop;

	/* Find frame key via bitset scan — O(N), no scratch copy */
	mpipe_pick_frame_key(priv, skb);

	/* Report to BQL — must run before llist_add so the kthread's
	 * netdev_completed_queue is always preceded by this charge. */
	netdev_sent_queue(ndev, mpipe_encoded_size(skb));

	/* Enqueue for TX kthread — lockless, zero-copy.  llist_add's
	 * cmpxchg provides a release barrier; llist_del_all's xchg in
	 * the kthread is the matching acquire — guarantees the kthread
	 * sees the NAT rewrites and frame_key in skb. */
	llist_add(&MPIPE_SKB_CB(skb)->ll, &priv->tx_llist);

	/* Wake kthread on the last packet of a qdisc batch.  Inside a
	 * batch netdev_xmit_more() is true and we batch the wake. */
	if (!netdev_xmit_more())
		wake_up(&priv->tx_wq);

	return NETDEV_TX_OK;

drop:
	skb_orphan(skb);
	dev_kfree_skb_any(skb);
	ndev->stats.tx_dropped++;
	return NETDEV_TX_OK;
}

/* ===== TX kthread + helpers ===== */

/*
 * Drain any NET_RX/NET_TX softirqs raised from this kthread before
 * going to sleep.  Used by both the TX and RX kthreads.
 *
 * napi_schedule() (RX path) and netdev_completed_queue() → BQL reopen
 * → netif_schedule_queue() (TX path) raise softirqs on the current
 * CPU.  In softirq / IRQ context the kernel drains them on return,
 * but we're in a process-context kthread: the softirq stays pending
 * until the next bh_enable, IRQ return, or ksoftirqd run.  If the
 * kthread then sleeps in wait_event_*(), the CPU may enter tickless
 * idle with the softirq still pending — nohz prints "local softirq
 * work is pending, handler #08" (NET_RX) or #06 (NET_TX).
 *
 * local_bh_enable() runs any pending softirqs inline, so bracketing a
 * no-op with disable/enable is a one-shot drain.  The *dirty flag is
 * the fast-path gate: only pay the bh_disable/enable round-trip when
 * the owning kthread has actually hit a softirq-raising path since
 * the last drain.  Flag is touched only by its owning kthread.
 */
static inline void mpipe_drain_pending_softirqs(bool *dirty)
{
	if (!*dirty)
		return;
	local_bh_disable();
	local_bh_enable();
	*dirty = false;
}

/*
 * Wait until the HW FIFO has space.  Called when the soft-cached
 * tx_avail hits 0.  Arms the TX Low-WM IRQ for the duration of the
 * wait, disarms on exit — the shared MMIO handler's skip-when-disabled
 * fast path relies on this bracketing so idle RX-only IRQs don't pay
 * for a TX status read.
 *
 * Wake signal is derived from the existing tx_irq_count atomic: take
 * a snapshot before arming and use "count changed" as the wait
 * condition, so wait_event itself doesn't poll an MMIO read on every
 * evaluation.  On IRQ wake the HW's `fire when fill < 3*QS/4`
 * invariant guarantees at least QS/4 free slots — grant that without
 * an MMIO read (matters on PCIe; near-free on MMIO).
 *
 * Does NOT flush BQL with a partial byte count: the frame's final
 * netdev_completed_queue accounts for the whole encoded size in one
 * shot.
 *
 * Carrier model: if we stay blocked here for longer than
 * MPIPE_CARRIER_DOWN_JIFFIES, the peer is presumed dead and we mark
 * the link netif_carrier_off.  The qdisc then stops pushing into us
 * and the kernel watchdog stops firing tx-timeouts.  As soon as the
 * peer drains the FIFO (we make progress) or sends us anything
 * (RX kthread observes data), carrier flips back on.
 */
static int mpipe_wait_tx_space(struct mango_mgmt_mpipe_netdev_priv *priv,
			       struct net_device *ndev)
{
	unsigned long start = jiffies;
	long irq_baseline;
	int ret;

	irq_baseline = mango_mgmt_mpipe_hw_tx_irq_count(priv->mgmt_dev);

	mango_mgmt_mpipe_hw_enable_tx_irq(priv->mgmt_dev,
					  MPIPE_TX_WAKE_FREE_SLOTS);

	while (!kthread_should_stop()) {
		size_t avail;

		mpipe_drain_pending_softirqs(&priv->tx_softirq_dirty);

		/* One status-register read right before wait_event (local
		 * AXI on SoC, expensive PCIe read on Host).  Serves both
		 * as the edge-check on the first iteration and the HZ-
		 * timeout salvage on subsequent iterations. */
		avail = mango_mgmt_mpipe_hw_tx_available(priv->mgmt_dev);
		if (avail > 0) {
			priv->tx_avail = avail;
			ret = 0;
			goto out;
		}

		wait_event_interruptible_timeout(
			priv->tx_wq,
			kthread_should_stop() ||
				mango_mgmt_mpipe_hw_tx_irq_count(
					priv->mgmt_dev) != irq_baseline,
			HZ);

		if (kthread_should_stop()) {
			ret = -EINTR;
			goto out;
		}

		if (mango_mgmt_mpipe_hw_tx_irq_count(priv->mgmt_dev) !=
		    irq_baseline) {
			/* Low-WM fired — HW guarantees free >= QS/4.
			 * Skip the status read entirely. */
			priv->tx_avail = MPIPE_TX_WAKE_FREE_SLOTS;
			ret = 0;
			goto out;
		}

		/* HZ timeout without an IRQ.  Next iteration will do the
		 * status-register read above; long enough blocked means
		 * the peer is presumed dead — the state change is
		 * observable via netif_carrier_ok() and the IFF_LOWER_UP
		 * flag (NO-CARRIER in `ip link`), so no log needed. */
		if (netif_carrier_ok(ndev) &&
		    time_after(jiffies, start + MPIPE_CARRIER_DOWN_JIFFIES))
			netif_carrier_off(ndev);
	}
	ret = -EINTR;

out:
	mango_mgmt_mpipe_hw_disable_tx_irq(priv->mgmt_dev);
	return ret;
}

/*
 * Emit one skb on the wire: [key] [word^key ...] [0].  Reads directly
 * from skb->data, XORs on the fly.  hw_send applies the wire-XOR
 * internally — we deal in clean discovery-domain values here.
 *
 * BQL: on -EINTR (shutdown), completes the FULL encoded size to keep
 * BQL balanced — bytes were charged in xmit and must be accounted
 * even if partially sent.  tx_words counts words actually written, so
 * uses pos (the partial count).
 */
static void mpipe_tx_emit_skb(struct mango_mgmt_mpipe_netdev_priv *priv,
			      struct net_device *ndev, struct sk_buff *skb)
{
	u32 key = MPIPE_SKB_CB(skb)->frame_key;
	const u8 *data = skb->data;
	size_t nwords = DIV_ROUND_UP(skb->len, 4);
	const size_t total_wire_words =
		nwords + 2; /* key + payload + sentinel */
	const bool aligned = !((unsigned long)data & 3);
	const size_t tail = skb->len & 3;
	u32 word;
	size_t pos = 0;

	while (pos < total_wire_words) {
		/* Soft cache: priv->tx_avail is our local estimate.  HW
		 * value only INCREASES between our reads, so cached <=
		 * actual always holds — false-low is safe. */
		if (priv->tx_avail == 0) {
			priv->tx_avail = mango_mgmt_mpipe_hw_tx_available(
				priv->mgmt_dev);
			if (priv->tx_avail == 0) {
				if (mpipe_wait_tx_space(priv, ndev))
					goto err_shutdown;
				continue;
			}
		}

		while (priv->tx_avail > 0 && pos < total_wire_words) {
			u32 out;

			if (pos == 0) {
				out = key;
			} else if (pos <= nwords) {
				size_t wi = pos - 1;

				if (unlikely(tail && wi == nwords - 1)) {
					word = 0;
					memcpy(&word, data + wi * 4, tail);
				} else if (likely(aligned)) {
					word = ((const u32 *)data)[wi];
				} else {
					word = get_unaligned(
						(const u32 *)(data + wi * 4));
				}
				out = word ^ key;
			} else {
				out = 0; /* sentinel */
			}

			mango_mgmt_mpipe_hw_send(priv->mgmt_dev, out);
			pos++;
			priv->tx_avail--;
		}
	}

	netdev_completed_queue(ndev, 1, total_wire_words * sizeof(u32));
	touch_softlockup_watchdog();
	priv->tx_softirq_dirty = true;
	atomic_long_add(pos, &priv->tx_words);
	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;

	/* TX progress = peer is draining FIFO = link is alive.
	 * netif_carrier_on flips IFF_LOWER_UP, which is the
	 * user-visible signal — no extra log needed. */
	if (unlikely(!netif_carrier_ok(ndev)))
		netif_carrier_on(ndev);
	return;

err_shutdown:
	/* BQL was charged total_wire_words in xmit — must complete the
	 * full amount even on partial send to keep BQL balanced. */
	netdev_completed_queue(ndev, 1, total_wire_words * sizeof(u32));
	priv->tx_softirq_dirty = true;
	atomic_long_add(pos, &priv->tx_words);
}

static int mpipe_netdev_tx_thread(void *data)
{
	struct net_device *ndev = data;
	struct mango_mgmt_mpipe_netdev_priv *priv = netdev_priv(ndev);

	while (!kthread_should_stop()) {
		struct llist_node *node;

		/* Fast path: try to pick up work first.  Only drain
		 * pending softirqs and sleep if there's nothing to do —
		 * calling local_bh_enable() on the hot path would run any
		 * NET_RX NAPI inline in this kthread and cripple
		 * throughput under concurrent RX. */
		node = llist_del_all(&priv->tx_llist);
		if (!node) {
			mpipe_drain_pending_softirqs(&priv->tx_softirq_dirty);

			/* Timeout safety net: bound missed-wake-up latency */
			wait_event_interruptible_timeout(
				priv->tx_wq,
				kthread_should_stop() ||
					!llist_empty(&priv->tx_llist),
				MPIPE_TX_WAKE_TIMEOUT_JIFFIES);
			if (kthread_should_stop())
				break;
			continue;
		}

		/* Reverse for FIFO order.  llist_del_all's xchg pairs
		 * with llist_add's cmpxchg in xmit — we observe the NAT
		 * rewrites and frame_key. */
		node = llist_reverse_order(node);

		while (node) {
			struct llist_node *next = node->next;
			struct sk_buff *skb = mpipe_skb_from_ll_node(node);

			mpipe_tx_emit_skb(priv, ndev, skb);
			skb_orphan(skb);
			dev_kfree_skb_any(skb);
			node = next;
		}
	}

	return 0;
}

/* ===== RX skb pre-allocation pool ===== */

static struct sk_buff *
mpipe_rx_alloc_skb(struct mango_mgmt_mpipe_netdev_priv *priv,
		   struct net_device *ndev)
{
	if (priv->rx_skb_pool_count > 0)
		return priv->rx_skb_pool[--priv->rx_skb_pool_count];
	return netdev_alloc_skb(ndev, MANGO_MGMT_MPIPE_NETDEV_MTU);
}

static void mpipe_rx_refill_pool(struct mango_mgmt_mpipe_netdev_priv *priv,
				 struct net_device *ndev)
{
	while (priv->rx_skb_pool_count < MPIPE_RX_SKB_POOL_SIZE) {
		struct sk_buff *skb =
			netdev_alloc_skb(ndev, MANGO_MGMT_MPIPE_NETDEV_MTU);

		if (!skb)
			break;
		priv->rx_skb_pool[priv->rx_skb_pool_count++] = skb;
	}
}

/* ===== RX: per-word state machine =====
 *
 * Persistent state across kthread iterations: rx_skb (in-progress
 * frame), rx_frame_key (key for current frame), rx_payload_words
 * (cursor), rx_draining (oversized frame: skip to next sentinel).
 *
 * Returns:
 *   non-NULL skb: a complete frame ready for the rx_llist;
 *   NULL: no frame yet (still scanning sentinels, mid-frame, or
 *         resyncing after a decode error — internal rx state advanced).
 */
static struct sk_buff *
mpipe_rx_consume_word(struct mango_mgmt_mpipe_netdev_priv *priv,
		      struct net_device *ndev, u32 word)
{
	/* State: draining oversized frame — skip until sentinel */
	if (priv->rx_draining) {
		if (word == 0) {
			priv->rx_draining = false;
			atomic_long_inc(&priv->rx_resyncs);
		}
		return NULL;
	}

	/* State: idle — skip leading sentinels, take the first non-zero
	 * word as the frame key, then start filling. */
	if (!priv->rx_skb) {
		if (word == 0)
			return NULL; /* still scanning sentinels */

		priv->rx_skb = mpipe_rx_alloc_skb(priv, ndev);
		if (!priv->rx_skb) {
			ndev->stats.rx_dropped++;
			/* Treat the missing skb as a frame we can't accept;
			 * drain to the next sentinel before resuming. */
			priv->rx_draining = true;
			return NULL;
		}
		priv->rx_frame_key = word;
		priv->rx_payload_words = 0;
		return NULL;
	}

	/* State: filling — sentinel ends the frame */
	if (word == 0) {
		struct sk_buff *skb = priv->rx_skb;
		u32 *payload = (u32 *)skb->data;
		size_t i;

		priv->rx_skb = NULL;

		for (i = 0; i < priv->rx_payload_words; i++)
			payload[i] ^= priv->rx_frame_key;

		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += skb->len;
		return skb;
	}

	/* MTU overflow — discard, drain to sentinel */
	if (priv->rx_payload_words * sizeof(u32) >=
	    MANGO_MGMT_MPIPE_NETDEV_MTU) {
		dev_kfree_skb_any(priv->rx_skb);
		priv->rx_skb = NULL;
		priv->rx_draining = true;
		return NULL;
	}

	skb_put_data(priv->rx_skb, &word, sizeof(word));
	priv->rx_payload_words++;
	return NULL;
}

/* ===== RX kthread ===== */

static int mpipe_netdev_rx_thread(void *data)
{
	struct net_device *ndev = data;
	struct mango_mgmt_mpipe_netdev_priv *priv = netdev_priv(ndev);

	while (!kthread_should_stop()) {
		bool produced;

		/* Soft-cache fast path: prior iteration may have left
		 * rx_avail > 0 with more words to drain; skip the arm/
		 * read dance. */
		if (priv->rx_avail == 0) {
			long baseline;

			mpipe_rx_refill_pool(priv, ndev);
			mpipe_drain_pending_softirqs(&priv->rx_softirq_dirty);

			/* Arm the IRQ and snapshot the counter before the
			 * final status read.  The HW's RX high-WM fires on
			 * the peer write that crosses the threshold — it
			 * does NOT retrofire writes that happened while the
			 * WM was disarmed.  Reading the status register
			 * AFTER arming is the edge-check that catches those
			 * stale writes; without it the kthread could miss
			 * the last peer write before a stall and sleep
			 * forever waiting for an IRQ that never comes (C2H
			 * FIFO fills, peer's TX blocks, deadlock). */
			baseline = mango_mgmt_mpipe_hw_rx_irq_count(
				priv->mgmt_dev);
			mango_mgmt_mpipe_hw_enable_rx_irq(
				priv->mgmt_dev, MPIPE_RX_WAKE_AVAIL_WORDS);

			/* One status read right before wait_event. */
			priv->rx_avail = mango_mgmt_mpipe_hw_rx_available(
				priv->mgmt_dev);
			if (priv->rx_avail == 0) {
				wait_event_interruptible(
					priv->rx_wq,
					kthread_should_stop() ||
						mango_mgmt_mpipe_hw_rx_irq_count(
							priv->mgmt_dev) !=
							baseline);
			}

			mango_mgmt_mpipe_hw_disable_rx_irq(priv->mgmt_dev);
			if (kthread_should_stop())
				break;
			continue; /* re-read cache / drain on next iteration */
		}

		/* Any RX activity = peer is alive = link is up.  State
		 * change is observable via IFF_LOWER_UP — no log. */
		if (unlikely(!netif_carrier_ok(ndev)))
			netif_carrier_on(ndev);

		/* Drain rx_avail words straight from HW into the state
		 * machine — no kfifo, no second copy. */
		produced = false;
		while (priv->rx_avail > 0) {
			u32 word = mango_mgmt_mpipe_hw_recv(priv->mgmt_dev);
			struct sk_buff *skb;

			priv->rx_avail--;
			atomic_long_inc(&priv->rx_words);

			skb = mpipe_rx_consume_word(priv, ndev, word);
			if (skb) {
				llist_add(&MPIPE_SKB_CB(skb)->ll,
					  &priv->rx_llist);
				produced = true;
			}
		}

		if (produced) {
			napi_schedule(&priv->rx_napi);
			touch_softlockup_watchdog();
			priv->rx_softirq_dirty = true;
		}
	}

	return 0;
}

/* ===== RX NAPI poll (lightweight: protocol only) ===== */

static int mpipe_rx_napi_poll(struct napi_struct *napi, int budget)
{
	struct mango_mgmt_mpipe_netdev_priv *priv = container_of(
		napi, struct mango_mgmt_mpipe_netdev_priv, rx_napi);
	struct net_device *ndev = napi->dev;
	struct llist_node *node;
	int packets = 0;

	/* Resume from previous poll's leftover */
	node = priv->rx_pending;
	priv->rx_pending = NULL;

	while (packets < budget) {
		struct llist_node *next;
		struct sk_buff *skb;

		if (!node) {
			node = llist_del_all(&priv->rx_llist);
			if (!node)
				break; /* both sources dry */
			node = llist_reverse_order(node);
		}

		next = node->next;
		skb = mpipe_skb_from_ll_node(node);

		/* Drop IPv6: the NAT layer's iphdr-based rewrite would
		 * mis-parse it (fe80::/64 zeros land on the IPv4 saddr
		 * offset and poison soc_internal_ip).  Other protocols
		 * pass through unchanged; the NAT layer skips them. */
		if (skb->len >= sizeof(struct iphdr) &&
		    ((struct iphdr *)skb->data)->version == 6) {
			ndev->stats.rx_dropped++;
			dev_kfree_skb_any(skb);
			node = next;
			continue;
		}

		mpipe_rx_nat(priv, skb);
		skb->protocol = htons(ETH_P_IP);
		skb_reset_network_header(skb);
		skb->dev = ndev;
		skb->pkt_type = PACKET_HOST;

		napi_gro_receive(napi, skb); /* now safe to clobber skb->cb */
		packets++;
		node = next;
	}

	priv->rx_pending = node;

	/* napi_complete_done credits `packets` to GRO and gives the
	 * scheduler a better hint than the bare napi_complete().  In
	 * particular it lets the kernel know how much we actually did
	 * this round, which silences the "NOHZ tick-stop error: local
	 * softirq work is pending" warnings when NAPI is busy. */
	if (!node && llist_empty(&priv->rx_llist))
		napi_complete_done(napi, packets);

	return packets;
}

/* ===== IRQ callbacks (registered with hw.c) ===== */

static void mango_mgmt_mpipe_tx_irq_cb(struct mango_mgmt_dev *dev)
{
	struct net_device *ndev = dev->mgmt.netdev;
	struct mango_mgmt_mpipe_netdev_priv *priv;

	if (!ndev)
		return;
	priv = netdev_priv(ndev);
	wake_up(&priv->tx_wq);
}

static void mango_mgmt_mpipe_rx_irq_cb(struct mango_mgmt_dev *dev)
{
	struct net_device *ndev = dev->mgmt.netdev;
	struct mango_mgmt_mpipe_netdev_priv *priv;

	if (!ndev)
		return;
	priv = netdev_priv(ndev);
	wake_up(&priv->rx_wq);
}

static const struct mango_mgmt_mpipe_hw_config mango_mgmt_mpipe_netdev_hw_cfg = {
	.tx_callback = mango_mgmt_mpipe_tx_irq_cb,
	.rx_callback = mango_mgmt_mpipe_rx_irq_cb,
};

/* ===== ndo_open ===== */

static int mpipe_netdev_open(struct net_device *ndev)
{
	struct mango_mgmt_mpipe_netdev_priv *priv = netdev_priv(ndev);
	int node = dev_to_node(priv->mgmt_dev->dev);
	struct task_struct *tx_thread, *rx_thread;
	int devid = priv->mgmt_dev->mgmt.devid;
	int ret;

	BUILD_BUG_ON((1 << ENCODE_M_MAX) <=
		     DIV_ROUND_UP(MANGO_MGMT_MPIPE_NETDEV_MTU, 4));

	init_llist_head(&priv->tx_llist);
	init_llist_head(&priv->rx_llist);
	priv->rx_pending = NULL;
	priv->tx_avail = 0;
	priv->rx_avail = 0;
	priv->rx_skb = NULL;
	priv->rx_frame_key = 0;
	priv->rx_payload_words = 0;
	priv->rx_draining = false;
	priv->rx_skb_pool_count = 0;

	/* Per-CPU NUMA-local bitsets for frame-key selection (sized for max MTU) */
	priv->encode_bitset_pcpu = __alloc_percpu_gfp(
		BITS_TO_LONGS(1UL << ENCODE_M_MAX) * sizeof(long),
		__alignof__(long), GFP_KERNEL);
	if (!priv->encode_bitset_pcpu)
		return -ENOMEM;

	/* RX-only NAPI (TX path is pure kthread → no TX NAPI).  The
	 * wrapper supplies NAPI_POLL_WEIGHT on kernels whose
	 * netif_napi_add() still takes the weight arg (dropped in 6.1,
	 * commit 58caed3dacb4, backported to RHEL 9). */
	mango_compat_netif_napi_add(ndev, &priv->rx_napi, mpipe_rx_napi_poll);
	napi_enable(&priv->rx_napi);

	tx_thread = kthread_create(mpipe_netdev_tx_thread, ndev,
				   "mango_mgmt_tx/%d", devid);
	if (IS_ERR(tx_thread)) {
		ret = PTR_ERR(tx_thread);
		goto err_tx_thread;
	}
	rx_thread = kthread_create(mpipe_netdev_rx_thread, ndev,
				   "mango_mgmt_rx/%d", devid);
	if (IS_ERR(rx_thread)) {
		ret = PTR_ERR(rx_thread);
		kthread_stop(tx_thread);
		goto err_tx_thread;
	}
	priv->tx_thread = tx_thread;
	priv->rx_thread = rx_thread;

	/* Pin to device-local NUMA CPUs.  kthread_bind ensures the two
	 * threads can run concurrently on different CPUs (cpumask_of_node
	 * alone permits the scheduler to place both on the same CPU). */
	if (node >= 0) {
		const struct cpumask *mask = cpumask_of_node(node);
		int cpu_tx = cpumask_first(mask);
		int cpu_rx = cpumask_next(cpu_tx, mask);

		if (cpu_rx >= nr_cpu_ids)
			cpu_rx = cpu_tx; /* single-CPU node — accept serial */
		kthread_bind(tx_thread, cpu_tx);
		kthread_bind(rx_thread, cpu_rx);
	}

	wake_up_process(tx_thread);
	wake_up_process(rx_thread);

	/* No pre-arming of the TX or RX IRQ.  Each kthread arms the
	 * watermark on demand when it's about to sleep in wait_tx_space /
	 * its own wait loop, and disarms on wake — this keeps the shared
	 * MMIO IRQ handler on its skip-when-disabled fast path during
	 * unrelated interrupts. */

	/* Optimistic carrier-up: the kernel won't let the *first* packet
	 * leave a linkdown route, so a "carrier comes up only after we
	 * see real traffic" model never bootstraps.  Declare the link up
	 * now; mpipe_wait_tx_space still flips it off on a real >2 s
	 * peer-dead stall, and TX/RX progress flips it back on. */
	netif_carrier_on(ndev);
	netif_start_queue(ndev);
	return 0;

err_tx_thread:
	napi_disable(&priv->rx_napi);
	netif_napi_del(&priv->rx_napi);
	free_percpu(priv->encode_bitset_pcpu);
	priv->encode_bitset_pcpu = NULL;
	return ret;
}

/* ===== ndo_stop — quiesce in data-flow order ===== */

static int mpipe_netdev_stop(struct net_device *ndev)
{
	struct mango_mgmt_mpipe_netdev_priv *priv = netdev_priv(ndev);
	struct llist_node *node;

	/* TX pipeline: xmit-softirq → tx_llist → tx-kthread → MMIO/HW → tx-IRQ */

	/* 1. tx-softirq: stop new producers */
	netif_tx_disable(ndev);

	/* 2. tx-kthread: stop the consumer; drain leftover skbs.
	 * Any in-flight mpipe_tx_emit_skb returns via err_shutdown when
	 * mpipe_wait_tx_space sees kthread_should_stop, completing BQL for
	 * that skb.  Skbs sitting in tx_llist (charged to BQL by xmit but
	 * never picked up) are drained below — must complete BQL credit. */
	if (priv->tx_thread) {
		kthread_stop(priv->tx_thread);
		priv->tx_thread = NULL;
	}
	{
		unsigned int bytes = 0;
		unsigned int pkts = 0;

		node = llist_del_all(&priv->tx_llist);
		while (node) {
			struct llist_node *next = node->next;
			struct sk_buff *skb = mpipe_skb_from_ll_node(node);

			bytes += mpipe_encoded_size(skb);
			pkts++;
			skb_orphan(skb);
			dev_kfree_skb_any(skb);
			node = next;
		}
		if (pkts)
			netdev_completed_queue(ndev, pkts, bytes);
	}

	/* 3. tx-irq: nothing left wants the "FIFO has space" wake-up */
	mango_mgmt_mpipe_hw_disable_tx_irq(priv->mgmt_dev);

	/* RX pipeline: rx-IRQ → MMIO/HW → rx-kthread → rx_llist → rx-NAPI */

	/* 4. rx-irq: stop the wake-up source */
	mango_mgmt_mpipe_hw_disable_rx_irq(priv->mgmt_dev);

	/* 5. rx-kthread: stop the producer of rx_llist */
	if (priv->rx_thread) {
		kthread_stop(priv->rx_thread);
		priv->rx_thread = NULL;
	}

	/* 6. rx-softirq (NAPI): stop the consumer; drain rx_pending + rx_llist.
	 * napi_disable waits for any in-flight poll, so after this only we
	 * touch rx_pending / rx_llist. */
	napi_disable(&priv->rx_napi);
	node = priv->rx_pending;
	priv->rx_pending = NULL;
	while (node) {
		struct llist_node *next = node->next;

		dev_kfree_skb_any(mpipe_skb_from_ll_node(node));
		node = next;
	}
	node = llist_del_all(&priv->rx_llist);
	while (node) {
		struct llist_node *next = node->next;

		dev_kfree_skb_any(mpipe_skb_from_ll_node(node));
		node = next;
	}
	netif_napi_del(&priv->rx_napi);

	/* RX skb pool + in-flight reassembly skb */
	while (priv->rx_skb_pool_count > 0)
		dev_kfree_skb(priv->rx_skb_pool[--priv->rx_skb_pool_count]);
	if (priv->rx_skb) {
		dev_kfree_skb(priv->rx_skb);
		priv->rx_skb = NULL;
	}
	priv->rx_draining = false;

	free_percpu(priv->encode_bitset_pcpu);
	priv->encode_bitset_pcpu = NULL;
	netdev_reset_queue(ndev);

	cancel_work_sync(&priv->nat_ip_change_work);
	return 0;
}

/* ===== ndo_tx_timeout ===== */

static void mpipe_netdev_tx_timeout(struct net_device *ndev,
				    unsigned int txqueue)
{
	struct mango_mgmt_mpipe_netdev_priv *priv = netdev_priv(ndev);

	netdev_warn(ndev, "TX timeout — kthread may be stalled\n");
	ndev->stats.tx_errors++;

	/* Kick the kthread in case it missed a wake-up */
	if (priv->tx_thread)
		wake_up(&priv->tx_wq);
}

/* ===== Stats ===== */

static void mpipe_netdev_get_stats64(struct net_device *ndev,
				     struct rtnl_link_stats64 *stats)
{
	stats->tx_packets = ndev->stats.tx_packets;
	stats->tx_bytes = ndev->stats.tx_bytes;
	stats->tx_dropped = ndev->stats.tx_dropped;
	stats->tx_errors = ndev->stats.tx_errors;
	stats->rx_packets = ndev->stats.rx_packets;
	stats->rx_bytes = ndev->stats.rx_bytes;
	stats->rx_dropped = ndev->stats.rx_dropped;
}

static const struct net_device_ops mpipe_netdev_ops = {
	.ndo_open = mpipe_netdev_open,
	.ndo_stop = mpipe_netdev_stop,
	.ndo_start_xmit = mpipe_netdev_xmit,
	.ndo_get_stats64 = mpipe_netdev_get_stats64,
	.ndo_tx_timeout = mpipe_netdev_tx_timeout,
};

/* ===== NAT IP-change work item ===== */

/*
 * The target is computed here rather than latched at schedule time.
 * The last event then wins when several race.  mango_mgmt_link_ready()
 * requires the netdev to be administratively up as well as addressed.
 */
static void mango_mgmt_mpipe_nat_ip_change_work_fn(struct work_struct *work)
{
	struct mango_mgmt_mpipe_netdev_priv *priv = container_of(
		work, struct mango_mgmt_mpipe_netdev_priv, nat_ip_change_work);
	struct mango_mgmt_mpipe_nat *nat = &priv->nat;
	struct net_device *ndev = mango_mgmt_mpipe_get_netdev(priv->mgmt_dev);
	__be32 soc_ext = MANGO_MGMT_IP_UNSET;

	if (ndev && mango_mgmt_link_ready(nat, ndev))
		soc_ext = nat_soc(atomic_long_read(&nat->external));

	if (nat->on_ip_change)
		nat->on_ip_change(nat->on_ip_change_ctx, soc_ext);
}

/* ===== Setup and Lifecycle ===== */

static void mpipe_netdev_setup(struct net_device *ndev)
{
	/* Point-to-point link, no Ethernet headers, no ARP */
	ndev->netdev_ops = &mpipe_netdev_ops;
	ndev->type = ARPHRD_NONE;
	ndev->flags = IFF_POINTOPOINT | IFF_NOARP;
	ndev->mtu = MANGO_MGMT_MPIPE_NETDEV_MTU;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = MANGO_MGMT_MPIPE_NETDEV_MTU;
	ndev->hard_header_len = 0;
	ndev->addr_len = ETH_ALEN;
	ndev->tx_queue_len = 1000;
	ndev->watchdog_timeo = 5 * HZ;
	ndev->needs_free_netdev = true;

	/* No checksum offload — NAT incremental fixup requires
	 * fully-computed checksums from the kernel. */
	ndev->features &= ~(NETIF_F_IP_CSUM | NETIF_F_HW_CSUM);
	ndev->hw_features &= ~(NETIF_F_IP_CSUM | NETIF_F_HW_CSUM);
}

static struct net_device *
mango_mgmt_mpipe_netdev_alloc(struct mango_mgmt_dev *mgmt_dev)
{
	struct net_device *ndev;
	struct mango_mgmt_mpipe_netdev_priv *priv;

	if (mango_mgmt_transport_is_soc_device(mgmt_dev)) {
		ndev = alloc_netdev(sizeof(struct mango_mgmt_mpipe_netdev_priv),
				    "mango_mgmt", NET_NAME_USER,
				    mpipe_netdev_setup);
	} else {
		char name[IFNAMSIZ];

		snprintf(name, sizeof(name), "mango_mgmt%d",
			 mgmt_dev->mgmt.devid);
		ndev = alloc_netdev(sizeof(struct mango_mgmt_mpipe_netdev_priv),
				    name, NET_NAME_USER, mpipe_netdev_setup);
	}
	if (!ndev)
		return NULL;

	priv = netdev_priv(ndev);
	priv->mgmt_dev = mgmt_dev;
	priv->is_soc = mango_mgmt_transport_is_soc_device(mgmt_dev);

	/* TX/RX waitqueues — init here (not in ndo_open) so the IRQ
	 * callbacks (tx_irq_cb / rx_irq_cb) can always wake_up safely.
	 * Under the mock, trigger_msi is independent of the driver's
	 * enable_{tx,rx}_irq state, so an MSI can land between
	 * register_netdev and the first ndo_open. */
	init_waitqueue_head(&priv->tx_wq);
	init_waitqueue_head(&priv->rx_wq);

	/* NAT defaults: SoC internal = 192.168.10.2, external = unset */
	atomic_set(&priv->nat.soc_internal_ip, (__force int)htonl(0xC0A80A02u));
	atomic_long_set(&priv->nat.external, MANGO_MGMT_IP_PAIR_UNSET);
	init_waitqueue_head(&priv->nat.ready_wq);
	INIT_WORK(&priv->nat_ip_change_work,
		  mango_mgmt_mpipe_nat_ip_change_work_fn);

	/* Hardware address: binary-encoded transport address.  Zero-pad to
	 * MAX_ADDR_LEN first — dev_addr is kmalloc'd (not zeroed), and
	 * ifconfig displays the full buffer. */
	if (mgmt_dev->transport->get_mgmt_addr_bin) {
		u8 hwaddr[MAX_ADDR_LEN] = {};
		int len;
		const u8 *bin =
			mgmt_dev->transport->get_mgmt_addr_bin(mgmt_dev, &len);

		memcpy(hwaddr, bin, min_t(int, len, MAX_ADDR_LEN));
		ndev->addr_len = MAX_ADDR_LEN;
		dev_addr_set(ndev, hwaddr);
		ndev->addr_len = len;
	} else {
		ndev->addr_len = 0;
	}

	SET_NETDEV_DEV(ndev, mgmt_dev->dev);

	return ndev;
}

/* ===== inetaddr notifier: auto-derive SoC external IP ===== */

static bool mango_mgmt_mpipe_is_our_netdev(struct net_device *ndev)
{
	return ndev->netdev_ops == &mpipe_netdev_ops;
}

/*
 * Validate that `peer` is a usable peer address for `host` on subnet
 * mask/prefixlen: in-subnet, not the network or broadcast address
 * (the latter two don't exist for /31 and /32).
 */
static bool mango_mgmt_mpipe_peer_ip_ok(__be32 host, __be32 peer, __be32 mask,
					int prefixlen)
{
	if ((peer & mask) != (host & mask))
		return false; /* out of subnet */
	if (prefixlen < 31) {
		if ((peer & ~mask) == 0)
			return false; /* network address */
		if ((peer & ~mask) == ~mask)
			return false; /* broadcast address */
	}
	return true;
}

/*
 * Peer-IP auto-derivation: try +1, then -1.
 * Returns 0 on success, -EINVAL if no valid peer found.
 */
static int mango_mgmt_mpipe_derive_peer_ip(__be32 host, __be32 mask,
					   int prefixlen, __be32 *peer_out)
{
	__be32 candidates[] = {
		htonl(ntohl(host) + 1),
		htonl(ntohl(host) - 1),
	};
	int i;

	for (i = 0; i < 2; i++) {
		__be32 peer = candidates[i];

		if (!mango_mgmt_mpipe_peer_ip_ok(host, peer, mask, prefixlen))
			continue;
		*peer_out = peer;
		return 0;
	}
	return -EINVAL;
}

/*
 * sysfs setter for the SoC's external IP.  Validates that `new_soc_ext`
 * is a usable peer address on the same subnet as the current host_ip
 * (in-subnet, not network/broadcast, not equal to host_ip).  On
 * success: republishes nat.external, wakes link_ready waiters, and
 * re-fires the RPC reconnect work.
 *
 * Returns 0 on success, or:
 *   -EOPNOTSUPP — called on the SoC side
 *   -ENODEV     — netdev not yet registered
 *   -ENXIO      — host hasn't been assigned an IP yet
 *   -EINVAL     — new_soc_ext fails validation
 *   -EAGAIN     — nat.external was concurrently updated; userspace retry
 */
int mango_mgmt_mpipe_nat_set_soc_ip(struct mango_mgmt_dev *dev,
				    __be32 new_soc_ext)
{
	struct net_device *ndev = mango_mgmt_mpipe_get_netdev(dev);
	struct mango_mgmt_mpipe_netdev_priv *priv;
	struct in_device *in_dev;
	const struct in_ifaddr *ifa;
	__be32 host_ip, mask = 0;
	long ext, new_ext;
	int prefixlen;

	if (!ndev)
		return -ENODEV;
	priv = netdev_priv(ndev);
	if (priv->is_soc)
		return -EOPNOTSUPP;

	ext = atomic_long_read(&priv->nat.external);
	if (ext == MANGO_MGMT_IP_PAIR_UNSET)
		return -ENXIO;
	host_ip = nat_host(ext);

	/* No-op fast path: requested matches current, so the existing
	 * pairing has already passed validation — skip the in_dev lookup
	 * and the cmpxchg.  Also short-circuits any -EINVAL / -ENXIO that
	 * a transient inetaddr-side change might otherwise raise on a
	 * write that wouldn't actually change anything. */
	if (nat_soc(ext) == new_soc_ext)
		return 0;

	/* The fundamental constraint is host_ip != soc_internal — that
	 * one is enforced upstream by mango_mgmt_mpipe_inetaddr_validator
	 * (and can only be lifted by remotely retargeting the SoC's
	 * hardcoded internal IP, which we can't do today).  Here, we only
	 * need to reject soc_ext == host_ip (would cause self-NAT);
	 * soc_ext == soc_internal is harmless — NAT rewrites collapse to
	 * no-ops. */
	if (new_soc_ext == host_ip)
		return -EINVAL;

	rcu_read_lock();
	in_dev = __in_dev_get_rcu(ndev);
	if (in_dev) {
		in_dev_for_each_ifa_rcu (ifa, in_dev) {
			if (ifa->ifa_local == host_ip) {
				mask = ifa->ifa_mask;
				break;
			}
		}
	}
	rcu_read_unlock();
	if (!mask)
		return -ENXIO;

	prefixlen = inet_mask_len(mask);
	if (!mango_mgmt_mpipe_peer_ip_ok(host_ip, new_soc_ext, mask, prefixlen))
		return -EINVAL;

	new_ext = nat_pack(host_ip, new_soc_ext);

	/* Commit only if nothing else has touched nat.external since we
	 * read host_ip and validated against its subnet — a concurrent
	 * inetaddr event (`ip addr add/del`) would otherwise let us write
	 * a stale host_ip alongside the new soc_ext.  Tiny race, but
	 * cheap to close. */
	if (atomic_long_cmpxchg(&priv->nat.external, ext, new_ext) != ext)
		return -EAGAIN;

	wake_up_all(&priv->nat.ready_wq);

	schedule_work(&priv->nat_ip_change_work);

	dev_info(priv->mgmt_dev->dev,
		 "mpipe NAT: soc_ext overridden to %pI4/%d via sysfs\n",
		 &new_soc_ext, prefixlen);
	return 0;
}

static int mango_mgmt_mpipe_inetaddr_validator(struct notifier_block *nb,
					       unsigned long event, void *ptr)
{
	struct in_validator_info *ivi = ptr;
	struct net_device *ndev = ivi->ivi_dev->dev;
	struct mango_mgmt_mpipe_netdev_priv *priv;
	__be32 host_ip, soc_int;

	if (!mango_mgmt_mpipe_is_our_netdev(ndev))
		return NOTIFY_DONE;

	priv = netdev_priv(ndev);

	if (priv->is_soc)
		return NOTIFY_DONE;

	host_ip = ivi->ivi_addr;

	soc_int = (__force __be32)atomic_read(&priv->nat.soc_internal_ip);
	if (host_ip == soc_int) {
		/* Host can't take the SoC's hardcoded internal IP: TX NAT
		 * would rewrite our packets' dst to ourselves, and RX NAT
		 * would translate incoming src to host_ip too — both
		 * broken.  Lifting this requires a remote-controlled SoC
		 * internal-IP change, which we don't have. */
		dev_warn(
			priv->mgmt_dev->dev,
			"mpipe: %pI4 rejected — collides with SoC internal IP (%pI4)\n",
			&host_ip, &soc_int);
		return notifier_from_errno(-EINVAL);
	}

	return NOTIFY_DONE;
}

static int mango_mgmt_mpipe_inetaddr_event(struct notifier_block *nb,
					   unsigned long event, void *ptr)
{
	struct in_ifaddr *ifa = ptr;
	struct net_device *ndev = ifa->ifa_dev->dev;
	struct mango_mgmt_mpipe_netdev_priv *priv;
	struct mango_mgmt_mpipe_nat *nat;

	if (!mango_mgmt_mpipe_is_our_netdev(ndev))
		return NOTIFY_DONE;

	priv = netdev_priv(ndev);

	if (priv->is_soc)
		return NOTIFY_DONE;

	nat = &priv->nat;

	if (event == NETDEV_UP) {
		__be32 host_ip = ifa->ifa_local;
		__be32 mask = ifa->ifa_mask;
		int prefixlen = inet_mask_len(mask);
		__be32 peer;

		if (mango_mgmt_mpipe_derive_peer_ip(host_ip, mask, prefixlen,
						    &peer) == 0) {
			long old_ext = atomic_long_read(&nat->external);
			long new_ext = nat_pack(host_ip, peer);

			atomic_long_set(&nat->external, new_ext);
			wake_up_all(&nat->ready_wq);
			dev_info(priv->mgmt_dev->dev,
				 "mpipe NAT: host=%pI4 soc_ext=%pI4/%d\n",
				 &host_ip, &peer, prefixlen);

			if (old_ext != new_ext)
				schedule_work(&priv->nat_ip_change_work);
		}
	} else if (event == NETDEV_DOWN) {
		atomic_long_set(&nat->external, MANGO_MGMT_IP_PAIR_UNSET);
		wake_up_all(&nat->ready_wq);
		dev_info(priv->mgmt_dev->dev, "mpipe NAT: IP removed\n");

		schedule_work(&priv->nat_ip_change_work);
	}

	return NOTIFY_DONE;
}

/* ===== netdevice notifier =====
 *
 * The inetaddr chain fires only on address add and remove.  An admin
 * down keeps the address and leaves nat.external set.  Folding link
 * changes into the same work item re-evaluates the target on both.
 */
static int mango_mgmt_mpipe_netdev_event(struct notifier_block *nb,
					 unsigned long event, void *ptr)
{
	struct net_device *ndev = netdev_notifier_info_to_dev(ptr);
	struct mango_mgmt_mpipe_netdev_priv *priv;

	if (!mango_mgmt_mpipe_is_our_netdev(ndev))
		return NOTIFY_DONE;

	priv = netdev_priv(ndev);

	if (priv->is_soc)
		return NOTIFY_DONE;

	if (event == NETDEV_UP || event == NETDEV_DOWN)
		schedule_work(&priv->nat_ip_change_work);

	return NOTIFY_DONE;
}

static struct notifier_block mango_mgmt_mpipe_inetaddr_validator_nb = {
	.notifier_call = mango_mgmt_mpipe_inetaddr_validator,
};

static struct notifier_block mango_mgmt_mpipe_netdev_nb = {
	.notifier_call = mango_mgmt_mpipe_netdev_event,
};

static struct notifier_block mango_mgmt_mpipe_inetaddr_nb = {
	.notifier_call = mango_mgmt_mpipe_inetaddr_event,
};

int mango_mgmt_mpipe_netdev_init_notifiers(void)
{
	int ret;

	ret = register_inetaddr_validator_notifier(
		&mango_mgmt_mpipe_inetaddr_validator_nb);
	if (ret)
		return ret;

	ret = register_inetaddr_notifier(&mango_mgmt_mpipe_inetaddr_nb);
	if (ret) {
		unregister_inetaddr_validator_notifier(
			&mango_mgmt_mpipe_inetaddr_validator_nb);
		return ret;
	}

	ret = register_netdevice_notifier(&mango_mgmt_mpipe_netdev_nb);
	if (ret) {
		unregister_inetaddr_notifier(&mango_mgmt_mpipe_inetaddr_nb);
		unregister_inetaddr_validator_notifier(
			&mango_mgmt_mpipe_inetaddr_validator_nb);
		return ret;
	}

	return 0;
}

void mango_mgmt_mpipe_netdev_exit_notifiers(void)
{
	unregister_netdevice_notifier(&mango_mgmt_mpipe_netdev_nb);
	unregister_inetaddr_notifier(&mango_mgmt_mpipe_inetaddr_nb);
	unregister_inetaddr_validator_notifier(
		&mango_mgmt_mpipe_inetaddr_validator_nb);
}

/* ===== MPIPE subsystem lifecycle =====
 * HW FIFO layer, its IRQ sources, then the virtual netdev; TCP/IP on top is
 * the only path the rest of the driver uses for Host<->SoC bytes. */

/* The IRQ sources (vectors, legacy line or the polling kthread) read the
 * HW context: start them after it exists, stop them before it goes.
 * free_irq() and kthread_stop() wait for a running handler. */
static int mpipe_irq_start(struct mango_mgmt_dev *dev)
{
	/* The transport degrades to polling rather than failing, so anything
	 * but a positive count leaves this side to drive the FIFO. */
	if (dev->transport->irq_setup(dev) > 0)
		return 0;

	return mango_mgmt_mpipe_hw_poll_start(dev);
}

static void mpipe_irq_stop(struct mango_mgmt_dev *dev)
{
	mango_mgmt_mpipe_hw_poll_stop(dev);
	dev->transport->irq_teardown(dev);
}

int mango_mgmt_mpipe_init(struct mango_mgmt_dev *dev)
{
	struct net_device *ndev;
	int ret;

	ret = mango_mgmt_mpipe_hw_init(dev, &mango_mgmt_mpipe_netdev_hw_cfg);
	if (ret < 0)
		return ret;

	ret = mpipe_irq_start(dev);
	if (ret)
		goto err_irq;

	ndev = mango_mgmt_mpipe_netdev_alloc(dev);
	if (!ndev) {
		dev_err(dev->dev, "MPIPE: failed to allocate netdev\n");
		ret = -ENOMEM;
		goto err_irq;
	}

	ret = register_netdev(ndev);
	if (ret) {
		dev_err(dev->dev, "MPIPE: failed to register netdev: %d\n",
			ret);
		free_netdev(ndev);
		goto err_irq;
	}

	dev->mgmt.netdev = ndev;

	dev_info(dev->dev, "MPIPE: netdev %s registered (down, no IP)\n",
		 ndev->name);
	return 0;

err_irq:
	mpipe_irq_stop(dev);
	mango_mgmt_mpipe_hw_exit(dev);
	return ret;
}

void mango_mgmt_mpipe_exit(struct mango_mgmt_dev *dev)
{
	struct net_device *ndev = dev->mgmt.netdev;

	if (!ndev)
		return;

	/* No wake-up reaches the netdev from here on; ndo_stop parks the
	 * kthreads on its own. */
	mpipe_irq_stop(dev);
	unregister_netdev(ndev);
	mango_mgmt_mpipe_hw_exit(dev);

	dev->mgmt.netdev = NULL;
}

void mango_mgmt_mpipe_quiesce(struct mango_mgmt_dev *dev)
{
	struct net_device *ndev = dev->mgmt.netdev;

	if (!dev->mgmt.hw)
		return;

	if (ndev)
		netif_device_detach(ndev);
	mpipe_irq_stop(dev);
}

int mango_mgmt_mpipe_resume(struct mango_mgmt_dev *dev)
{
	struct net_device *ndev = dev->mgmt.netdev;
	int ret;

	if (!dev->mgmt.hw)
		return 0;

	ret = mpipe_irq_start(dev);
	if (ret)
		return ret;
	if (ndev)
		netif_device_attach(ndev);
	return 0;
}

struct net_device *mango_mgmt_mpipe_get_netdev(struct mango_mgmt_dev *dev)
{
	return dev->mgmt.netdev;
}

struct mango_mgmt_mpipe_nat *
mango_mgmt_mpipe_get_nat(struct mango_mgmt_dev *dev)
{
	struct net_device *ndev = dev->mgmt.netdev;
	struct mango_mgmt_mpipe_netdev_priv *priv;

	if (!ndev)
		return NULL;
	priv = netdev_priv(ndev);
	return &priv->nat;
}
