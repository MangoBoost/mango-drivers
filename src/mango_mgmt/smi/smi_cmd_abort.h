/* SPDX-License-Identifier: GPL-2.0 */
/*
 * smi_cmd_abort.h - The commands this driver drops by decision, and why.
 *
 * One table from errno to reason, which both the check and the per-card
 * counters read.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_CMD_ABORT_H
#define SMI_CMD_ABORT_H

#include <linux/errno.h>
#include <linux/types.h>

/**
 * enum smi_cmd_abort_reason - Why a command was dropped without running.
 * @SMI_CMD_ABORT_SIGNAL: -EINTR, a signal ended a wait: the region or a resend.
 * @SMI_CMD_ABORT_QUIESCE: -ECANCELED, transport halted before submission.
 * @SMI_CMD_ABORT_SHUTDOWN: -ESHUTDOWN, a teardown drain claimed the command.
 * @SMI_CMD_ABORT_NODEV: -ENODEV, refused because the device is SMC-less.
 * @SMI_CMD_ABORT_MAX: Number of reasons, and none: a real error.
 *
 * Indexes pf_dev_struct.cmd_aborted[].
 */
enum smi_cmd_abort_reason {
	SMI_CMD_ABORT_SIGNAL = 0,
	SMI_CMD_ABORT_QUIESCE,
	SMI_CMD_ABORT_SHUTDOWN,
	SMI_CMD_ABORT_NODEV,
	SMI_CMD_ABORT_MAX
};

/* The reason @ret, as smi_submit_command() returned it, names. */
static inline enum smi_cmd_abort_reason smi_cmd_abort_reason(int ret)
{
	switch (ret) {
	case -EINTR:
		return SMI_CMD_ABORT_SIGNAL;
	case -ECANCELED:
		return SMI_CMD_ABORT_QUIESCE;
	case -ESHUTDOWN:
		return SMI_CMD_ABORT_SHUTDOWN;
	case -ENODEV:
		return SMI_CMD_ABORT_NODEV;
	default:
		return SMI_CMD_ABORT_MAX;
	}
}

/**
 * smi_cmd_aborted() - Was a command dropped by decision rather than failure?
 * @ret: Return code from smi_submit_command().
 *
 * The cause is reported where it is decided, so callers keep the code and log
 * at debug; anything else is a real error.
 *
 * Return: True if @ret denotes a planned abort.
 */
static inline bool smi_cmd_aborted(int ret)
{
	return smi_cmd_abort_reason(ret) != SMI_CMD_ABORT_MAX;
}

/**
 * smi_cmd_aborted_after_submit() - Could the aborted command have reached SMC?
 * @ret: Return code from smi_submit_command().
 *
 * -EINTR, -ECANCELED and -ENODEV are decided before submission, so the device
 * is untouched. Only -ESHUTDOWN drains a live command, outcome unknown.
 *
 * Return: True if the command may have executed.
 */
static inline bool smi_cmd_aborted_after_submit(int ret)
{
	return ret == -ESHUTDOWN;
}

#endif /* SMI_CMD_ABORT_H */
