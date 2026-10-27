// SPDX-License-Identifier: GPL-2.0-only
/*
 * smi_node.h - The SMI class and the character device nodes in it.
 *
 * Copyright (c) 2023 Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_NODE_H
#define SMI_NODE_H

#include <linux/cdev.h>
#include <linux/types.h>

#include "smi.h"

#define DEFAULT_CDEV_COUNT 1
#define DEFAULT_CLS_NAME SMI_NAME
#define DEFAULT_DEVICE_NAME SMI_NAME
#define DEFAULT_CDEV_BASEMINOR (unsigned)(0)

struct smi_cdev_struct;

/* The class every node belongs to, made with the module and removed with it. */
int smi_node_class_create(void);
void smi_node_class_destroy(void);

/**
 * smi_node_create() - Make one node at the lowest free minor.
 * @node: Filled here; @node->count says whether it was made.
 * @parent: Pins the parent's kobject under the cdev, or NULL.
 * @fops: What the node answers with.
 * @mode: The permission its /dev entry gets.
 *
 * Return: 0 or negative error code.
 */
int smi_node_create(struct smi_cdev_struct *node, struct device *parent,
		    const struct file_operations *fops, umode_t mode);

/* Remove a node smi_node_create() made; one it did not make is left alone. */
void smi_node_destroy(struct smi_cdev_struct *node);

#endif /* SMI_NODE_H */
