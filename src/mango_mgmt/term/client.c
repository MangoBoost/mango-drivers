// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - Terminal Client (Host only)
 *
 * Wires the /dev/mango-mgmtN chardev to a TCP connection to the SoC
 * console port (1022) via the socket_tty fops-inheritance adapter.
 *
 * The SoC's IP is read from the NAT struct at open time — no
 * pre-allocated sockaddr, no per-device state.
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include "client.h"
#include "socket_tty.h"
#include "../cdev.h"
#include "../dev.h"

int mango_mgmt_term_client_init(struct mango_mgmt_dev *mgmt_dev)
{
	if (!mgmt_dev)
		return -EINVAL;

	if (!mgmt_dev->mgmt.cdev)
		return -ENODEV;

	mgmt_dev->mgmt.cdev->cdev.ops = mango_mgmt_socket_tty_fops();
	return 0;
}

void mango_mgmt_term_client_exit(struct mango_mgmt_dev *mgmt_dev)
{
	if (mgmt_dev && mgmt_dev->mgmt.cdev)
		mgmt_dev->mgmt.cdev->cdev.ops = NULL;
}
