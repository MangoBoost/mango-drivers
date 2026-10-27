// SPDX-License-Identifier: GPL-2.0-only
/*
 * legacy.c - The legacy surface's row, its face and its module-wide node.
 *
 * The face is worn by every card this surface pairs with, bound or not: the
 * node, the PCI kobject files and the per-card state the ioctls keep.
 *
 * Copyright (c) 2023 - 2025 Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h> /* sscanf() */
#include <linux/module.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

#include "legacy.h"
#include "legacy_card.h"
#include "legacy_hwmon.h"
#include "legacy_ioctl.h"
#include "legacy_prims.h"
#include "legacy_pub.h"
#include "legacy_qsfp.h"
#include "legacy_sysfs.h"
#include "legacy_utils.h"
#include "smi_node.h"
#include "smi_driver_version.h"
#include "smi_log.h"

#define LEGACY_NODE_MODE (0666)

/**
 * struct pf_dev_application - One user application registered with the driver.
 * @list: Linked list node.
 * @pid: PID struct of the application - safe to store, it is not reused.
 */
struct pf_dev_application {
	struct list_head list;
	struct pid *pid;
};

/* The one node that is no card, kept for the whole module at minor 0. */
static struct smi_cdev_struct legacy_global_node;

static int legacy_global_open(struct inode *inode, struct file *filp)
{
	return -ENODEV;
}

static const struct file_operations legacy_global_fops = {
	.owner = THIS_MODULE,
	.open = legacy_global_open,
};

static int legacy_module_init(void)
{
	int ret = smi_node_create(&legacy_global_node, NULL,
				  &legacy_global_fops, LEGACY_NODE_MODE);

	if (ret) {
		PR_ERR("Failed to register character device to the kernel");
		return ret;
	}
	/* Released tools open minor 0 as this node: a surface whose own node
	 * came first must not take it silently. */
	if (MINOR(legacy_global_node.cdev_num) != DEFAULT_CDEV_BASEMINOR) {
		PR_ERR("the global node did not get minor %u",
		       DEFAULT_CDEV_BASEMINOR);
		smi_node_destroy(&legacy_global_node);
		return -EBUSY;
	}
	return 0;
}

static void legacy_module_exit(void)
{
	PR_DBG("Delete cdev");
	smi_node_destroy(&legacy_global_node);
}

const struct smi_legacy_prims *smi_legacy_prims(struct pf_dev_struct *pf_dev)
{
	return &smi_legacy_v2_prims;
}

/* App Management */
int smi_legacy_app_add(struct pf_dev_struct *pf_dev, struct task_struct *task)
{
	struct smi_legacy_card *card;
	struct pf_dev_application *app = NULL, *pos = NULL;
	struct pid *pid = NULL;
	int ret = 0;

	if (!pf_dev || !task)
		return -EINVAL;
	card = smi_legacy_card(pf_dev);

	mutex_lock(&card->app_lock);
	pid = get_task_pid(task, PIDTYPE_PID);

	if (!pid) {
		ret = -EINVAL;
		goto done;
	}

	/* Do nothing if app already exists. */
	list_for_each_entry (pos, &card->apps, list) {
		if (pid == pos->pid)
			goto done;
	}

	app = devm_kzalloc(pf_dev->dev, sizeof(struct pf_dev_application),
			   GFP_KERNEL);

	if (!app) {
		ret = -ENOMEM;
		goto done;
	}

	app->pid = pid;
	INIT_LIST_HEAD(&app->list);
	list_add(&app->list, &card->apps);
	pid = NULL; /* the list holds it now */

done:
	put_pid(pid);
	mutex_unlock(&card->app_lock);
	return ret;
}

int smi_legacy_app_del(struct pf_dev_struct *pf_dev, struct task_struct *task)
{
	struct smi_legacy_card *card;
	struct pf_dev_application *pos = NULL, *next = NULL;
	struct pid *pid = NULL;

	if (!pf_dev || !task)
		return -EINVAL;
	card = smi_legacy_card(pf_dev);

	mutex_lock(&card->app_lock);
	pid = get_task_pid(task, PIDTYPE_PID);

	/* Do nothing if app is not in list. */
	list_for_each_entry_safe (pos, next, &card->apps, list) {
		if (pid == pos->pid) {
			put_pid(pos->pid);
			list_del(&pos->list);
			devm_kfree(pf_dev->dev, pos);
			break;
		}
	}

	put_pid(pid);
	mutex_unlock(&card->app_lock);
	return 0;
}

