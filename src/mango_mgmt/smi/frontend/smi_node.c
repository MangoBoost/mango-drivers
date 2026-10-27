// SPDX-License-Identifier: GPL-2.0-only
/*
 * smi_node.c - The SMI class and the character device nodes in it.
 *
 * A node is made with the file operations and permission of whoever asks for
 * it; what the node answers is theirs.
 *
 * Copyright (c) 2023 - 2025 Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/kernel.h>
#include <linux/module.h> /* THIS_MODULE */
#include <linux/string.h>

#include <mango_compat/device_class.h>

#include "smi_node.h"
#include "smi_top.h" /* struct smi_cdev_struct */

static struct class *smi_node_class;
static DEFINE_IDA(smi_node_index);
static int dev_major = 0; /* Set by the first region allocated. */

/* Each node carries its own permission, set by whoever made it. */
#ifdef MANGO_COMPAT_CLASS_DEVNODE_CONST_DEV
static char *devnode(const struct device *dev, umode_t *mode)
#else
static char *devnode(struct device *dev, umode_t *mode)
#endif
{
	const struct smi_cdev_struct *node = dev_get_drvdata(dev);

	if (mode && node)
		*mode = node->mode;

	return NULL;
}

int smi_node_class_create(void)
{
	smi_node_class = mango_compat_class_create(DEFAULT_CLS_NAME);
	if (IS_ERR(smi_node_class)) {
		int ret = PTR_ERR(smi_node_class);

		PR_ERR("Failed to create class %s. ret : %d", DEFAULT_CLS_NAME,
		       ret);
		smi_node_class = NULL;
		return ret;
	}
	smi_node_class->devnode = devnode;

	return 0;
}

void smi_node_class_destroy(void)
{
	if (smi_node_class)
		class_destroy(smi_node_class);
	smi_node_class = NULL;
}

int smi_node_create(struct smi_cdev_struct *node, struct device *parent,
		    const struct file_operations *fops, umode_t mode)
{
	unsigned baseminor;
	int ret = 0;
	int id;

	if (!node || !fops || !smi_node_class)
		return -EINVAL;

	id = ida_alloc(&smi_node_index, GFP_KERNEL);
	if (id < 0)
		return id;
	baseminor = id + DEFAULT_CDEV_BASEMINOR;

	/* Allocate chrdev region */
	if (dev_major) {
		node->cdev_num = MKDEV(dev_major, baseminor);
		ret = register_chrdev_region(node->cdev_num, DEFAULT_CDEV_COUNT,
					     (const char *)DEFAULT_DEVICE_NAME);
	} else {
		/* This is the first device. */
		ret = alloc_chrdev_region(&node->cdev_num, baseminor,
					  DEFAULT_CDEV_COUNT,
					  (const char *)DEFAULT_DEVICE_NAME);
		if (!ret)
			dev_major = MAJOR(node->cdev_num);
	}
	if (ret)
		goto free_id;

	/* Initialize the cdev structure */
	cdev_init(&node->cdev, fops);
	node->cdev.owner = THIS_MODULE;
	node->mode = mode;

	/*
	 * Setting the parent is necessary so that the kobject is referenced
	 * appropriately and the parent is not freed before the cdev.
	 */
	if (parent)
		cdev_set_parent(&node->cdev, &parent->kobj);

	/* Register cdev to the kernel */
	ret = cdev_add(&node->cdev, node->cdev_num, DEFAULT_CDEV_COUNT);
	if (ret) {
		PR_ERR("Failed to register cdev to the kernel, err_code : %d",
		       ret);
		goto unreg_cdev_reg;
	}

	/* Create device; devnode reads the permission off it. */
	snprintf(node->dev_name, DEV_NAME_SIZE, "%s%d", DEFAULT_DEVICE_NAME,
		 baseminor);
	node->device = device_create(smi_node_class, NULL, node->cdev_num, node,
				     node->dev_name);
	if (IS_ERR(node->device)) {
		ret = PTR_ERR(node->device);
		PR_ERR("Failed to create device %s. ret : %d", node->dev_name,
		       ret);
		goto del_cdev;
	}

	node->count = DEFAULT_CDEV_COUNT;
	return 0;

del_cdev:
	cdev_del(&node->cdev);
unreg_cdev_reg:
	unregister_chrdev_region(node->cdev_num, DEFAULT_CDEV_COUNT);
free_id:
	ida_free(&smi_node_index, id);
	return ret;
}

void smi_node_destroy(struct smi_cdev_struct *node)
{
	if (!node || !node->count)
		return;

	cdev_del(&node->cdev);
	device_destroy(smi_node_class, node->cdev_num);
	unregister_chrdev_region(node->cdev_num, node->count);
	ida_free(&smi_node_index,
		 MINOR(node->cdev_num) - DEFAULT_CDEV_BASEMINOR);
	node->count = 0;
}
