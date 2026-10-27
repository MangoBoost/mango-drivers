// SPDX-License-Identifier: GPL-2.0-only
/*
 * smi_top.c - This file contains the main entry point for the SMI driver.
 *
 * Copyright (c) 2023 - 2025 Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/pci.h>

#include "backend/smi_smc_control.h"
#include "core/smi_alarm.h"
#include "core/smi_sensor.h"
#include "frontend/smi_frontend.h"

#include "smi_top.h"
#include "smi.h"

/* Global Variables */
bool smi_debug_enabled = false;

/* List of registered SMI devices for lookup (e.g. for create_map_str) */
static LIST_HEAD(smi_device_list);
/* Global device mutex, for fetching/deleting handles without already holding a
 * valid pointer. It cannot live inside pf_dev_struct: any pointer there may be
 * invalid by the time its lock is acquired. */
static DEFINE_MUTEX(pf_dev_lock);

/* Event Callback */
static void smc_event_cb(enum smc_event_id id, void *data)
{
	struct pf_dev_struct *pf_dev = data;

	switch (id) {
	case SMC_EVENT_ID_HEARTBEAT_EXPIRED:
		PR_ERR("SMC Heartbeat expired event received");
		break;

	case SMC_EVENT_ID_HEARTBEAT_VALIDATION:
		PR_ERR("SMC Heartbeat validation event received");
		break;

	/* The card is gone: the state closes the gate, the drain wakes those
	 * already waiting. Threads and the BAR are teardown's -- taking them
	 * here would have the heartbeat thread reap itself. */
	case SMC_EVENT_ID_CARD_RESTARTED:
		PR_ERR("SMC session ended (card gone), stopping submissions...");
		if (!pf_dev) {
			PR_ERR("SMC callback cannot recover! dev/pf_dev invalid");
		} else {
			WRITE_ONCE(pf_dev->state, PF_DEV_STATE_NO_SMC);
			smi_device_quiesce(pf_dev);
		}
		break;

	/* One poll tick. What the host makes of it -- an alarm sweep over the
	 * store -- is decided here, not by the worker that ticks. */
	case SMC_EVENT_ID_POLL:
		smi_alarm_evaluate(pf_dev);
		break;

	/* This event will only be raised once */
	case SMC_EVENT_ID_HEARTBEAT_FATAL:
		PR_ERR("SMC Heartbeat fatal event received, stopping GCQ...");
		if (!pf_dev) {
			PR_ERR("SMC Heartbeat callback cannot recover! dev/pf_dev invalid");
		} else {
			/* State before the disconnect NULLs the BAR: readers
			 * gate on state, so READY must still imply a BAR. */
			WRITE_ONCE(pf_dev->state, PF_DEV_STATE_NO_SMC);
			smi_smc_disconnect(pf_dev->smc_ctx);
		}
		break;

	default:
		break;
	}
}

int smi_pf_dev_locate(struct pf_dev_struct *pf_dev, uint16_t seconds)
{
	if (!pf_dev)
		return -EINVAL;
	if (!smi_pf_dev_ready(pf_dev) || !pf_dev->smc_ctx)
		return -ENODEV;

	return smi_smc_locate(pf_dev->smc_ctx, seconds);
}

void smi_device_quiesce(struct pf_dev_struct *pf_dev)
{
	if (pf_dev && pf_dev->smc_ctx)
		smi_smc_quiesce(pf_dev->smc_ctx);
}

/* Shut every service of the card down. Deletes no data: they stay unusable
 * until the device is removed and the bus rescanned. */
