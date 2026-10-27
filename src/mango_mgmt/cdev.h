/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MangoBoost Management Module — chardev defaults
 *
 * The /dev/mango-mgmt%d cdev has two file_operations paths in its
 * lifetime:
 *   1. Initial fops (cdev.c): installed at probe.  Provides open
 *      + mmap of the MPIPE register window.  Used unconditionally
 *      on the SoC and on the Host before term_client_init runs.
 *   2. sty_fops (term/socket_tty.c, Host only): overlays the cdev
 *      with a TCP-backed terminal that also exposes mmap.  Both
 *      paths share mango_mgmt_cdev_mmap_common().
 * Both resolve the management device through struct mango_mgmt_cdev
 * under SRCU; the node outlives the device it fronts.
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_CDEV_H_
#define _MANGO_MGMT_CDEV_H_

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/srcu.h>

struct mango_mgmt_dev;
struct vm_area_struct;

struct mango_mgmt_cdev {
	struct mango_mgmt_dev __rcu *mgmt;
	struct srcu_struct srcu;
	struct device dev;
	struct cdev cdev;
	dev_t devno;
	int devid;
};

static inline struct mango_mgmt_cdev *mango_mgmt_cdev_of(struct inode *inode)
{
	return container_of(inode->i_cdev, struct mango_mgmt_cdev, cdev);
}

struct mango_mgmt_dev *mango_mgmt_cdev_lock(struct mango_mgmt_cdev *cdev,
					    int *idx);
void mango_mgmt_cdev_unlock(struct mango_mgmt_cdev *cdev, int idx);

int mango_mgmt_cdev_mmap_common(struct mango_mgmt_dev *mgmt,
				struct vm_area_struct *vma);

extern const struct file_operations mango_mgmt_cdev_fops;

#endif /* _MANGO_MGMT_CDEV_H_ */
