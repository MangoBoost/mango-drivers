/* SPDX-License-Identifier: GPL-2.0 */
/*
 * libcompat — netdev shims.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_COMPAT_NETDEV_H_
#define _MANGO_COMPAT_NETDEV_H_

#include <linux/netdevice.h>

/*
 * netif_napi_add() lost its weight arg in v6.1 (RHEL 9 has the new
 * form; v5.19/v6.0 still take 4 args).
 */
static inline void
mango_compat_netif_napi_add(struct net_device *dev, struct napi_struct *napi,
			    int (*poll)(struct napi_struct *, int))
{
#ifdef MANGO_COMPAT_NETIF_NAPI_ADD_NO_WEIGHT
	netif_napi_add(dev, napi, poll);
#else
	netif_napi_add(dev, napi, poll, NAPI_POLL_WEIGHT);
#endif
}

#endif /* _MANGO_COMPAT_NETDEV_H_ */
