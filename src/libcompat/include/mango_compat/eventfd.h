/* SPDX-License-Identifier: GPL-2.0 */
/*
 * libcompat — eventfd shims.
 *
 * Feature flags (libcompat/compat.mk):
 *   MANGO_COMPAT_EVENTFD_SIGNAL_NO_COUNTER  eventfd_signal() lost its
 *                                           counter arg (v6.8; RHEL 9.5)
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_COMPAT_EVENTFD_H_
#define _MANGO_COMPAT_EVENTFD_H_

#include <linux/eventfd.h>

/*
 * Signal an eventfd, incrementing its counter by exactly 1.
 *
 * There is deliberately NO counter argument: kernels v6.8+ (RHEL 9.5+)
 * dropped it and always increment by 1, so any increment-by-N semantics
 * cannot be provided portably.  Callers that historically passed byte
 * counts on old kernels get uniform increment-by-1 behavior everywhere —
 * which is what userspace already has to tolerate on current kernels.
 */
static inline void mango_compat_eventfd_signal(struct eventfd_ctx *ctx)
{
#ifdef MANGO_COMPAT_EVENTFD_SIGNAL_NO_COUNTER
	eventfd_signal(ctx);
#else
	eventfd_signal(ctx, 1);
#endif
}

#endif /* _MANGO_COMPAT_EVENTFD_H_ */