/* Signal every registered application; the signalled leave the list. */
static void legacy_leave(struct pf_dev_struct *pf_dev)
{
	struct smi_legacy_card *card = smi_legacy_card(pf_dev);
	struct pf_dev_application *pos = NULL, *next = NULL;

	mutex_lock(&card->app_lock);

	list_for_each_entry_safe (pos, next, &card->apps, list) {
		if (!kill_pid(pos->pid, SIGBUS, 0)) {
			put_pid(pos->pid);
			list_del(&pos->list);
			devm_kfree(pf_dev->dev, pos);
		}
	}

	mutex_unlock(&card->app_lock);
}

/* The legacy UAPI judges a card by its partition tables, so this face reads
 * them wherever a session runs. Only a card still in INIT is judged: one
 * whose tables did not read is INIT_ERROR, one with no record MISSING_INFO. */
static int legacy_found(struct pf_dev_struct *pf_dev)
{
	int ret;

	if (pf_dev->state == PF_DEV_STATE_NO_SMC)
		return SMI_FACE_FOUND_ALL;

	ret = smi_fpt_load(pf_dev, &smi_legacy_card(pf_dev)->fpt);
	if (!ret || (pf_dev->state != PF_DEV_STATE_INIT))
		return SMI_FACE_FOUND_ALL;
	if (ret == -ENOMSG)
		return SMI_FACE_FOUND_MISSING;

	SMI_DEV_ERR(pf_dev->dev, "partition table unreadable (%d)", ret);
	return SMI_FACE_FOUND_BROKEN;
}

