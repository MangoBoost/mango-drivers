/* SPDX-License-Identifier: GPL-2.0 */
/*
 * smi_smc_internal.h - The session's own state, for the files that are it.
 *
 * Above the backend a session is a handle and smi_smc_control.h is the whole
 * of it; this body is shared only by the files that make one work.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_SMC_INTERNAL_H
#define SMI_SMC_INTERNAL_H

#include <linux/sched.h>
#include <linux/wait.h>

#include "smi_smc_control.h"

/**
 * struct smc_control_ctx - The session's own state, backend-private.
 * @dev: Generic device (e.g. &pci_dev->dev).
 * @cmd: Message-layer state for this lane.
 * @version: SMC/firmware version info.
 * @heartbeat_thread: Thread that generates heartbeat requests, NULL when none
 *		      is running.
 * @pf_dev: The SMI device this session serves; also @event_cb's data.
 * @event_cb: Callback invoked on SMC events (e.g. heartbeat fatal).
 * @logging_thread: Thread that handles SMC log output, NULL when none is
 *		    running.
 * @log_init_wq: Wait queue for log initialization.
 * @log_init_done: True when log subsystem is ready.
 */
struct smc_control_ctx {
	struct device *dev;
	struct smi_cmd *cmd;
	struct smc_version version;
	struct task_struct *heartbeat_thread;
	struct pf_dev_struct *pf_dev;
	smc_event_callback event_cb;
	struct task_struct *logging_thread;
	wait_queue_head_t log_init_wq;
	bool log_init_done;
};

#endif /* SMI_SMC_INTERNAL_H */