static void shutdown_pf_dev_services(struct pf_dev_struct *pf_dev)
{
	s64 quiesce_aborts;
	s64 shutdown_aborts;

	if (!pf_dev || (pf_dev->state == PF_DEV_STATE_SHUTDOWN))
		return;

	/* Publish SHUTDOWN before teardown as a fast policy gate for cyclic SMI
	 * threads; the transport's locked halt gate stays the correctness
	 * boundary. */
	WRITE_ONCE(pf_dev->state, PF_DEV_STATE_SHUTDOWN);

	/* Shutdown SMC (clears pf_dev->smc_ctx; devm reclaims it at detach). */
	if (pf_dev->smc_ctx)
		smi_smc_teardown(pf_dev);

	/* Removal disables get_pf_dev_entry() before reaching here, so the
	 * shutdown count is not observable through sysfs. Report it once
	 * teardown has stopped every SMC submitter. */
	shutdown_aborts =
		atomic64_read(&pf_dev->cmd_aborted[SMI_CMD_ABORT_SHUTDOWN]);
	quiesce_aborts =
		atomic64_read(&pf_dev->cmd_aborted[SMI_CMD_ABORT_QUIESCE]);
	if (quiesce_aborts || shutdown_aborts)
		SMI_DEV_INFO(
			pf_dev->dev,
			"SMC commands canceled during shutdown: pre-submit=%lld live=%lld",
			quiesce_aborts, shutdown_aborts);
}

/**
 * delete_pf_dev_data() - Delete a pf_dev struct and remove all services.
 * @pf_dev: Device data struct.
 *
 * Return: None.
 */
static void delete_pf_dev_data(struct pf_dev_struct *pf_dev)
{
	if (!pf_dev)
		return;
	mutex_lock(&pf_dev_lock);
	list_del(&pf_dev->node);
	mutex_unlock(&pf_dev_lock);

	shutdown_pf_dev_services(pf_dev);

	/* The node and the face's files before the records they read. */
	smi_frontend_unpresent(pf_dev);

	/* The session's records, not the bus device's: a re-attach must not
	 * inherit them. */
	delete_sensors(pf_dev);
}

/**
 * release_pf_dev_entry() - Free device data once the refcount reaches 0.
 * @ref: refcount struct of the device to be removed
 *
 * Deletes nothing itself: it tells `pcie_device_remove` the device is safe to
 * remove.
 *
 * Return: None
 */
static void release_pf_dev_entry(struct kref *ref)
{
	struct pf_dev_struct *pf_dev = NULL;

	if (!ref)
		return;

	pf_dev = container_of(ref, struct pf_dev_struct, refcount);

	/* No need to acquire pf_dev_lock here */
	up(&pf_dev->remove_sema);
}

/* Caller holds pf_dev_lock. A device is either the bus device the SMI device
 * sits on or the hwmon device registered for it. */
static struct pf_dev_struct *smi_pf_dev_of_locked(struct device *dev)
{
	struct pf_dev_struct *pos;

	list_for_each_entry (pos, &smi_device_list, node)
		if (pos->dev == dev || smi_frontend_hwmon(pos) == dev)
			return pos;

	return NULL;
}

struct pf_dev_struct *smi_pf_dev_of(struct device *dev)
{
	struct pf_dev_struct *pf_dev;

	mutex_lock(&pf_dev_lock);
	pf_dev = smi_pf_dev_of_locked(dev);
	mutex_unlock(&pf_dev_lock);
	return pf_dev;
}

/* Caller holds pf_dev_lock. */
static struct pf_dev_struct *smi_pf_dev_get_locked(struct pf_dev_struct *pf_dev)
{
	if (!pf_dev || !smp_load_acquire(&pf_dev->enabled) ||
	    !kref_get_unless_zero(&pf_dev->refcount))
		return NULL;
	return pf_dev;
}

struct pf_dev_struct *smi_pf_dev_get(struct pf_dev_struct *pf_dev)
{
	mutex_lock(&pf_dev_lock);
	pf_dev = smi_pf_dev_get_locked(pf_dev);
	mutex_unlock(&pf_dev_lock);
	return pf_dev;
}

/*
 * Get a pf_dev_struct pointer and increment the reference count. */
struct pf_dev_struct *get_pf_dev_entry(void *cache,
				       enum pf_dev_cache_type cache_type)
{
	struct pf_dev_struct *entry = NULL;

	if (!cache)
		return NULL;

	mutex_lock(&pf_dev_lock);

	switch (cache_type) {
	case PF_DEV_CACHE_DEV:
		entry = smi_pf_dev_of_locked((struct device *)cache);
		break;

	/* Only a card node reaches here: a node that is no card has its own
	 * file operations. */
	case PF_DEV_CACHE_INODE: {
		struct inode *inode = (struct inode *)cache;
		struct smi_cdev_struct *cdev = NULL;

		cdev = container_of(inode->i_cdev, struct smi_cdev_struct,
				    cdev);
		entry = container_of(cdev, struct pf_dev_struct, cdev);
		break;
	}

	default:
		break;
	}

