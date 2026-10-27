// SPDX-License-Identifier: GPL-2.0-only
/*
 * smi_top.h - This file contains common SMI driver definitions.
 *
 * Copyright (c) 2023 - 2025 Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_TOP_H
#define SMI_TOP_H

#include <asm/barrier.h> /* smp_load_acquire() */
#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/cdev.h>
#include <linux/compiler.h> /* READ_ONCE() */
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/semaphore.h>

#include "contract/smi/smi_abi.h" /* struct smi_pub_key */

#include "backend/transport/fal/fw_if.h"
#include "smi.h" /* BDF_STR_LEN */
#include "smi_cmd_abort.h" /* SMI_CMD_ABORT_MAX */
#include "smi_facts.h"
#include "smi_resource.h"

struct attribute_group;
struct smc_control_ctx;
struct smi_alarm;
struct smi_face_ops;
struct smi_frontend_ops;
struct smi_repo;

#define DEV_NAME_SIZE 50

/**
 * struct smi_cdev_struct - One node of the SMI class.
 * @dev_name: Its name: SMI_NAME and its minor.
 * @count: Minor numbers it spans: 1 once made, 0 before and after.
 * @cdev_num: Its device number.
 * @cdev: The character device.
 * @device: The class device.
 * @mode: The permission devnode gives its /dev entry.
 */
struct smi_cdev_struct {
	char dev_name[DEV_NAME_SIZE];
	unsigned count;
	dev_t cdev_num;
	struct cdev cdev;
	struct device *device;
	umode_t mode;
};

/**
 * struct smi_frontend - What one card's UAPI surface is, once bound.
 * @ops: the surface. Written last and read as the proof there is one.
 * @priv: what its callbacks read, or NULL where they read none.
 * @hwmon: the device devm_hwmon_device_register_with_info() returned.
 * @face: the user side the card wears, set once at registration.
 * @face_priv: what the face keeps for this card; its @present fills it.
 *
 * Allocated with the card. The binding is filled once, by smi_frontend_bound();
 * the face before any reader can look it up.
 */
struct smi_frontend {
	const struct smi_frontend_ops *ops;
	void *priv;
	struct device *hwmon;
	const struct smi_face_ops *face;
	void *face_priv;
};

/**
 * enum pf_dev_state - List of possible device states.
 * @PF_DEV_STATE_INIT: Device is initialising.
 * @PF_DEV_STATE_READY: Device is fully initialised and ready to use.
 * @PF_DEV_STATE_MISSING_INFO: usable device, but some data is missing.
 * @PF_DEV_STATE_NO_SMC: Device may be used but has no SMC instance.
 * @PF_DEV_STATE_INIT_ERROR: SMC setup and device may be used but with no data.
 * @PF_DEV_STATE_SHUTDOWN: All services have been shutdown.
 *
 * PF_DEV_STATE_INIT lives only inside probe, so a user rarely sees it.
 */
enum pf_dev_state {
	PF_DEV_STATE_INIT = 0,
	PF_DEV_STATE_READY,
	PF_DEV_STATE_MISSING_INFO,
	PF_DEV_STATE_NO_SMC,
	PF_DEV_STATE_INIT_ERROR,
	PF_DEV_STATE_SHUTDOWN
};

/* The states a PF serves requests in, as bits of the state: READY, and
 * MISSING_INFO, whose board data is absent */
#define SMI_PF_SERVING_STATES                                                  \
	(BIT(PF_DEV_STATE_READY) | BIT(PF_DEV_STATE_MISSING_INFO))

/**
 * enum pf_dev_cache_type() - List of possible pf_dev_struct cache locations
 * @PF_DEV_CACHE_INODE: pf_dev is in the cdev struct inside an inode
 * @PF_DEV_CACHE_DEV: the bus device the SMI device sits on, or its hwmon
 *		      device; resolved through the device registry
 *
 * A cached pointer (a character device reaching pf_dev through the inode, say)
 * is not reference counted and must not be used directly: call
 * `get_pf_dev_entry` for a counted pointer, `put_pf_dev_entry` when done.
 */
enum pf_dev_cache_type {
	PF_DEV_CACHE_INODE,
	PF_DEV_CACHE_DEV,
};

/* One refresh's Sensors section and pairing: @slot names each record's cell,
 * kept while @key (shape, epoch, boot) holds; grown to the card's length. */
struct smi_pub_scratch {
	uint8_t *sect; /* the section, read whole under its seq */
	size_t sect_bytes;

