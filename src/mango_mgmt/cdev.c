// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module — chardev defaults
 *
 * Default file_operations for /dev/mango-mgmt%d.  Provides open +
 * mmap of the MPIPE register window so userspace tools can drive raw
 * FIFO traffic without going through the netdev framing layer.
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>

#include "cdev.h"
#include "dev.h"
#include "regmap.h"

int mango_mgmt_cdev_mmap_common(struct mango_mgmt_dev *mgmt,
				struct vm_area_struct *vma)
{
	phys_addr_t phys;
	size_t size, len, off;
	int ret;

	if (!mgmt)
		return -EOPNOTSUPP;

	ret = mango_mgmt_regmap_mpipe_phys(mgmt, &phys, &size);
	if (ret)
		return ret;

	/* Reject overflow before bounds-checking so a malicious huge
	 * vm_pgoff cannot wrap (off + len) into a small value. */
	len = vma->vm_end - vma->vm_start;
	off = (size_t)vma->vm_pgoff << PAGE_SHIFT;
	if (off + len < off || off + len > size)
		return -EINVAL;

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	return io_remap_pfn_range(vma, vma->vm_start,
				  (phys + off) >> PAGE_SHIFT, len,
				  vma->vm_page_prot);
}

struct mango_mgmt_dev *mango_mgmt_cdev_lock(struct mango_mgmt_cdev *cdev,
					    int *idx)
{
	*idx = srcu_read_lock(&cdev->srcu);
	return srcu_dereference(cdev->mgmt, &cdev->srcu);
}

void mango_mgmt_cdev_unlock(struct mango_mgmt_cdev *cdev, int idx)
{
	srcu_read_unlock(&cdev->srcu, idx);
}

static int mango_mgmt_cdev_open(struct inode *inode, struct file *filp)
{
	struct mango_mgmt_cdev *cdev = mango_mgmt_cdev_of(inode);
	int idx, ret = 0;

	if (!mango_mgmt_cdev_lock(cdev, &idx))
		ret = -ENODEV;
	mango_mgmt_cdev_unlock(cdev, idx);

	filp->private_data = NULL;
	return ret;
}

static int mango_mgmt_cdev_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct mango_mgmt_cdev *cdev = mango_mgmt_cdev_of(file_inode(filp));
	struct mango_mgmt_dev *mgmt;
	int idx, ret = -ENODEV;

	mgmt = mango_mgmt_cdev_lock(cdev, &idx);
	if (mgmt)
		ret = mango_mgmt_cdev_mmap_common(mgmt, vma);
	mango_mgmt_cdev_unlock(cdev, idx);

	return ret;
}

const struct file_operations mango_mgmt_cdev_fops = {
	.owner = THIS_MODULE,
	.open = mango_mgmt_cdev_open,
	.mmap = mango_mgmt_cdev_mmap,
	.llseek = noop_llseek,
};
