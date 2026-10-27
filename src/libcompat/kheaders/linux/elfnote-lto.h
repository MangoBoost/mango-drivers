/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stub for kernel-headers packages whose modpost references this header
 * without shipping it (seen on some Ubuntu Jammy 5.15 builds). LTO is off on
 * those kernels, so BUILD_LTO_INFO expands to nothing. Used only when the real
 * header is missing (see libcompat/compat.mk).
 */
#ifndef __ELFNOTE_LTO_H
#define __ELFNOTE_LTO_H
#define BUILD_LTO_INFO
#endif