static int legacy_present(struct pf_dev_struct *pf_dev)
{
	struct smi_legacy_card *card;
	int ret;

	card = devm_kzalloc(pf_dev->dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	mutex_init(&card->app_lock);
	INIT_LIST_HEAD(&card->apps);
	mutex_init(&card->fpt.lock);
	mutex_init(&card->fpt_lock);
	pf_dev->frontend.face_priv = card;

	/* The published cage plane, if this card has one. A card that seats no
	 * cage leaves the store empty and nothing below notices. */
	smi_qsfp_attach(pf_dev);
	smi_legacy_counters_load(pf_dev);

	ret = register_sysfs(pf_dev);
	if (ret) {
		smi_qsfp_detach(pf_dev);
		pf_dev->frontend.face_priv = NULL;
		devm_kfree(pf_dev->dev, card);
		return ret;
	}

	return legacy_found(pf_dev);
}

static void legacy_unpresent(struct pf_dev_struct *pf_dev)
{
	struct smi_legacy_card *card = smi_legacy_card(pf_dev);
	struct pf_dev_application *pos = NULL, *next = NULL;

	mutex_lock(&card->app_lock);
	list_for_each_entry_safe (pos, next, &card->apps, list) {
		put_pid(pos->pid);
		list_del(&pos->list);
		devm_kfree(pf_dev->dev, pos);
	}
	mutex_unlock(&card->app_lock);

	/* Remove sysfs before forgetting the plane it reads */
	remove_sysfs(pf_dev);
	smi_qsfp_detach(pf_dev);
}

/**
 * struct legacy_map - The device map being written.
 * @map: The entries so far.
 * @map_sz: Its size, terminator included.
 * @num_entries: How many.
 * @bus: The bus the asking driver serves.
 */
struct legacy_map {
	char *map;
	int map_sz;
	int num_entries;
	const struct bus_type *bus;
};

static void legacy_map_one(struct pf_dev_struct *pf_dev, void *arg)
{
	struct legacy_map *m = arg;
	char temp[BDF_STR_LEN + 11] = { 0 };
	struct device *hwmon;
	int hwmon_id;

	if (!pf_dev->enabled || (m->bus && pf_dev->dev->bus != m->bus))
		return;

	/* The number hwmon named the device with, -1 while none is bound. */
	hwmon = smi_frontend_hwmon(pf_dev);
	if (!hwmon || (sscanf(dev_name(hwmon), "hwmon%d", &hwmon_id) != 1))
		hwmon_id = -1;

	sprintf(temp, "%s %d %d\n",
		pf_dev->slot_name, // Generic Slot/UID
		MINOR(pf_dev->cdev.cdev_num), hwmon_id);

	strconcat(&m->map, temp, &m->map_sz);
	m->num_entries++;
}

/**
 * create_map_str() - Create a device mapping string.
 * @map_str: Output parameter for the string - must be NULL
 * @map_str_sz: Output parameter for the string size - must be 0
 * @bus_filter: The bus the asking driver serves.
 *
 * Maps BDF numbers to device files. The string:
 *      2
 *      c1:00.0 1 3
 *      21:00.0 2 4
 * says two devices are attached: c1:00.0 has /dev/ami1 and
 * /sys/class/hwmon3, 21:00.0 has /dev/ami2 and /sys/class/hwmon4.
 *
 * Return: 0 or negative error code
 */
static int create_map_str(char **map_str, int *map_str_sz,
			  const struct bus_type *bus_filter)
{
	struct legacy_map m = { .map_sz = 1, .bus = bus_filter };
	char temp[BDF_STR_LEN + 11] = { 0 };

	if (!map_str || !map_str_sz || (*map_str) || (*map_str_sz != 0)) {
		return -EINVAL;
	}

	m.map = kzalloc(m.map_sz, GFP_KERNEL);
	if (!m.map)
		return -ENOMEM;

	smi_pf_dev_for_each(legacy_map_one, &m);

	/* Add number of entries to top of map. */
	*map_str_sz = 1;
	*map_str = kzalloc(*map_str_sz, GFP_KERNEL);

	if (!(*map_str)) {
		kfree(m.map);
		return -ENOMEM;
	}

	sprintf(temp, "%d\n", m.num_entries);
	strconcat(map_str, temp, map_str_sz);
	strconcat(map_str, m.map, map_str_sz);
	kfree(m.map);
	return SUCCESS;
}

/**
 * devices_show() - Sysfs read callback for 'devices' attribute.
 * @drv: Driver that this attribute belongs to.
 * @buf: Output character buffer.
 *
 * Return: Number of bytes written to output buffer.
 */
static ssize_t devices_show(struct device_driver *drv, char *buf)
{
	int ret = 0;
	char *map_str = NULL;
	int map_str_sz = 0;

	ret = create_map_str(&map_str, &map_str_sz, drv->bus);
	if (ret)
		return ret;

	if (map_str_sz > PAGE_SIZE) {
		PR_WARN("Device map string too large for sysfs buffer");
		map_str_sz = PAGE_SIZE;
		map_str[PAGE_SIZE - 1] = '\0'; // Ensure termination
	}

	ret = sysfs_emit(buf, "%s", map_str);
	kfree(map_str);

	return ret;
}

/**
 * version_show() - Sysfs read callback for 'version' attribute.
 * @drv: Driver that this attribute belongs to.
 * @buf: Output character buffer.
 *
 * Return: Number of bytes written to output buffer.
 */
static ssize_t version_show(struct device_driver *drv, char *buf)
{
	return sysfs_emit(buf, "%s\n", GIT_TAG);
}

static struct driver_attribute driver_attr_devices = {
	.attr = { .name = "devices", .mode = 0444 },
	.show = devices_show
};

static struct driver_attribute driver_attr_version = {
	.attr = { .name = "version", .mode = 0444 },
	.show = version_show
};

static struct attribute *legacy_driver_attrs[] = { &driver_attr_version.attr,
						   &driver_attr_devices.attr,
						   NULL };

const struct attribute_group smi_legacy_driver_group = {
	.attrs = legacy_driver_attrs,
};

/* Every card a newer row does not take. */
static bool legacy_pairs(const struct pf_dev_struct *pf_dev)
{
	return true;
}

const struct smi_face_ops smi_legacy_face = {
	.fops = &smi_legacy_fops,
	.node_mode = LEGACY_NODE_MODE,
	.present = legacy_present,
	.leave = legacy_leave,
	.unpresent = legacy_unpresent,
};

const struct smi_frontend_ops smi_legacy_frontend_ops = {
	.id = SMI_UAPI_LEGACY,
	.name = "legacy",
	.pairs = legacy_pairs,
	.hwmon_register = smi_hwmon_v1_register,
	.chan_resolve = smi_hwmon_v1_chan,
	.face = &smi_legacy_face,
	.mod_init = legacy_module_init,
	.mod_exit = legacy_module_exit,
};
