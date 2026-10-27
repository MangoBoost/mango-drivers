/* SPDX-License-Identifier: GPL-2.0 */
/*
 * legacy_ioctl.h - The legacy card node: the legacy UAPI's ioctls.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_LEGACY_IOCTL_H
#define SMI_LEGACY_IOCTL_H

#include <linux/fs.h>
#include <linux/types.h>

struct device;
struct smi_flash_op;
struct smi_fpt_copy;

/* The card node's file operations. */
extern const struct file_operations smi_legacy_fops;

/* The legacy UAPI's check of a copy against its tables, before the card sees
 * it: 0, or -EINVAL for a partition they do not hold or a destination too
 * small. */
int smi_legacy_copy_fits(struct device *dev, struct smi_fpt_copy *f,
			 const struct smi_flash_op *op);

/* Which handler serves @cmd on a PF in @state, or ERR_PTR(-ENOTTY), -EPERM or
 * -ENXIO; @admin is asked only of a verb that needs privilege. */
struct smi_ioctl_handler;
const struct smi_ioctl_handler *smi_ioctl_gate(unsigned int cmd, int state,
					       bool (*admin)(void));

#endif /* SMI_LEGACY_IOCTL_H */