	entry = smi_pf_dev_get_locked(entry);
	mutex_unlock(&pf_dev_lock);
	return entry;
}

/* Get the platform (non-PCI) SMI device and take a reference. On the SoC it
 * binds to the platform bus, where the host's domain/bus/slot lookup fails;
 * there is only one, so return the first enabled non-PCI entry. */
static struct pf_dev_struct *smi_get_platform_pf_dev(void)
{
	struct pf_dev_struct *entry = NULL, *pos;

	mutex_lock(&pf_dev_lock);
	list_for_each_entry (pos, &smi_device_list, node) {
		if (!pos || !pos->dev || dev_is_pci(pos->dev))
			continue;
		if (pos->enabled && kref_get_unless_zero(&pos->refcount)) {
			entry = pos;
			break;
		}
	}
	mutex_unlock(&pf_dev_lock);

	return entry;
}

/* The SMI registered on one PCI function, counted; NULL if none. */
static struct pf_dev_struct *
smi_pf_dev_of_pci(unsigned int dom, unsigned int bus, unsigned int devfn)
{
	struct pci_dev *pdev = pci_get_domain_bus_and_slot(dom, bus, devfn);
	struct pf_dev_struct *pf_dev =
		pdev ? get_pf_dev_entry(&pdev->dev, PF_DEV_CACHE_DEV) : NULL;

	pci_dev_put(pdev);
	return pf_dev;
}

struct pf_dev_struct *smi_pf_dev_of_addr(const char *hw_addr)
{
	unsigned int dom, bus, slot, func, fn;
	struct pf_dev_struct *pf_dev;

	if (!hw_addr)
		return NULL;
	if (!strncmp(hw_addr, "mmio:", 5))
		return smi_get_platform_pf_dev();
	if (strncmp(hw_addr, "pcie:", 5) ||
	    sscanf(hw_addr + 5, "%x:%x:%x.%x", &dom, &bus, &slot, &func) != 4 ||
	    bus > 0xff || slot > 0x1f || func > 7)
		return NULL;

	/* The named function first; the card's SMI may sit on a sibling. */
	pf_dev = smi_pf_dev_of_pci(dom, bus, PCI_DEVFN(slot, func));
	for (fn = 0; !pf_dev && fn < 8; fn++)
		if (fn != func)
			pf_dev = smi_pf_dev_of_pci(dom, bus,
						   PCI_DEVFN(slot, fn));

	return pf_dev;
}

/*
 * Decrement the refcount of a pf_dev_struct. */
void put_pf_dev_entry(struct pf_dev_struct *pf_dev)
{
	if (pf_dev)
		kref_put(&pf_dev->refcount, release_pf_dev_entry);
}

void smi_pf_dev_for_each(void (*fn)(struct pf_dev_struct *pf_dev, void *arg),
			 void *arg)
{
	struct pf_dev_struct *pf_dev;

	mutex_lock(&pf_dev_lock);
	list_for_each_entry (pf_dev, &smi_device_list, node)
		fn(pf_dev, arg);
	mutex_unlock(&pf_dev_lock);
}

/**
 * smi_debug_enabled_store() - Sysfs write callback for 'smi_debug_enabled'.
 * @drv: Driver that this attribute belongs to.
 * @buf: Input character buffer.
 * @count: Number of bytes in input buffer.
 *
 * Return: Number of bytes used from the buffer.
 */
static ssize_t smi_debug_enabled_store(struct device_driver *drv,
				       const char *buf, size_t count)
{
	int ret = 0;
	long val = 0;

	ret = kstrtol(buf, 10, &val);
	if (ret)
		return ret;

	smi_debug_enabled = (bool)val;

	return count;
}

/**
 * smi_debug_enabled_show() - Sysfs read callback for 'smi_debug_enabled'.
 * @drv: Driver that this attribute belongs to.
 * @buf: Output character buffer.
 *
 * Return: Number of bytes written to output buffer.
 */
