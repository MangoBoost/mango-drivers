// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - Host-mode terminal chardev (/dev/mango-mgmtN)
 *
 * Provides a character device backed by a kernel TCP socket to the SoC
 * console (port 1022) via the generic socket_tty adapter.
 * Host-mode only — SoC mode uses virtio-console instead.
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */
#ifndef _MANGO_MGMT_TERM_CLIENT_H_
#define _MANGO_MGMT_TERM_CLIENT_H_

#include <linux/types.h>

struct mango_mgmt_dev; /* fwd */

/* Initialize terminal chardev — allocates socket_tty for SoC:1022.
 * Called by frontend_init_device() in host mode only. */
int mango_mgmt_term_client_init(struct mango_mgmt_dev *mgmt_dev);

/* Destroy terminal chardev — frees socket_tty and term context.
 * Called by frontend_cleanup_device(). */
void mango_mgmt_term_client_exit(struct mango_mgmt_dev *mgmt_dev);

#endif /* _MANGO_MGMT_TERM_CLIENT_H_ */
