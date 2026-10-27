/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stub for kernel-headers packages whose modpost references this header
 * without shipping it (seen on some Ubuntu Jammy 5.15 builds). Those kernels
 * do not use the macros it would define, so empty is correct. Used only when
 * the real header is missing (see libcompat/compat.mk).
 */
#ifndef __LINUX_EXPORT_INTERNAL_H__
#define __LINUX_EXPORT_INTERNAL_H__
#endif