static ssize_t smi_debug_enabled_show(struct device_driver *drv, char *buf)
{
	return sysfs_emit(buf, "%d\n", smi_debug_enabled);
}

static struct driver_attribute driver_attr_ami_debug_enabled = {
	.attr = { .name = "smi_debug_enabled", .mode = 0644 },
	.show = smi_debug_enabled_show,
	.store = smi_debug_enabled_store
};

/* Driver-wide attributes, today under /sys/bus/pci/drivers/mango_smi/ */
static struct attribute *smi_driver_attrs[] = {
	&driver_attr_ami_debug_enabled.attr, NULL
};

const struct attribute_group smi_driver_group = {
	.attrs = smi_driver_attrs,
};

/* Helper Functions for Initialization */

static struct pf_dev_struct *smi_alloc_pf_dev(struct device *dev)
{
	struct pf_dev_struct *pf_dev;

	pf_dev = devm_kzalloc(dev, sizeof(struct pf_dev_struct), GFP_KERNEL);
	if (!pf_dev) {
		PR_ERR("Failed to allocate kernel memory for pf_dev_struct");
		return ERR_PTR(-ENOMEM);
	}

	/* Not ready until init ends; the registry entry is published now. */
	pf_dev->state = PF_DEV_STATE_INIT;
	pf_dev->enabled = false;

	pf_dev->dev = dev;
	pf_dev->sensor_refresh = SENSOR_REFRESH_TIMEOUT_MS;

	snprintf(pf_dev->slot_name, sizeof(pf_dev->slot_name), "%s",
		 dev_name(dev));

	sema_init(&pf_dev->remove_sema, 0);
	mutex_init(&pf_dev->sensor_lock);
	mutex_init(&pf_dev->eeprom_lock);
	mutex_init(&pf_dev->flash_lock);
	kref_init(&pf_dev->refcount);

	/* Registered now so sysfs callbacks find it during probe; counted
	 * lookups stay closed until enabled is set at the end of init. */
	mutex_lock(&pf_dev_lock);
	list_add_tail(&pf_dev->node, &smi_device_list);
	mutex_unlock(&pf_dev_lock);

	return pf_dev;
}

static int smi_init_smc_services(struct pf_dev_struct *pf_dev,
				 struct smi_init_data *init_data,
				 int *empty_sdr_count)
{
	int ret;

	ret = smi_smc_setup(pf_dev, init_data, smc_event_cb);
	if (ret) {
		pf_dev->smc_ctx = NULL;
		/* A deferral is not a diagnosis -- the device may yet become
		 * complete -- so it goes back to the bus instead of into a
		 * state that says this card has no SMC. */
		if (ret == -EPROBE_DEFER)
			return ret;

		pf_dev->state = PF_DEV_STATE_NO_SMC;
		/* Partial success on purpose: registered, no SMC/sensors */
		return SUCCESS;
	}

	/* What the session settled on, read once so every later reader has it
	 * without holding the session. */
	smi_smc_facts(pf_dev->smc_ctx, &pf_dev->facts);

	/* Sensor discovery. The SoC sensor service is ready a few seconds after
	 * the ring attaches; gate discovery so the first GET_SDR does not race
	 * that window. Best-effort -- discovery runs regardless. */
	wait_sensor_service_ready(pf_dev);
	if (!discover_sensors(pf_dev, empty_sdr_count)) {
		/* Thresholds come from the store, so the monitors arm only
		 * once it holds what the card stated. */
		if (smi_alarm_init(pf_dev)) {
			SMI_DEV_ERR(pf_dev->dev, "no alarm monitors");
			pf_dev->state = PF_DEV_STATE_INIT_ERROR;
		}

		/* A card with no surface keeps its cdev and sysfs: that is
		 * where the reason is read. */
		ret = smi_frontend_bind(pf_dev, smi_uapi_wanted(pf_dev));
		if (ret) {
			SMI_DEV_ERR(pf_dev->dev,
				    "no uapi surface presented (%d)", ret);
			pf_dev->state = PF_DEV_STATE_INIT_ERROR;
		}
	} else {
		pf_dev->state = PF_DEV_STATE_INIT_ERROR;
	}

	smi_smc_discovery_done(pf_dev->smc_ctx);

	return SUCCESS;
}

