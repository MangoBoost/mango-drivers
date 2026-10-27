/* SPDX-License-Identifier: GPL-2.0 */
/*
 * libcompat -- sysfs attribute shims.
 *
 * Feature flags (libcompat/compat.mk):
 *   MANGO_COMPAT_BIN_ATTR_READ_CONST   bin_attribute::read takes
 *                                      const struct bin_attribute * (v6.13,
 *                                      the constification of the sysfs ops).
 *                                      Ubuntu jammy and RHEL 9 carry
 *                                      the mutable form.
 *
 * A binary attribute's read callback is declared with MANGO_COMPAT_BIN_ATTR_ARG
 * in place of its attribute parameter, so one definition builds on both, and
 * the body reaches its own state through container_of() either way.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_COMPAT_SYSFS_H_
#define _MANGO_COMPAT_SYSFS_H_

#include <linux/sysfs.h>

#ifdef MANGO_COMPAT_BIN_ATTR_READ_CONST
#define MANGO_COMPAT_BIN_ATTR_ARG const struct bin_attribute
#else
#define MANGO_COMPAT_BIN_ATTR_ARG struct bin_attribute
#endif

#endif /* _MANGO_COMPAT_SYSFS_H_ */
