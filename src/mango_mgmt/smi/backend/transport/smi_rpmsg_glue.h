/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * smi_rpmsg_glue.h - Glue API for RPMsg driver to bind channel to FAL instance
 *
 * Copyright (c) 2026 MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_RPMSG_GLUE_H
#define SMI_RPMSG_GLUE_H

#include <linux/completion.h>

#include "fal/fw_if.h"

#ifndef CONFIG_OF
/* Forward declaration for non-OF builds (x86/RHEL).
 * linux/rpmsg.h is unavailable; pointer use only, no dereference. */
struct rpmsg_device;
#endif

extern struct completion rpmsg_ready_comp;

/* The RPMsg endpoint driver; registered by mango_mgmt_smi_init(). */
int smi_rpmsg_driver_init(void);
void smi_rpmsg_driver_exit(void);

/**
 * smi_rpmsg_bind_channel() - Bind an rpmsg_device into ONE FAL config.
 * @fal: the config to bind into -- the transport's own, never a global.
 * @rpdev: struct rpmsg_device * (void * to keep rpmsg.h out of every includer)
 *
 * Called by the rpmsg backend's create(), which is what makes "this transport
 * exists" mean "this transport can carry a frame". After this, FAL write uses
 * rpmsg_send.
 *
 * Return: 0, or -EINVAL when there is nothing to bind or nothing to bind into.
 */
int smi_rpmsg_bind_channel(FW_IF_CFG *fal, void *rpdev);

struct rpmsg_device *smi_rpmsg_get_current_rpdev(void);

/**
 * smi_rpmsg_unregister_channel() - Unbind rpmsg_device from the FAL instance
 *
 * Call from rpmsg_driver .remove.
 */
void smi_rpmsg_unregister_channel(void);

/**
 * smi_rpmsg_get_fal() - Get current RPMSG FAL instance (for RX callback)
 *
 * Returns the FW_IF_CFG* so that the rpmsg callback can call raiseEvent.
 * NULL if no FAL has been created yet.
 */
FW_IF_CFG *smi_rpmsg_get_fal(void);

/**
 * fw_if_rpmsg_rx_push() - Push RX payload into FAL queue (D1 Option A: read() path).
 * Call from rpmsg driver callback so that rpmsg_read() can return data.
 *
 * @data: payload from rpmsg callback
 * @len:  payload length
 */
void fw_if_rpmsg_rx_push(const uint8_t *data, uint32_t len);

#endif /* SMI_RPMSG_GLUE_H */
