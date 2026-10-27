/* SPDX-License-Identifier: GPL-2.0 */
/*
 * libcompat — devlink shims.
 *
 * Feature flags (libcompat/compat.mk):
 *   MANGO_COMPAT_DEVLINK_REGISTER_RETURNS_VOID v5.16+ register form
 *   MANGO_COMPAT_HAS_DEVLINK_DRIVER_NAME_PUT   removed in v6.2
 *   MANGO_COMPAT_DEVLINK_HEALTH_GRACE_IN_OPS   v6.18+ graceful_period in ops
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_COMPAT_DEVLINK_H_
#define _MANGO_COMPAT_DEVLINK_H_

#include <net/devlink.h>

/*
 * Register a devlink instance.  The v5.16+ form cannot fail; the wrapper
 * keeps the older int contract so callers check the result uniformly.
 */
static inline int mango_compat_devlink_register(struct devlink *devlink)
{
#ifdef MANGO_COMPAT_DEVLINK_REGISTER_RETURNS_VOID
	devlink_register(devlink);
	return 0;
#else
	return devlink_register(devlink);
#endif
}

/*
 * v6.18 dropped @graceful_period from devlink_health_reporter_create() and
 * reads ops->default_graceful_period instead. Callers keep passing the value here for older kernels and must also
 * set the ops member under MANGO_COMPAT_DEVLINK_HEALTH_GRACE_IN_OPS.
 */
static inline struct devlink_health_reporter *
mango_compat_devlink_health_reporter_create(
	struct devlink *devlink, const struct devlink_health_reporter_ops *ops,
	u64 graceful_period, void *priv)
{
#ifdef MANGO_COMPAT_DEVLINK_HEALTH_GRACE_IN_OPS
	(void)graceful_period;
	return devlink_health_reporter_create(devlink, ops, priv);
#else
	return devlink_health_reporter_create(devlink, ops, graceful_period,
					      priv);
#endif
}

/* The core fills the driver name itself from v6.2 (helper removed). */
static inline int
mango_compat_devlink_info_driver_name_put(struct devlink_info_req *req,
					  const char *name)
{
#ifdef MANGO_COMPAT_HAS_DEVLINK_DRIVER_NAME_PUT
	return devlink_info_driver_name_put(req, name);
#else
	return 0;
#endif
}

#endif /* _MANGO_COMPAT_DEVLINK_H_ */
