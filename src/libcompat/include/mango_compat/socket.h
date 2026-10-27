/* SPDX-License-Identifier: GPL-2.0 */
/*
 * libcompat — socket shims.
 *
 * Feature flags (libcompat/compat.mk):
 *   MANGO_COMPAT_KERNEL_BIND_TAKES_UNSIZED kernel_bind()/kernel_connect()
 *                                         take struct sockaddr_unsized
 *                                         (v7.0)
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_COMPAT_SOCKET_H_
#define _MANGO_COMPAT_SOCKET_H_

#include <linux/net.h>
#include <linux/tcp.h>
#include <net/sock.h>
#include <net/tcp.h>

/* Cast helper for kernel_bind()/kernel_connect() address arguments. */
#ifdef MANGO_COMPAT_KERNEL_BIND_TAKES_UNSIZED
#define MANGO_COMPAT_SOCKADDR(p) ((struct sockaddr_unsized *)(p))
#else
#define MANGO_COMPAT_SOCKADDR(p) ((struct sockaddr *)(p))
#endif

#endif /* _MANGO_COMPAT_SOCKET_H_ */