	uint16_t *slot;
	size_t slot_bytes;
	uint16_t slot_cnt;
	bool paired;
	bool fmt_warned; /* a format this build cannot read, said once */
	struct {
		uint32_t boot;
		uint32_t epoch;
		uint16_t cnt;
		uint16_t stride;
		uint16_t map_off;
		uint16_t val_off;
	} key;
};

/* No cell carries this record's value. */
#define SMI_PUB_NO_SLOT ((uint16_t)0xFFFF)

/**
 * struct smi_reset_fence_ops - A glue's reset fence
 * @begin: Fence the unit under @dev against resets. Returns a token for
 *	   @end, NULL when @dev sits on no fenceable unit, or ERR_PTR.
 * @end: Drop the fence @begin returned. NULL is accepted and ignored.
 */
struct smi_reset_fence_ops {
	void *(*begin)(struct device *dev);
	void (*end)(void *fence);
};

/**
 * struct pf_dev_struct - Core device structure for SMI driver (Bus Agnostic).
 * @state: Current device state.
 * @smc_ctx: SMC control context (only valid for Master/PF0).
 * @fence_ops: The glue's reset fence, NULL when the glue has none.
 * @sensor_refresh: Sensor refresh rate in ms.
 * @num_sensor_repos: Count of sensor repositories.
 * @sensor_repos: Array of sensor repositories.
 * @sensor_lock: Guards @sensor_repos, @pub_scratch and their payloads; held by
 *		 the sensor layer alone, so no record or repo pointer leaves it.
 * @pub_scratch: One refresh's read of the Sensors section, grown to what the
 *		 card says it is.
 * @cdev: The card's node, made with its face's file operations.
 * @slot_name: Generic device identifier string (e.g., BDF or generic name).
 * @frontend: The UAPI surface this card presents, and the face it wears.
 * @facts: What the SMC session settled on. Taken once, so a reader needs
 *	   neither the session nor a lock.
 * @alarm: What the alarm sweep watches here. NULL until discovery has filled
 *	   the store; see smi_alarm_init().
 * @node: Node in the global device list.
 * @enabled: Device enabled flag. False during removal.
 * @refcount: Reference count for lifecycle management.
 * @remove_sema: Semaphore to wait for safe removal.
 * @devres_group: The devres group this attach charged to @dev. The group
 *		holds this struct, so remove reads it out before releasing.
 * @dev: Pointer to the generic struct device (parent).
 * @cmd_aborted: Per-reason counters of planned aborts (see smi_cmd_aborted()).
 *		 Device-lived, so they outlive SMC teardown.
 * @eeprom_lock: One EEPROM access at a time on this host, so a read-modify-write
 *		 is whole against every other reader and writer.
 * @flash_lock: One flash op at a time on this card: smi_flash_run().
 */
struct pf_dev_struct {
	enum pf_dev_state state;
	struct smc_control_ctx *smc_ctx; /* Only applicable for PF0 */
	const struct smi_reset_fence_ops *fence_ops;
	uint16_t sensor_refresh;
	uint8_t num_sensor_repos;
	struct smi_repo *sensor_repos;
	struct mutex sensor_lock;
	struct smi_pub_scratch pub_scratch;
	struct smi_cdev_struct
		cdev; /* Not a pointer so we can use `container_of` */
	char slot_name[BDF_STR_LEN];
	struct smi_frontend frontend;
	struct smi_smc_facts facts;
	struct smi_alarm *alarm;
	struct list_head node; /* Global device list node */
	bool enabled;
	struct kref refcount;
	struct semaphore remove_sema;
	struct device *dev; /* Generic Linux Device (e.g. &pci->dev) */
	void *devres_group; /* This attach's devres group, as devres minted it */
	/* Diagnostics */
	atomic64_t cmd_aborted[SMI_CMD_ABORT_MAX];
	struct mutex eeprom_lock;
	struct mutex flash_lock;
};

/* The glue's reset fence around a call a reset must not cut: ERR_PTR while a
 * reset runs, else the token smi_reset_unfence() drops (NULL: no fence). */
static inline void *smi_reset_fence(struct pf_dev_struct *pf_dev)
{
	return pf_dev->fence_ops ? pf_dev->fence_ops->begin(pf_dev->dev) : NULL;
}