void smi_device_remove(struct pf_dev_struct *pf_dev)
{
	struct device *dev;
	void *group;

	if (!pf_dev)
		return;

	dev = pf_dev->dev;
	/* Read out before the release frees the struct it lives in. */
	group = pf_dev->devres_group;

	mutex_lock(&pf_dev_lock);
	pf_dev->enabled = false;
	mutex_unlock(&pf_dev_lock);
	put_pf_dev_entry(pf_dev);

	smi_frontend_leave(pf_dev);

	/* Uninterruptible: the last reference signals this (release_pf_dev_entry
	 * -> up()), and freeing without it frees under a live holder. A signal to
	 * the removing task must not turn removal into a use-after-free. */
	down(&pf_dev->remove_sema);

	delete_pf_dev_data(pf_dev);
	/* Frees pf_dev itself and everything else the attach charged to the
	 * parent, so nothing of this session outlives it. */
	devres_release_group(dev, group);
}

struct pf_dev_struct *smi_device_init(struct device *dev,
				      struct smi_init_data *init_data)
{
	int ret = 0;
	int found;
	int empty_sdr_count = 0;
	struct pf_dev_struct *pf_dev = NULL;
	void *group;

	if (!dev || !init_data)
		return ERR_PTR(-EINVAL);

	/* One live attach per device: the group and the registry entry are
	 * both keyed by it. */
	if (smi_pf_dev_of(dev))
		return ERR_PTR(-EBUSY);

	/* The attach owns its devres, so a failed or removed SMI reclaims what
	 * it charged to the parent without waiting for the parent's unbind. */
	group = devres_open_group(dev, NULL, GFP_KERNEL);
	if (!group)
		return ERR_PTR(-ENOMEM);

	pf_dev = smi_alloc_pf_dev(dev);
	if (IS_ERR(pf_dev)) {
		devres_release_group(dev, group);
		return pf_dev;
	}
	pf_dev->devres_group = group;

	pf_dev->fence_ops = init_data->fence_ops;

	ret = smi_init_smc_services(pf_dev, init_data, &empty_sdr_count);
	if (ret)
		goto cleanup;

	/* The user side, bound or not: a card that bound nothing still gets a
	 * node that says why. */
	found = smi_frontend_present(pf_dev, smi_uapi_wanted(pf_dev));
	if (found < 0) {
		ret = found;
		goto cleanup;
	}

	/* Update final state: what discovery left, and what the face found of
	 * the facts it reads itself. */
	if (pf_dev->state == PF_DEV_STATE_INIT) {
		if (found == SMI_FACE_FOUND_BROKEN)
			pf_dev->state = PF_DEV_STATE_INIT_ERROR;
		else if (empty_sdr_count || (found == SMI_FACE_FOUND_MISSING))
			pf_dev->state = PF_DEV_STATE_MISSING_INFO;
		else
			pf_dev->state = PF_DEV_STATE_READY;
	}

	/* Release: pairs with the smp_load_acquire in smi_pf_dev_ready and
	 * smi_pf_dev_get_locked, so a CPU observing enabled==true also sees
	 * the prior writes to state and sensor_repos. */
	smp_store_release(&pf_dev->enabled, true);

	devres_close_group(dev, group);
	return pf_dev;

cleanup:
	/* Stop SMC services before the data their threads read is freed. Do not
	 * pre-set SHUTDOWN state: shutdown_pf_dev_services() would early-return
	 * and skip smi_smc_teardown(), orphaning the smc proxy thread. */
	shutdown_pf_dev_services(pf_dev);
	/* Free sensor_repos if discover_sensors ran partially, before the
	 * entry leaves the registry. */
	delete_sensors(pf_dev);
	/* hwmon sits in this attach's group, released below. */
	pf_dev->frontend.hwmon = NULL;
	mutex_lock(&pf_dev_lock);
	pf_dev->enabled = false;
	list_del(&pf_dev->node);
	mutex_unlock(&pf_dev_lock);
	devres_release_group(dev, group);
	return ERR_PTR(ret);
}
