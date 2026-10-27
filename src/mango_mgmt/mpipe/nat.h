/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MangoBoost Management Module - MPIPE NAT Types
 *
 * Minimal header for NAT struct and inline helpers.
 * Does NOT include <linux/netdevice.h> — safe to include from
 * files that also include libsmi headers (which conflict on dev_open).
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_MPIPE_NAT_H_
#define _MANGO_MGMT_MPIPE_NAT_H_

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/wait.h>

#define MANGO_MGMT_IP_UNSET ((__force __be32)0xFFFFFFFFu)
#define MANGO_MGMT_IP_PAIR_UNSET (~0L)

struct mango_mgmt_mpipe_nat {
	atomic_t soc_internal_ip; /* learned from RX src */
	atomic_long_t external; /* high32: host_ip, low32: soc_ext */
	wait_queue_head_t ready_wq; /* woken on external set/clear */

	/* Callback for RPC reconnection on IP change.
	 * Set by api.c during init.  Called from a work item context
	 * (safe to sleep).  new_soc_ext == UNSET means IP was removed. */
	void (*on_ip_change)(void *ctx, __be32 new_soc_ext);
	void *on_ip_change_ctx;
};

/* Pack/unpack the external pair for atomic read/write.
 * __force silences sparse about __be32 <-> native mixing. */
static inline long nat_pack(__be32 host, __be32 soc)
{
	return ((long)(__force u32)host << 32) | (__force u32)soc;
}

static inline __be32 nat_host(long pair)
{
	return (__force __be32)(u32)(pair >> 32);
}

static inline __be32 nat_soc(long pair)
{
	return (__force __be32)(u32)pair;
}

/*
 * NAT-ready check only — does not consult netif_running, so callers
 * without netdevice.h can use it.  For the combined netif_running +
 * nat_ready check, see mango_mgmt_link_ready in netdev.h.
 */
static inline bool mango_mgmt_nat_ready(struct mango_mgmt_mpipe_nat *nat)
{
	return atomic_long_read(&nat->external) != MANGO_MGMT_IP_PAIR_UNSET;
}

/* Accessor for callers that can't pull in netdev.h / netdevice.h — notably
 * api.c, which also includes libsmi headers that collide on dev_open(). */
struct mango_mgmt_dev;
struct mango_mgmt_mpipe_nat *
mango_mgmt_mpipe_get_nat(struct mango_mgmt_dev *dev);

#endif /* _MANGO_MGMT_MPIPE_NAT_H_ */