static inline void smi_reset_unfence(struct pf_dev_struct *pf_dev, void *fence)
{
	if (pf_dev->fence_ops)
		pf_dev->fence_ops->end(fence);
}

/* The errno the card's state refuses a request with; 0 while the session
 * runs or with no card. Pairs with the fatal handler's WRITE_ONCE(). */
static inline int smi_pf_dev_refusal(const struct pf_dev_struct *pf_dev)
{
	switch (pf_dev ? READ_ONCE(pf_dev->state) : PF_DEV_STATE_READY) {
	case PF_DEV_STATE_NO_SMC:
		return -ENODEV;
	case PF_DEV_STATE_SHUTDOWN:
		return -ECANCELED;
	default:
		return 0;
	}
}

/* Whether the card may be asked for sensor or board data: probe published
 * @enabled, whose acquire pairs with that release, and the state serves. */
static inline bool smi_pf_dev_ready(struct pf_dev_struct *pf_dev)
{
	return pf_dev && smp_load_acquire(&pf_dev->enabled) &&
	       (BIT(READ_ONCE(pf_dev->state)) & SMI_PF_SERVING_STATES);
}

/**
 * struct smi_init_data - What a glue hands the core at bind
 * @msg_view: GCQ queue/payload windows the glue mapped; NULL for RPMsg
 * @transport_type: GCQ or RPMSG, selects the message-layer backend
 * @fence_ops: The glue's reset fence; NULL when the glue has none
 */

struct smi_init_data {
	struct smi_cmd_resource_view *msg_view;
	FW_IF_TYPE transport_type;
	const struct smi_reset_fence_ops *fence_ops;
};

/**
 * get_pf_dev_entry() - Request a reference counted pointer to a pf_dev_struct
 * @cache: Data cache in which to lookup the pointer.
 * @cache_type: Type of data cache.
 *
 * Return: Pointer to data struct or NULL if unavailable.
 */
struct pf_dev_struct *get_pf_dev_entry(void *cache,
				       enum pf_dev_cache_type cache_type);

/* The SMI serving a "pcie:D:B:S.F" (that function first, then its slot
 * siblings) or "mmio:..." address. Counted; release with put_pf_dev_entry(). */
struct pf_dev_struct *smi_pf_dev_of_addr(const char *hw_addr);

/* Counted reference on a device the caller already points at; NULL once it
 * is disabled or going away. Release with put_pf_dev_entry(). */
struct pf_dev_struct *smi_pf_dev_get(struct pf_dev_struct *pf_dev);

/* Call @fn on every registered device, in attach order, under the registry
 * lock: @fn must not look a device up. */
void smi_pf_dev_for_each(void (*fn)(struct pf_dev_struct *pf_dev, void *arg),
			 void *arg);

/**
 * put_pf_dev_entry() - Decrement the reference count of a pf_dev_struct
 * @pf_dev: Pointer to data struct.
 *
 * If this is the last pointer reference, all device data will be deleted.
 *
 * Return: None
 */
void put_pf_dev_entry(struct pf_dev_struct *pf_dev);

/**
 * smi_device_init() - Core initialization function called by Bus Glue.
 * @dev: Generic struct device pointer.
 * @init_data: Initialization parameters.
 *
 * Everything the attach charges to @dev goes in its own devres group; the
 * caller holds no token for it. smi_device_remove(), or a failure here,
 * releases it. One live attach per @dev: -EBUSY for a second.
 *
 * Return: The SMI device, or ERR_PTR on failure.
 */
struct pf_dev_struct *smi_device_init(struct device *dev,
				      struct smi_init_data *init_data);

/**
 * smi_device_remove() - Core cleanup function called by Bus Glue.
 * @pf_dev: The device smi_device_init() returned. Freed on return.
 */
void smi_device_remove(struct pf_dev_struct *pf_dev);

/* The registered SMI device on @dev (bus or hwmon device), no reference
 * taken; for callbacks that run while the device is being set up. */
struct pf_dev_struct *smi_pf_dev_of(struct device *dev);

/* The module's own driver attribute: the debug switch. */
extern const struct attribute_group smi_driver_group;

/* Blink the card's locate beacon for @seconds, 0 turning it off. */
int smi_pf_dev_locate(struct pf_dev_struct *pf_dev, uint16_t seconds);

/* Stop submissions to a card that may be gone; the session stays for
 * teardown. NULL-safe. */
void smi_device_quiesce(struct pf_dev_struct *pf_dev);

#endif /* SMI_TOP_H */
