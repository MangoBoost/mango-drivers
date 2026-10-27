/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Socket-backed chardev with terminal ioctl emulation.
 *
 * Inherits the kernel socket's own file_operations for read/write/poll,
 * overrides open (connect), release (close), and ioctl (terminal emulation
 * for minicom compatibility).
 *
 * Usage:
 *   1. Call mango_mgmt_socket_tty_init() once at module init.
 *   2. Set cdev.ops = mango_mgmt_socket_tty_fops().  Connect target
 *      is the SoC's external IP (read from the MPIPE NAT at open time)
 *      on MANGO_TERM_SOC_PORT.
 *   3. Call mango_mgmt_socket_tty_exit() at module exit.
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_SOCKET_TTY_H_
#define _MANGO_MGMT_SOCKET_TTY_H_

struct file_operations;

int mango_mgmt_socket_tty_init(void);
void mango_mgmt_socket_tty_exit(void);

const struct file_operations *mango_mgmt_socket_tty_fops(void);

#endif /* _MANGO_MGMT_SOCKET_TTY_H_ */
