/* SPDX-License-Identifier: GPL-2.0 */
/*
 * libcompat — driver-core class shims.
 *
 * Feature flags (libcompat/compat.mk):
 *   MANGO_COMPAT_CLASS_CREATE_NO_MODULE   class_create() lost its module
 *                                         arg (v6.4; RHEL 9 backport)
 *   MANGO_COMPAT_CLASS_DEVNODE_CONST_DEV  class::devnode takes
 *                                         const struct device * (v6.2;
 *                                         RHEL 9 backport) — use directly
 *                                         at the callback definition
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_COMPAT_DEVICE_CLASS_H_
#define _MANGO_COMPAT_DEVICE_CLASS_H_

#include <linux/device.h>
#include <linux/module.h>

#ifdef MANGO_COMPAT_CLASS_CREATE_NO_MODULE
#define mango_compat_class_create(name) class_create(name)
#else
#define mango_compat_class_create(name) class_create(THIS_MODULE, name)
#endif

#endif /* _MANGO_COMPAT_DEVICE_CLASS_H_ */
