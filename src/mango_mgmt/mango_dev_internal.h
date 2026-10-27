/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mango_dev private definitions, shared between the presence layer
 * (mango_dev.c) and its devlink health reporters (mango_dev_health.c).
 * mango_dev.h is the interface other modules bind to. Nothing here is.
 *
 * Only what both files have to agree on lives here. Everything the health
 * layer owns alone sits behind struct mango_dev_health, which is opaque
 * outside mango_dev_health.c.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */
#ifndef MANGO_DEV_INTERNAL_H
#define MANGO_DEV_INTERNAL_H

#include <linux/device.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pci.h>

#include "mango_dev.h"

#define MANGO_DEV_BDF_LEN 16
/* Member/candidate identifier: a PCIe BDF ("0000:2b:00.0") or an MMIO platform
 * device name ("a0000000.net"); the latter needs more than a BDF's worth. */
#define MANGO_DEV_ID_LEN 32

struct mango_dev_health;

/* Which bus a member/candidate lives on -- governs identity, naming, the
 * members view, and which actions (recover/reset are PCIe/host-only) apply. */
enum mango_dev_xport {
	MANGO_DEV_XPORT_PCI = 0,
	MANGO_DEV_XPORT_MMIO,
};

/**
 * struct mango_dev_func - a member function (hw_port) under a hardware unit.
 * @node: link on mango_dev.funcs.
 * @role: short label ("data", "mgmt", "smi", "sib").
 * @id: BDF (PCIe) or platform device name (MMIO).
 * @bridge: slot anchor, reference held; NULL for MMIO.
 * @sec_bus: the bridge's secondary bus number; PCIe only.
 * @present: the function is currently on the bus.
 */
struct mango_dev_func {
	struct list_head node;
	char role[8];
	char id[MANGO_DEV_ID_LEN];
	struct pci_dev *bridge;
	u8 sec_bus;
	bool present;
};

/**
 * struct mango_dev - a mango hardware unit: the functions sharing one hw_id
 * in one PCI domain.
 * @node: link on mango_dev_list.
 * @xport: PCIe functions or MMIO/SoC platform devices.
 * @hw_id: the shared HW_ID (a BDF, or the synthetic MMIO id).
 * @domain: PCI domain (HW_ID is domain-less; MMIO: 0).
 * @funcs: struct mango_dev_func members.
 * @dev: /sys/class/mango_dev/<node>.
 * @reset_lock: serializes reset on this node; also guards the recovery budget
 *              inside @health.
 * @fence_lock: guards @reset_in_flight and @reset_fences, and is never held
 *              across a reset, so a fence asked for during one is refused.
 * @reset_in_flight: a reset is running on this node. The member roster is
 *                   expected to be torn up while it is set.
 * @reset_fences: the reset fences held on this unit (mango_dev_reset_fence()).
 * @health: everything the devlink health layer owns -- the instance, the fatal
 *          reporter, the fault latch, the recovery budget and the last report.
 *          NULL on an MMIO/SoC node, on a kernel without devlink, and when the
 *          instance could not be built. Every entry point in
 *          mango_dev_health.c is a no-op then.
 */
struct mango_dev {
	struct list_head node;
	enum mango_dev_xport xport;
	u32 hw_id;
	u16 domain;
	struct list_head funcs;
	struct device *dev;
	struct mutex reset_lock;
	struct mutex fence_lock;
	bool reset_in_flight;
	unsigned int reset_fences;
	struct mango_dev_health *health;
};

/* Guards mango_dev_list, mango_dev_cand_list and the funcs of every unit. */
extern struct mutex mango_dev_lock;

/* The reset itself, once mango_dev_reset_claim() marked it. */
int mango_dev_reset_remove(struct mango_dev *hw, bool rescan);
struct mango_dev *mango_dev_find_slot(u16 domain, u8 bus);
/* Mark a reset running on @hw unless a reset fence holds it (-EBUSY); undone
 * by mango_dev_reset_unclaim(). Caller holds @reset_lock. */
int mango_dev_reset_claim(struct mango_dev *hw);
void mango_dev_reset_unclaim(struct mango_dev *hw);

/* mango_dev_health.c. The bus the anchors sit on, up before the first unit. */
int mango_dev_health_init(void);
void mango_dev_health_exit(void);
/* All are no-ops on a unit with no @health. */
void mango_dev_health_create(struct mango_dev *hw);
void mango_dev_health_destroy(struct mango_dev *hw);
void mango_dev_budget_clear(struct mango_dev *hw);
/* @mango_dev_lock held by the caller. */
void mango_dev_health_sync(struct mango_dev *hw);

/*
 * The unit's primary bus, decoded from hw_id -- which mango_core sets to the
 * primary function's BDF, PCI_DEVID(bus, devfn). Nothing about a slot is
 * hardcoded. The node name derives from this, and the devlink handle derives
 * from the node name, whatever the BDF turns out to be.
 */
static inline u8 mango_dev_primary_bus(const struct mango_dev *hw)
{
	return (hw->hw_id >> 8) & 0xff;
}

#endif /* MANGO_DEV_INTERNAL_H */
