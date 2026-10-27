/* SPDX-License-Identifier: GPL-2.0 */
/*
 * libcompat — platform-device shims.
 *
 * Feature flags (libcompat/compat.mk):
 *   MANGO_COMPAT_PLATFORM_REMOVE_RETURNS_VOID  platform_driver::remove
 *                                              returns void (v6.11)
 *
 * Declare the callback as
 *     static MANGO_COMPAT_PLATFORM_REMOVE_RET foo_remove(...)
 * and end it (and every exit path) with
 *     return MANGO_COMPAT_PLATFORM_REMOVE_OK;
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_COMPAT_PLATFORM_DEVICE_H_
#define _MANGO_COMPAT_PLATFORM_DEVICE_H_

#include <linux/platform_device.h>

#ifdef MANGO_COMPAT_PLATFORM_REMOVE_RETURNS_VOID
#define MANGO_COMPAT_PLATFORM_REMOVE_RET void
#define MANGO_COMPAT_PLATFORM_REMOVE_OK
#else
#define MANGO_COMPAT_PLATFORM_REMOVE_RET int
#define MANGO_COMPAT_PLATFORM_REMOVE_OK 0
#endif

#endif /* _MANGO_COMPAT_PLATFORM_DEVICE_H_ */
