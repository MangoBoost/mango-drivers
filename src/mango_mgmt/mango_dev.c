// SPDX-License-Identifier: GPL-2.0
/*
 * mango_dev — thin, mango_core-independent HW-presence layer.
 *
 * Owns an hw-boundary-aware inventory exposed at /sys/class/mango_dev/, so
 * userspace never walks /sys/bus/pci. Transport-agnostic: a mango device is a
 * PCIe function (host, or an SoC behind the card's second PCIe port) or an
 * MMIO platform device (an SoC that enumerates the card from a device tree).
 * Two tiers:
 *
 *   candidate — a mango device present but never registered (never worked as a
 *               mango-device). Discovered without binding by a PCI bus notifier
 *               (vendor 0x1F52) and a platform bus notifier ("mangoboost,*").
 *   member    — a device mango_core registered (worked >= once). PCIe members
 *               group by HW_ID and hold their slot-anchor (upstream bridge) so
 *               the hw survives a fabric death; all MMIO members belong to the
 *               one unit the SoC lives on and group into a single "soc" node.
 *
 * reset (remove the leaves, Secondary Bus Reset, optionally re-enumerate) is a
 * host/PCIe operation, exposed only on PCIe nodes.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/srcu.h>

#include <mango_compat/device_class.h>

#include "mango_dev.h"
#include "mango_dev_internal.h"
#include "dev.h"
#include "transport/pcie.h"

#define MANGO_DEV_VENDOR_ID MANGO_HW_PCI_VENDOR_ID
#define MANGO_DEV_MAX_BRIDGES 8
#define MANGO_DEV_MAX_FUNCS 16
/* After the bus reset the endpoint re-enumerates on a parent-bus rescan,
 * usually within one poll. Poll up to a generous bound rather than assuming a
 * fixed settle. */
#define MANGO_DEV_RECOVER_POLL_MS 2000
#define MANGO_DEV_RECOVER_TRIES 30 /* ~60s safety bound; typically 1 poll */
#define MANGO_DEV_RESET_READY_MS 5000 /* first config access after the reset */
#define MANGO_DEV_REASSERT_MAX 3
#define MANGO_DEV_REASSERT_EVERY 3
#define MANGO_DEV_LINK_WAIT_TRIES 150
#define MANGO_DEV_LINK_WAIT_MS 20

/* Every MMIO mango device the SoC sees is on the one unit it lives on, so they
 * all share this synthetic hw_id -> a single "soc" node. Distinct from a PCIe
 * hw_id (bit 31 set, low bits a BDF). */
#define MANGO_DEV_MMIO_HWID 0x40000000u
#define MANGO_DEV_MMIO_NODE "soc"
#define MANGO_DEV_COMPAT_PREFIX "mangoboost,"

static struct class *mango_dev_class;
DEFINE_MUTEX(mango_dev_lock); /* guards the two lists + create/destroy seq */
/* Serializes candidate discovery (notifier vs the init seed) so two callers
 * cannot both device_create() the same bdf (EEXIST WARN / orphan node). Ordered
 * BEFORE mango_dev_lock; the sysfs show() handlers take only mango_dev_lock, so
 * holding this across the sleeping device_create is ABBA-free. */
static DEFINE_MUTEX(mango_dev_discover_lock);
static LIST_HEAD(mango_dev_list); /* struct mango_dev */
static LIST_HEAD(mango_dev_cand_list); /* struct mango_dev_cand */
static const struct mango_dev_reset_ops
	*mango_dev_reset_ops; /* set by mango_core */
static const struct mango_dev_card_ops *mango_dev_cards; /* set at init */
/* Drains in-flight reset dispatches against set_reset_ops(NULL) so a concurrent
 * rmmod mango_core cannot free the ops' text mid-call. */
DEFINE_STATIC_SRCU(mango_dev_ops_srcu);

/* A recognized-but-unregistered mango function. */
struct mango_dev_cand {
	struct list_head node; /* mango_dev_cand_list */
	enum mango_dev_xport xport;
	char id[MANGO_DEV_ID_LEN];
	u16 vendor; /* PCIe only */
	u16 device; /* PCIe only */
	const char *role; /* PCIe only */
	u16 domain; /* PCIe only */
	struct pci_dev *bridge; /* ref held; NULL for MMIO */
	u8 sec_bus; /* PCIe only */
	struct device *dev; /* /sys/class/mango_dev/<id> */
};

static void mango_dev_fmt_bdf(struct pci_dev *pdev, char *buf, size_t len)
{
	snprintf(buf, len, "%04x:%02x:%02x.%x", pci_domain_nr(pdev->bus),
		 pdev->bus->number, PCI_SLOT(pdev->devfn),
		 PCI_FUNC(pdev->devfn));
}

/* The /sys/class/mango_dev/<node> name: a PCIe hw is named by its slot --
 * domain:bus of the primary's *secondary bus* (e.g. "0000:2b"), stable across
 * the removable leaf. hw_id carries only the BDF, so the domain is kept
 * alongside it (two units can share a BDF across domains). The single MMIO/SoC
 * hw is a fixed "soc". */
static void mango_dev_node_name(const struct mango_dev *hw, char *buf,
				size_t len)
{
	if (hw->xport == MANGO_DEV_XPORT_MMIO)
		strscpy(buf, MANGO_DEV_MMIO_NODE, len);
	else
		snprintf(buf, len, "%04x:%02x", hw->domain,
			 mango_dev_primary_bus(hw));
}

/* Resolve a live pci_dev from a "DDDD:BB:SS.F" bdf string (caller puts it). */
static struct pci_dev *mango_dev_bdf_to_pdev(const char *bdf)
{
	unsigned int dom, bus, slot, func;

	if (sscanf(bdf, "%x:%x:%x.%x", &dom, &bus, &slot, &func) != 4)
		return NULL;
	return pci_get_domain_bus_and_slot(dom, bus, PCI_DEVFN(slot, func));
}

/* callers hold mango_dev_lock. hw_id is domain-less, so match on the
 * (domain, hw_id) pair -- two units can share a BDF across PCI domains. */
static struct mango_dev *mango_dev_find_locked(u16 domain, u32 hw_id)
{
	struct mango_dev *hw;

	list_for_each_entry (hw, &mango_dev_list, node)
		if (hw->hw_id == hw_id && hw->domain == domain)
			return hw;
	return NULL;
}

static struct mango_dev_cand *mango_dev_cand_find_locked(const char *bdf)
{
	struct mango_dev_cand *c;

	list_for_each_entry (c, &mango_dev_cand_list, node)
		if (!strcmp(c->id, bdf))
			return c;
	return NULL;
}

/**
 * mango_dev_set_present() - flip a member's presence and settle its unit
 * @hw: the unit @f belongs to.
 * @f: the member function.
 * @present: whether it is on the bus now.
 *
 * The only writer of @f->present. The reporter state is derived from the whole
 * roster, and every transition has to settle it. Leaving that to each caller
 * is how a path ends up flipping a member silently.
 *
 * Caller holds mango_dev_lock.
 */
static void mango_dev_set_present(struct mango_dev *hw,
				  struct mango_dev_func *f, bool present)
{
	WRITE_ONCE(f->present, present);
	mango_dev_health_sync(hw);
}

/* If @bdf is an existing member function (any hw), mark it present, settle its
 * unit's health state and return true. Used on discovery so a member
 * re-appearing after a recover/reset (esp. a sibling, which has no driver to
 * re-register) flips back to present rather than being mistaken for an
 * already-tracked entry and left absent. */
static bool mango_dev_func_mark_present_locked(const char *bdf)
{
	struct mango_dev *hw;
	struct mango_dev_func *f;

	list_for_each_entry (hw, &mango_dev_list, node)
		list_for_each_entry (f, &hw->funcs, node)
			if (!strcmp(f->id, bdf)) {
				mango_dev_set_present(hw, f, true);
				return true;
			}
	return false;
}

/* Map a mango function to a member role. Data PFs register with "data"
 * explicitly; siblings take the role their driver's ID table states. */
static const char *mango_dev_role(struct pci_dev *pdev)
{
	const struct mango_mgmt_desc *desc = mango_mgmt_pcie_match(pdev);

	return desc ? desc->role : "sib";
}

/* Add (or refresh) a function on @hw; idempotent by bdf. Caller holds the lock.
 * Takes a reference on @bridge for the new func. */
static void mango_dev_func_add_locked(struct mango_dev *hw, const char *bdf,
				      const char *role, struct pci_dev *bridge,
				      u8 sec_bus)
{
	struct mango_dev_func *f;

	list_for_each_entry (f, &hw->funcs, node)
		if (!strcmp(f->id, bdf)) {
			/* Refresh identity: a data PF first seen via the notifier
			 * is absorbed with the default "sib" role; its later
			 * explicit register(...,"data") must upgrade it so the
			 * members inventory reports the true role. */
			strscpy(f->role, role, sizeof(f->role));
			if (f->bridge != bridge) {
				pci_dev_put(f->bridge);
				f->bridge = pci_dev_get(bridge);
			}
			f->sec_bus = sec_bus;
			mango_dev_set_present(hw, f, true);
			return;
		}
	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return;
	strscpy(f->id, bdf, sizeof(f->id));
	strscpy(f->role, role, sizeof(f->role));
	f->bridge = pci_dev_get(bridge);
	f->sec_bus = sec_bus;
	list_add_tail(&f->node, &hw->funcs);
	mango_dev_set_present(hw, f, true);
}

/* The PCIe hw owning secondary bus @domain:@bus, or NULL. Caller holds the lock.
 * A physical unit owns whole buses, so a (domain,bus) maps to at most one hw.
 * bus 0 is not a unit slot (a bridgeless/root-complex/passthrough function
 * computes bus 0) -- never claim an owner for it, or unrelated funcs get
 * absorbed. */
static struct mango_dev *mango_dev_bus_owner_locked(u16 domain, u8 bus)
{
	struct mango_dev *hw;
	struct mango_dev_func *f;

	if (!bus)
		return NULL;
	list_for_each_entry (hw, &mango_dev_list, node) {
		if (hw->xport != MANGO_DEV_XPORT_PCI || hw->domain != domain)
			continue;
		list_for_each_entry (f, &hw->funcs, node)
			if (f->sec_bus == bus)
				return hw;
	}
	return NULL;
}

/**
 * mango_dev_find_slot() - the PCIe unit that owns secondary bus @domain:@bus.
 * @domain: PCI domain.
 * @bus: the bus the reporting function sits on.
 *
 * For callers outside the presence layer (the health reporters, and through
 * them mango_core). A unit is retained until module unload. The returned
 * pointer therefore stays valid for as long as mango-mgmt is loaded -- and
 * mango_core, the only reporter, cannot outlive it.
 *
 * Return: the unit, or NULL when the bus belongs to no registered unit.
 */
struct mango_dev *mango_dev_find_slot(u16 domain, u8 bus)
{
	struct mango_dev *hw;

	mutex_lock(&mango_dev_lock);
	hw = mango_dev_bus_owner_locked(domain, bus);
	mutex_unlock(&mango_dev_lock);
	return hw;
}

/* ---- sysfs: hw device ---------------------------------------------------
 * show() reads immutable-at-create fields locklessly; the members view takes
 * the lock to walk funcs. Destroy paths drop the lock before device_unregister
 * so an in-flight show never deadlocks against it.
 */
static ssize_t hw_id_show(struct device *dev, struct device_attribute *a,
			  char *buf)
{
	struct mango_dev *hw = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%08x\n", hw->hw_id);
}
static DEVICE_ATTR_RO(hw_id);

/*
 * Presence only, which is all a single word can carry honestly. A unit can be
 * fully enumerated and still be latched dead, and this attribute reads ok for
 * both. The unit verdict and the inputs behind it live on the fatal reporter's
 * devlink diagnose instead.
 */
static ssize_t state_show(struct device *dev, struct device_attribute *a,
			  char *buf)
{
	struct mango_dev *hw = dev_get_drvdata(dev);
	struct mango_dev_func *f;
	bool any_present = false;

	mutex_lock(&mango_dev_lock);
	list_for_each_entry (f, &hw->funcs, node)
		any_present |= READ_ONCE(f->present);
	mutex_unlock(&mango_dev_lock);

	return sysfs_emit(buf, "%s\n", any_present ? "ok" : "removed");
}
static DEVICE_ATTR_RO(state);

static ssize_t members_show(struct device *dev, struct device_attribute *a,
			    char *buf)
{
	struct mango_dev *hw = dev_get_drvdata(dev);
	struct mango_dev_func *f;
	int len = 0;

	mutex_lock(&mango_dev_lock);
	list_for_each_entry (f, &hw->funcs, node) {
		const char *state =
			READ_ONCE(f->present) ? "present" : "absent";

		/* MMIO members have no bridge/secondary bus; keep the view honest
		 * rather than printing placeholder PCIe fields. */
		if (hw->xport == MANGO_DEV_XPORT_MMIO)
			len += sysfs_emit_at(buf, len, "%-5s %s %s\n", f->role,
					     f->id, state);
		else
			len += sysfs_emit_at(
				buf, len, "%-5s %s bridge=%s sec_bus=%02x %s\n",
				f->role, f->id,
				f->bridge ? pci_name(f->bridge) : "-",
				f->sec_bus, state);
	}
	mutex_unlock(&mango_dev_lock);

	return len;
}
static DEVICE_ATTR_RO(members);

/* Reset a unit's bus directly by asserting its bridge's Secondary Bus
 * Reset. Used by the "remove" leaf mode, which has already detached the
 * leaves (or they fell off in a fabric death), so there is no live bound
 * driver to coordinate with. No guard: the operator asked to remove. */
static int mango_dev_bus_reset(struct pci_dev *bridge)
{
	int ret;

	if (!bridge->subordinate)
		return -ENODEV;
	pci_lock_rescan_remove();
	ret = pci_bridge_secondary_bus_reset(bridge);
	pci_unlock_rescan_remove();
	if (ret)
		pci_warn(bridge, "mango_dev: bus %02x reset failed (%d)\n",
			 bridge->subordinate->number, ret);
	return ret;
}

/**
 * struct mango_dev_port_quirk - how to reset a card behind one kind of port
 * @name: printed when the quirk applies.
 * @link_disable_ms: hold Link Disable this long, set before the SBR is
 *                   released. It also turns off the link-down SBR re-assert
 *                   and drops non-mango functions before each rescan.
 */
struct mango_dev_port_quirk {
	const char *name;
	unsigned int link_disable_ms;
};

/*
 * The card warm-resets itself as soon as the link retrains after a hot reset,
 * so the link drops twice. A Broadcom synthetic-mode virtual port answers the
 * second drop by binding a placeholder endpoint (1000:02b2) for good. Link
 * Disable set before the SBR is released and held 2 s re-binds the card; a
 * shorter hold, or Link Disable set after the SBR, leaves the placeholder.
 */
static const struct mango_dev_port_quirk mango_dev_brcm_vport = {
	.name = "Broadcom virtual port",
	.link_disable_ms = 2000,
};

/*
 * Anchor ports that need more than a plain SBR, matched on downstream ports
 * only. Root ports and unlisted switches keep the plain SBR.
 */
static const struct pci_device_id mango_dev_port_quirks[] = {
	/* PEX88000/PEX89000 synthetic mode, any device ID ("Virtual ... Port") */
	{ PCI_DEVICE_SUB(PCI_VENDOR_ID_LSI_LOGIC, PCI_ANY_ID,
			 PCI_VENDOR_ID_LSI_LOGIC, 0x100b),
	  .driver_data = (kernel_ulong_t)&mango_dev_brcm_vport },
	{}
};

/**
 * mango_dev_unit_quirk() - the reset quirk shared by every anchor of a unit
 * @bridges: the unit's anchor bridges.
 * @nb: number of @bridges.
 *
 * Return: the quirk when every anchor is a downstream port matching the same
 * mango_dev_port_quirks entry, NULL otherwise (plain SBR).
 */
static const struct mango_dev_port_quirk *
mango_dev_unit_quirk(struct pci_dev **bridges, int nb)
{
	const struct mango_dev_port_quirk *q = NULL;
	const struct pci_device_id *id;
	int i;

	for (i = 0; i < nb; i++) {
		if (pci_pcie_type(bridges[i]) != PCI_EXP_TYPE_DOWNSTREAM)
			return NULL;
		id = pci_match_id(mango_dev_port_quirks, bridges[i]);
		if (!id || (q && q != (const void *)id->driver_data))
			return NULL;
		q = (const void *)id->driver_data;
	}
	return q;
}

/**
 * mango_dev_held_link_reset() - SBR plus a held Link Disable on every anchor
 * @bridges: the unit's anchor bridges.
 * @nb: number of @bridges.
 * @q: the unit's quirk, with a non-zero @q->link_disable_ms.
 *
 * Every anchor goes together because a bifurcated card reboots as one unit.
 *
 * Return: 0 on success, -ENODEV when a bridge has no subordinate bus.
 */
static int mango_dev_held_link_reset(struct pci_dev **bridges, int nb,
				     const struct mango_dev_port_quirk *q)
{
	u16 ctrl;
	int i;

	for (i = 0; i < nb; i++)
		if (!bridges[i]->subordinate)
			return -ENODEV;

	pci_lock_rescan_remove();
	for (i = 0; i < nb; i++) {
		pci_read_config_word(bridges[i], PCI_BRIDGE_CONTROL, &ctrl);
		pci_write_config_word(bridges[i], PCI_BRIDGE_CONTROL,
				      ctrl | PCI_BRIDGE_CTL_BUS_RESET);
	}
	msleep(2);
	for (i = 0; i < nb; i++)
		pcie_capability_set_word(bridges[i], PCI_EXP_LNKCTL,
					 PCI_EXP_LNKCTL_LD);
	for (i = 0; i < nb; i++) {
		pci_read_config_word(bridges[i], PCI_BRIDGE_CONTROL, &ctrl);
		pci_write_config_word(bridges[i], PCI_BRIDGE_CONTROL,
				      ctrl & ~PCI_BRIDGE_CTL_BUS_RESET);
	}
	pci_unlock_rescan_remove();

	msleep(q->link_disable_ms);
	for (i = 0; i < nb; i++)
		pcie_capability_clear_word(bridges[i], PCI_EXP_LNKCTL,
					   PCI_EXP_LNKCTL_LD);
	pci_info(bridges[0],
		 "mango_dev: %s, bus reset with %ums link disable\n", q->name,
		 q->link_disable_ms);
	return 0;
}

/**
 * mango_dev_drop_foreign() - remove non-mango functions behind @bridge
 * @bridge: an anchor bridge with a port quirk.
 *
 * A rescan that catches the switch's placeholder endpoint enumerates it at the
 * member's BDF, and later rescans then skip that BDF. Caller holds
 * pci_lock_rescan_remove().
 */
static void mango_dev_drop_foreign(struct pci_dev *bridge)
{
	struct pci_dev *child, *tmp;

	list_for_each_entry_safe (child, tmp, &bridge->subordinate->devices,
				  bus_list) {
		if (child->vendor == MANGO_DEV_VENDOR_ID)
			continue;
		pci_info(child, "mango_dev: removing foreign %04x:%04x\n",
			 child->vendor, child->device);
		pci_stop_and_remove_bus_device(child);
	}
}

static void mango_dev_wait_link(struct pci_dev *bridge)
{
	int i;
	u16 lnksta = 0;

	for (i = 0; i < MANGO_DEV_LINK_WAIT_TRIES; i++) {
		pcie_capability_read_word(bridge, PCI_EXP_LNKSTA, &lnksta);
		if (lnksta & PCI_EXP_LNKSTA_DLLLA)
			return;
		msleep(MANGO_DEV_LINK_WAIT_MS);
	}
	pci_info(
		bridge,
		"mango_dev: link not active %dms after reset; rescan poll continues\n",
		MANGO_DEV_LINK_WAIT_TRIES * MANGO_DEV_LINK_WAIT_MS);
}

/**
 * mango_dev_all_present() - is every member function of @hw back on the bus?
 * @hw: the hardware unit.
 *
 * The re-enumeration verdict. The bus notifier marks each function present as
 * it re-appears, which covers the siblings (mgmt, smi) that have no driver of
 * their own to register. "The secondary bus is not empty" is a different, much
 * weaker question. One function of four is enough to answer that one yes while
 * the aux devices never rebind.
 *
 * Return: true when no member is absent.
 */
static bool mango_dev_all_present(struct mango_dev *hw)
{
	struct mango_dev_func *f;
	bool all = true;

	mutex_lock(&mango_dev_lock);
	list_for_each_entry (f, &hw->funcs, node)
		all &= READ_ONCE(f->present);
	mutex_unlock(&mango_dev_lock);
	return all;
}

/* leaf=remove: evict every present member, then reset each anchor bus, then --
 * if @rescan -- re-enumerate. One operation covering golden reflash, fabric-death
 * recovery, and the traditional uncooperative reset: "remove" is a no-op when the
 * leaves are already gone (fabric death) and an eviction when they are present.
 * (remove, rescan)  = re-enumerate the unit.
 * (remove, norescan) = detach + bus-reset, leave the slot ready (pre-reflash). */
int mango_dev_reset_remove(struct mango_dev *hw, bool rescan)
{
	struct pci_dev *bridges[MANGO_DEV_MAX_BRIDGES];
	char bdfs[MANGO_DEV_MAX_FUNCS][MANGO_DEV_BDF_LEN];
	struct mango_dev_func *f;
	unsigned long t0 = jiffies;
	int nb = 0, nf = 0, i, t, ret = -ENODEV;
	int reassert[MANGO_DEV_MAX_BRIDGES] = { 0 };
	const struct mango_dev_port_quirk *quirk;
	bool asserted = false;

	/* Snapshot present members' bdfs + the unique anchor bridges (hold refs;
	 * they outlive the leaves). Drop the lock before removal, since a leaf's
	 * .remove re-enters mango_dev (unregister). */
	mutex_lock(&mango_dev_lock);
	list_for_each_entry (f, &hw->funcs, node) {
		if (f->bridge) {
			for (i = 0; i < nb; i++)
				if (bridges[i] == f->bridge)
					break;
			if (i == nb && nb < MANGO_DEV_MAX_BRIDGES)
				bridges[nb++] = pci_dev_get(f->bridge);
		}
		if (READ_ONCE(f->present) && nf < MANGO_DEV_MAX_FUNCS)
			strscpy(bdfs[nf++], f->id, MANGO_DEV_BDF_LEN);
	}
	mutex_unlock(&mango_dev_lock);

	/* Precondition: without an anchor bridge there is nothing to bus-reset,
	 * and evicting the leaves first would strand them detached (a norescan
	 * caller cannot bring them back). Bail before touching any binding. */
	if (!nb)
		return -ENODEV;

	/* Evict the present leaves (their .remove marks the member absent; the
	 * bridge ref we hold keeps the anchor alive). No-op if already gone. */
	for (i = 0; i < nf; i++) {
		struct pci_dev *pdev = mango_dev_bdf_to_pdev(bdfs[i]);

		if (!pdev)
			continue;
		pci_stop_and_remove_bus_device_locked(pdev);
		pci_dev_put(pdev);
	}

	/* Reset every anchor bus back-to-back: a bifurcated unit is one physical
	 * unit, so resetting its links seconds apart would re-reset it mid-recovery. */
	quirk = mango_dev_unit_quirk(bridges, nb);
	if (quirk)
		asserted = !mango_dev_held_link_reset(bridges, nb, quirk);
	else
		for (i = 0; i < nb; i++)
			if (!mango_dev_bus_reset(bridges[i]))
				asserted = true;
	if (asserted)
		ret = 0;

	/* The bus reset fenced DMA: reclaim any F_DMA buffers quarantined on these
	 * slots. The eviction above freed their owning core, so the by-core release
	 * can no longer match -- reclaim by slot via the delegate (under SRCU, so a
	 * concurrent rmmod mango_core cannot free the op mid-call). */
	if (asserted) {
		const struct mango_dev_reset_ops *ops;
		int idx = srcu_read_lock(&mango_dev_ops_srcu);

		ops = READ_ONCE(mango_dev_reset_ops);
		if (ops && ops->reclaim_fenced)
			for (i = 0; i < nb; i++)
				if (bridges[i]->subordinate)
					ops->reclaim_fenced(
						pci_domain_nr(bridges[i]->bus),
						bridges[i]->subordinate->number);
		srcu_read_unlock(&mango_dev_ops_srcu, idx);
	}

	if (asserted && rescan) {
		pci_lock_rescan_remove();
		msleep(MANGO_DEV_RESET_READY_MS);
		pci_unlock_rescan_remove();
		for (t = 0; t < MANGO_DEV_RECOVER_TRIES; t++) {
			if (t)
				msleep(MANGO_DEV_RECOVER_POLL_MS);

			if (!quirk && t && t % MANGO_DEV_REASSERT_EVERY == 0)
				for (i = 0; i < nb; i++) {
					u16 ls = 0;

					if (reassert[i] >=
					    MANGO_DEV_REASSERT_MAX)
						continue;
					pcie_capability_read_word(
						bridges[i], PCI_EXP_LNKSTA,
						&ls);
					if (ls & PCI_EXP_LNKSTA_DLLLA)
						continue;
					reassert[i]++;
					pci_warn(
						bridges[i],
						"mango_dev: link down, re-asserting bus reset (%d/%d)\n",
						reassert[i],
						MANGO_DEV_REASSERT_MAX);
					if (!mango_dev_bus_reset(bridges[i]))
						mango_dev_wait_link(bridges[i]);
				}

			pci_lock_rescan_remove();
			/* Rescan from the bridge's PARENT bus, as the sysfs bridge
			 * .../rescan does. That re-evaluates the bridge's
			 * secondary side and recurses behind it. Rescanning the
			 * emptied subordinate bus alone does NOT re-enumerate a
			 * post-reset endpoint. */
			for (i = 0; i < nb; i++) {
				if (quirk && bridges[i]->subordinate)
					mango_dev_drop_foreign(bridges[i]);
				if (bridges[i]->bus)
					pci_rescan_bus(bridges[i]->bus);
			}
			pci_unlock_rescan_remove();
			if (mango_dev_all_present(hw))
				break;
		}
		ret = (t < MANGO_DEV_RECOVER_TRIES) ? 0 : -ETIMEDOUT;
		/*
		 * Elapsed, not the poll interval. pci_rescan_bus() re-probes
		 * every driver behind the bridge before it returns. The first
		 * poll therefore typically already carries the whole aux
		 * bring-up -- an order of magnitude more than
		 * MANGO_DEV_RECOVER_POLL_MS.
		 */
		if (ret) {
			int absent = 0, total = 0;

			mutex_lock(&mango_dev_lock);
			list_for_each_entry (f, &hw->funcs, node) {
				total++;
				if (READ_ONCE(f->present))
					continue;
				absent++;
				dev_warn(
					hw->dev,
					"reset(remove,rescan): member %s(%s) did not come back\n",
					f->id, f->role);
			}
			mutex_unlock(&mango_dev_lock);
			dev_warn(
				hw->dev,
				"reset(remove,rescan): timed out after %ums, %d of %d members absent\n",
				jiffies_to_msecs(jiffies - t0), absent, total);
		} else {
			dev_info(
				hw->dev,
				"reset(remove,rescan): re-enumerated after %ums\n",
				jiffies_to_msecs(jiffies - t0));
		}
	}

	for (i = 0; i < nb; i++)
		pci_dev_put(bridges[i]);
	return ret;
}

/* Reset action, two explicit knobs: "<leaf> <rescan>".
 *   leaf   = remove                 (the cooperative in-place leaf was reaped:
 *                                     the datapath engine cannot recover in
 *                                     place, so recovery is always re-enumeration)
 *   rescan = rescan | norescan       (re-enumerate after, or not)
 * "remove rescan"   re-enumerates the unit (the recovery path).
 * "remove norescan" detaches + bus-resets, leaving the slot ready (pre-reflash).
 * Explicit, not inferred from presence. A state-based branch would be a TOCTOU
 * race. A caller that read a now-stale state gets -errno and can retry.
 * A write is an operator action. It clears the autonomous-recovery budget
 * instead of consuming it -- this is also how a spent budget is un-latched. */
static ssize_t reset_store(struct device *dev, struct device_attribute *a,
			   const char *buf, size_t count)
{
	struct mango_dev *hw = dev_get_drvdata(dev);
	char leaf[16], rescan[16];
	bool do_rescan;
	int ret;

	if (sscanf(buf, "%15s %15s", leaf, rescan) != 2)
		return -EINVAL;
	if (strcmp(leaf, "remove"))
		return -EINVAL; /* cooperative in-place reset reaped: remove-mode only */
	if (!strcmp(rescan, "rescan"))
		do_rescan = true;
	else if (!strcmp(rescan, "norescan"))
		do_rescan = false;
	else
		return -EINVAL;

	/* Serialize resets on this node: two concurrent stores would evict /
	 * SBR the same buses against each other. */
	mutex_lock(&hw->reset_lock);
	if (mango_dev_reset_claim(hw)) {
		mutex_unlock(&hw->reset_lock);
		dev_warn(hw->dev,
			 "reset refused: a reset fence holds the unit\n");
		return -EBUSY;
	}
	mango_dev_budget_clear(hw);
	ret = mango_dev_reset_remove(hw, do_rescan);
	mango_dev_reset_unclaim(hw);
	mutex_lock(&mango_dev_lock);
	mango_dev_health_sync(hw);
	mutex_unlock(&mango_dev_lock);
	mutex_unlock(&hw->reset_lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(reset);

/**
 * mango_dev_reset_fence() - fence the unit under @dev against resets.
 * @dev: a member of the unit.
 *
 * Return: the fenced unit, NULL when @dev sits on no registered unit, or
 *         ERR_PTR(-EBUSY) while a reset runs.
 */
struct mango_dev *mango_dev_reset_fence(struct device *dev)
{
	struct pci_dev *pdev;
	struct mango_dev *hw;

	if (!dev || !dev_is_pci(dev))
		return NULL;

	pdev = to_pci_dev(dev);
	hw = mango_dev_find_slot(pci_domain_nr(pdev->bus), pdev->bus->number);
	if (!hw)
		return NULL;

	mutex_lock(&hw->fence_lock);
	if (hw->reset_in_flight) {
		mutex_unlock(&hw->fence_lock);
		return ERR_PTR(-EBUSY);
	}
	hw->reset_fences++;
	mutex_unlock(&hw->fence_lock);
	return hw;
}

/**
 * mango_dev_reset_unfence() - drop a fence taken by mango_dev_reset_fence().
 * @hw: the fenced unit. NULL is accepted and ignored.
 */
void mango_dev_reset_unfence(struct mango_dev *hw)
{
	if (!hw)
		return;

	mutex_lock(&hw->fence_lock);
	if (!WARN_ON(!hw->reset_fences))
		hw->reset_fences--;
	mutex_unlock(&hw->fence_lock);
}

/* A reset fence holds @hw: one this host holds, or a member's card that
 * publishes Busy. Caller holds @fence_lock. */
static bool mango_dev_reset_fenced(struct mango_dev *hw)
{
	char bdfs[MANGO_DEV_MAX_FUNCS][MANGO_DEV_BDF_LEN];
	struct mango_dev_func *f;
	int nf = 0, i;

	lockdep_assert_held(&hw->fence_lock);
	if (hw->reset_fences)
		return true;
	if (!mango_dev_cards)
		return false;

	/* A card answers from its BAR, so ask outside mango_dev_lock. */
	mutex_lock(&mango_dev_lock);
	list_for_each_entry (f, &hw->funcs, node)
		if (READ_ONCE(f->present) && nf < MANGO_DEV_MAX_FUNCS)
			strscpy(bdfs[nf++], f->id, MANGO_DEV_BDF_LEN);
	mutex_unlock(&mango_dev_lock);

	for (i = 0; i < nf; i++) {
		struct pci_dev *pdev = mango_dev_bdf_to_pdev(bdfs[i]);
		bool busy = pdev && mango_dev_cards->card_busy(&pdev->dev);

		pci_dev_put(pdev);
		if (busy)
			return true;
	}

	return false;
}

int mango_dev_reset_claim(struct mango_dev *hw)
{
	int ret = 0;

	lockdep_assert_held(&hw->reset_lock);
	mutex_lock(&hw->fence_lock);
	if (mango_dev_reset_fenced(hw))
		ret = -EBUSY;
	else
		WRITE_ONCE(hw->reset_in_flight, true);
	mutex_unlock(&hw->fence_lock);
	return ret;
}

void mango_dev_reset_unclaim(struct mango_dev *hw)
{
	mutex_lock(&hw->fence_lock);
	WRITE_ONCE(hw->reset_in_flight, false);
	mutex_unlock(&hw->fence_lock);
}

static struct attribute *mango_dev_dev_attrs[] = {
	&dev_attr_hw_id.attr,
	&dev_attr_state.attr,
	&dev_attr_members.attr,
	&dev_attr_reset.attr,
	NULL,
};

/* reset (SBR / pci_reset_bus, in any leaf mode) is a PCIe/host operation with no
 * meaning on an MMIO/SoC node -- hide it there. */
static umode_t mango_dev_dev_attr_visible(struct kobject *kobj,
					  struct attribute *a, int n)
{
	struct mango_dev *hw = dev_get_drvdata(kobj_to_dev(kobj));

	if (a == &dev_attr_reset.attr && hw && hw->xport != MANGO_DEV_XPORT_PCI)
		return 0;
	return a->mode;
}

static const struct attribute_group mango_dev_dev_group = {
	.attrs = mango_dev_dev_attrs,
	.is_visible = mango_dev_dev_attr_visible,
};
static const struct attribute_group *mango_dev_dev_groups[] = {
	&mango_dev_dev_group,
	NULL,
};

/* ---- sysfs: candidate device ------------------------------------------- */
static ssize_t bdf_show(struct device *dev, struct device_attribute *a,
			char *buf)
{
	struct mango_dev_cand *c = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", c->id);
}
static DEVICE_ATTR_RO(bdf);

static ssize_t ids_show(struct device *dev, struct device_attribute *a,
			char *buf)
{
	struct mango_dev_cand *c = dev_get_drvdata(dev);

	if (c->xport == MANGO_DEV_XPORT_MMIO)
		return sysfs_emit(buf, "platform\n");
	return sysfs_emit(buf, "%04x:%04x\n", c->vendor, c->device);
}
static DEVICE_ATTR_RO(ids);

static ssize_t cand_state_show(struct device *dev, struct device_attribute *a,
			       char *buf)
{
	return sysfs_emit(buf, "candidate\n");
}
/* file named "state"; distinct symbol from the hw device's dev_attr_state. */
static struct device_attribute dev_attr_cand_state =
	__ATTR(state, 0444, cand_state_show, NULL);

static struct attribute *mango_dev_cand_attrs[] = {
	&dev_attr_bdf.attr,
	&dev_attr_ids.attr,
	&dev_attr_cand_state.attr,
	NULL,
};
ATTRIBUTE_GROUPS(mango_dev_cand);

/* ---- candidate lifecycle ------------------------------------------------ */
static void mango_dev_cand_free(struct mango_dev_cand *c);

static void __mango_dev_cand_add(struct pci_dev *pdev)
{
	struct pci_dev *bridge = pci_upstream_bridge(pdev);
	struct mango_dev_cand *c;
	struct mango_dev *owner;
	char bdf[MANGO_DEV_BDF_LEN];
	u16 domain = pci_domain_nr(pdev->bus);
	u8 bus =
		bridge && bridge->subordinate ? bridge->subordinate->number : 0;

	mango_dev_fmt_bdf(pdev, bdf, sizeof(bdf));

	/* Fast path: already tracked, or absorbable as a sibling, without a new
	 * candidate. An existing member re-appearing (e.g. after recover) flips
	 * back to present; a bus already owned by an hw means this is a sibling
	 * (mgmt/smi) -> absorb it as a member. */
	mutex_lock(&mango_dev_lock);
	if (mango_dev_func_mark_present_locked(bdf) ||
	    mango_dev_cand_find_locked(bdf)) {
		mutex_unlock(&mango_dev_lock);
		return;
	}
	owner = mango_dev_bus_owner_locked(domain, bus);
	if (owner) {
		mango_dev_func_add_locked(owner, bdf, mango_dev_role(pdev),
					  bridge, bus);
		mutex_unlock(&mango_dev_lock);
		return;
	}
	mutex_unlock(&mango_dev_lock);

	/* Build + create the sysfs device BEFORE publishing to the list. While c
	 * is off the list, the register()/absorb path cannot find or free it, so
	 * the create -- which sleeps and must NOT run under mango_dev_lock, since
	 * the hw-node show() handlers take that lock (ABBA) -- is race-safe. */
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return;
	strscpy(c->id, bdf, sizeof(c->id));
	c->vendor = pdev->vendor;
	c->device = pdev->device;
	c->role = mango_dev_role(pdev);
	c->bridge = pci_dev_get(bridge);
	c->sec_bus = bus;
	c->domain = domain;
	c->dev =
		device_create_with_groups(mango_dev_class, NULL, MKDEV(0, 0), c,
					  mango_dev_cand_groups, "%s", c->id);
	if (IS_ERR(c->dev))
		c->dev = NULL;

	/* Publish -- re-checking, since a concurrent register() may have promoted
	 * this bdf or taken its bus while the device was being created. */
	mutex_lock(&mango_dev_lock);
	if (mango_dev_func_mark_present_locked(bdf) ||
	    mango_dev_cand_find_locked(bdf)) {
		mutex_unlock(&mango_dev_lock);
		mango_dev_cand_free(
			c); /* lost the race -- drop the duplicate */
		return;
	}
	owner = mango_dev_bus_owner_locked(domain, bus);
	if (owner) {
		mango_dev_func_add_locked(owner, bdf, mango_dev_role(pdev),
					  bridge, bus);
		mutex_unlock(&mango_dev_lock);
		mango_dev_cand_free(c); /* absorbed as a member instead */
		return;
	}
	list_add_tail(&c->node, &mango_dev_cand_list);
	mutex_unlock(&mango_dev_lock);
}

static void mango_dev_cand_add(struct pci_dev *pdev)
{
	mutex_lock(&mango_dev_discover_lock);
	__mango_dev_cand_add(pdev);
	mutex_unlock(&mango_dev_discover_lock);
}

/* callers hold mango_dev_lock; unlinks and returns the candidate to destroy. */
static struct mango_dev_cand *mango_dev_cand_unlink_locked(const char *bdf)
{
	struct mango_dev_cand *c = mango_dev_cand_find_locked(bdf);

	if (c)
		list_del(&c->node);
	return c;
}

static void mango_dev_cand_free(struct mango_dev_cand *c)
{
	if (!c)
		return;
	if (c->dev)
		device_unregister(c->dev); /* waits for in-flight show */
	pci_dev_put(c->bridge);
	kfree(c);
}

/* A tracked device @id left the tree: drop its candidate (if any) and mark any
 * member with that id absent (the hw is retained). Transport-agnostic. */
static void mango_dev_cand_remove_id(const char *id)
{
	struct mango_dev_cand *c;
	struct mango_dev *hw;
	struct mango_dev_func *f;

	mutex_lock(&mango_dev_lock);
	c = mango_dev_cand_unlink_locked(id);
	/* Sibling members (mgmt/smi) have no driver to call unregister, so the
	 * notifier is how we learn they left -> mark absent (hw is retained). */
	list_for_each_entry (hw, &mango_dev_list, node)
		list_for_each_entry (f, &hw->funcs, node)
			if (!strcmp(f->id, id))
				mango_dev_set_present(hw, f, false);
	mutex_unlock(&mango_dev_lock);
	mango_dev_cand_free(c);
}

static void mango_dev_cand_remove(struct pci_dev *pdev)
{
	char bdf[MANGO_DEV_BDF_LEN];

	mango_dev_fmt_bdf(pdev, bdf, sizeof(bdf));
	mango_dev_cand_remove_id(bdf);
}

/* ---- member registration (called by mango_core) ------------------------ */
int mango_dev_register(struct pci_dev *pdev, u32 hw_id, const char *role)
{
	struct mango_dev *hw, *new_hw = NULL;
	struct mango_dev_cand *promoted, *c, *tc;
	struct pci_dev *bridge;
	char bdf[MANGO_DEV_BDF_LEN];
	LIST_HEAD(absorbed);
	bool created = false;
	u16 domain;
	u8 bus;

	if (!pdev)
		return -EINVAL;
	mango_dev_fmt_bdf(pdev, bdf, sizeof(bdf));
	bridge = pci_upstream_bridge(pdev);
	domain = pci_domain_nr(pdev->bus);
	bus = bridge && bridge->subordinate ? bridge->subordinate->number : 0;

	/* 0 hw_id (unresolved) => singleton keyed by this function's BDF. */
	if (!hw_id)
		hw_id = pci_dev_id(pdev) | 0x80000000u;

	mutex_lock(&mango_dev_lock);
	promoted = mango_dev_cand_unlink_locked(bdf);

	hw = mango_dev_find_locked(domain, hw_id);
	if (!hw) {
		new_hw = kzalloc(sizeof(*new_hw), GFP_KERNEL);
		if (!new_hw) {
			mutex_unlock(&mango_dev_lock);
			mango_dev_cand_free(promoted);
			return -ENOMEM;
		}
		new_hw->hw_id = hw_id;
		new_hw->domain = domain;
		new_hw->xport = MANGO_DEV_XPORT_PCI;
		mutex_init(&new_hw->reset_lock);
		mutex_init(&new_hw->fence_lock);
		INIT_LIST_HEAD(&new_hw->funcs);
		list_add_tail(&new_hw->node, &mango_dev_list);
		hw = new_hw;
		created = true;
	}

	/* Add (or refresh) this data PF as a member. */
	mango_dev_func_add_locked(hw, bdf, role ? role : "data", bridge, bus);

	/* Absorb any candidate on this same-domain bus as a sibling member (mgmt/
	 * smi -- they carry no HW_ID and can't register themselves). Unlink here;
	 * free their candidate sysfs devices after the lock drops. */
	list_for_each_entry_safe (c, tc, &mango_dev_cand_list, node) {
		if (c->sec_bus != bus || c->domain != domain ||
		    c->xport != MANGO_DEV_XPORT_PCI)
			continue;
		list_del(&c->node);
		mango_dev_func_add_locked(hw, c->id, c->role, c->bridge,
					  c->sec_bus);
		list_add_tail(&c->node, &absorbed);
	}
	mutex_unlock(&mango_dev_lock);

	if (created) {
		char name[MANGO_DEV_BDF_LEN];

		mango_dev_node_name(hw, name, sizeof(name));
		hw->dev = device_create_with_groups(mango_dev_class, NULL,
						    MKDEV(0, 0), hw,
						    mango_dev_dev_groups, "%s",
						    name);
		if (IS_ERR(hw->dev)) {
			/* The hw is tracked but has no sysfs control node: reset /
			 * members are unreachable. Fail loudly rather than silently. */
			pr_err("mango_dev: node %s: sysfs create failed (%ld); reset/members unavailable\n",
			       name, PTR_ERR(hw->dev));
			hw->dev = NULL;
		}
		mango_dev_health_create(hw);
	}
	mango_dev_cand_free(promoted); /* device_unregister outside the lock */
	list_for_each_entry_safe (c, tc, &absorbed, node) {
		list_del(&c->node);
		mango_dev_cand_free(c);
	}
	mutex_lock(&mango_dev_lock);
	mango_dev_health_sync(hw);
	mutex_unlock(&mango_dev_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(mango_dev_register);

/* ---- MMIO/platform members (platform-bus SoC) -------------------------- */

/* True if @dev is a mango platform device (a "mangoboost,*" DT node). */
static bool mango_dev_dev_is_mango_plt(struct device *dev)
{
	struct device_node *np = dev->of_node;
	const char *compat;
	int i = 0;

	if (!np)
		return false;
	while (of_property_read_string_index(np, "compatible", i++, &compat) ==
	       0)
		if (!strncmp(compat, MANGO_DEV_COMPAT_PREFIX,
			     strlen(MANGO_DEV_COMPAT_PREFIX)))
			return true;
	return false;
}

/* Derive a member role from a mango platform device's compatible suffix
 * ("mangoboost,net" -> "net"); "data" if it cannot be read. */
static void mango_dev_mmio_role(struct device *dev, char *buf, size_t len)
{
	const char *compat, *suffix;
	int i = 0;

	strscpy(buf, "data", len);
	if (!dev->of_node)
		return;
	while (of_property_read_string_index(dev->of_node, "compatible", i++,
					     &compat) == 0) {
		suffix = strchr(compat, ',');
		if (suffix && *(suffix + 1)) {
			strscpy(buf, suffix + 1, len);
			return;
		}
	}
}

int mango_dev_register_mmio(struct device *dev, const char *role)
{
	struct mango_dev *hw, *new_hw = NULL;
	struct mango_dev_cand *promoted;
	const char *id;
	char role_buf[8];
	bool created = false;

	if (!dev)
		return -EINVAL;
	id = dev_name(dev);
	if (!role) {
		mango_dev_mmio_role(dev, role_buf, sizeof(role_buf));
		role = role_buf;
	}

	mutex_lock(&mango_dev_lock);
	promoted = mango_dev_cand_unlink_locked(id);

	/* All SoC MMIO mango devices share one hw (they are one unit). */
	hw = mango_dev_find_locked(
		0, MANGO_DEV_MMIO_HWID); /* MMIO: single soc hw, domain 0 */
	if (!hw) {
		new_hw = kzalloc(sizeof(*new_hw), GFP_KERNEL);
		if (!new_hw) {
			mutex_unlock(&mango_dev_lock);
			mango_dev_cand_free(promoted);
			return -ENOMEM;
		}
		new_hw->hw_id = MANGO_DEV_MMIO_HWID;
		new_hw->xport = MANGO_DEV_XPORT_MMIO;
		mutex_init(&new_hw->reset_lock);
		mutex_init(&new_hw->fence_lock);
		INIT_LIST_HEAD(&new_hw->funcs);
		list_add_tail(&new_hw->node, &mango_dev_list);
		hw = new_hw;
		created = true;
	}
	/* No bridge/secondary bus on MMIO; no bus-based sibling absorption. */
	mango_dev_func_add_locked(hw, id, role, NULL, 0);
	mutex_unlock(&mango_dev_lock);

	if (created) {
		hw->dev = device_create_with_groups(mango_dev_class, NULL,
						    MKDEV(0, 0), hw,
						    mango_dev_dev_groups, "%s",
						    MANGO_DEV_MMIO_NODE);
		if (IS_ERR(hw->dev)) {
			pr_err("mango_dev: node %s: sysfs create failed (%ld); reset/members unavailable\n",
			       MANGO_DEV_MMIO_NODE, PTR_ERR(hw->dev));
			hw->dev = NULL;
		}
	}
	mango_dev_cand_free(promoted);
	return 0;
}
EXPORT_SYMBOL_GPL(mango_dev_register_mmio);

void mango_dev_unregister_mmio(struct device *dev)
{
	struct mango_dev *hw;
	struct mango_dev_func *f;

	if (!dev)
		return;
	mutex_lock(&mango_dev_lock);
	list_for_each_entry (hw, &mango_dev_list, node)
		list_for_each_entry (f, &hw->funcs, node)
			if (!strcmp(f->id, dev_name(dev)))
				mango_dev_set_present(hw, f, false);
	mutex_unlock(&mango_dev_lock);
}
EXPORT_SYMBOL_GPL(mango_dev_unregister_mmio);

/* Track a mango platform device as an MMIO candidate (grouped into the soc hw
 * on registration). Simpler than the PCIe path: no bridge, no bus absorption. */
static void __mango_dev_cand_add_mmio(struct device *dev)
{
	const char *id = dev_name(dev);
	struct mango_dev_cand *c;

	mutex_lock(&mango_dev_lock);
	if (mango_dev_func_mark_present_locked(id) ||
	    mango_dev_cand_find_locked(id)) {
		mutex_unlock(&mango_dev_lock);
		return;
	}
	mutex_unlock(&mango_dev_lock);

	/* Create before publishing (see mango_dev_cand_add): off-list, the create
	 * cannot race register()'s free, and it must not run under the lock. */
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return;
	c->xport = MANGO_DEV_XPORT_MMIO;
	strscpy(c->id, id, sizeof(c->id));
	c->dev =
		device_create_with_groups(mango_dev_class, NULL, MKDEV(0, 0), c,
					  mango_dev_cand_groups, "%s", c->id);
	if (IS_ERR(c->dev))
		c->dev = NULL;

	mutex_lock(&mango_dev_lock);
	if (mango_dev_func_mark_present_locked(id) ||
	    mango_dev_cand_find_locked(id)) {
		mutex_unlock(&mango_dev_lock);
		mango_dev_cand_free(c);
		return;
	}
	list_add_tail(&c->node, &mango_dev_cand_list);
	mutex_unlock(&mango_dev_lock);
}

static void mango_dev_cand_add_mmio(struct device *dev)
{
	mutex_lock(&mango_dev_discover_lock);
	__mango_dev_cand_add_mmio(dev);
	mutex_unlock(&mango_dev_discover_lock);
}

void mango_dev_unregister(struct pci_dev *pdev)
{
	struct mango_dev *hw;
	struct mango_dev_func *f;
	char bdf[MANGO_DEV_BDF_LEN];

	if (!pdev)
		return;
	mango_dev_fmt_bdf(pdev, bdf, sizeof(bdf));

	/* Mark absent; retain the hw + slot-anchor for recovery. */
	mutex_lock(&mango_dev_lock);
	list_for_each_entry (hw, &mango_dev_list, node)
		list_for_each_entry (f, &hw->funcs, node)
			if (!strcmp(f->id, bdf))
				mango_dev_set_present(hw, f, false);
	mutex_unlock(&mango_dev_lock);
}
EXPORT_SYMBOL_GPL(mango_dev_unregister);

void mango_dev_set_reset_ops(const struct mango_dev_reset_ops *ops)
{
	WRITE_ONCE(mango_dev_reset_ops, ops);
	/* On teardown (NULL), wait for in-flight reset dispatches to finish so
	 * the caller (mango_core) can free the ops' text safely. */
	if (!ops)
		synchronize_srcu(&mango_dev_ops_srcu);
}
EXPORT_SYMBOL_GPL(mango_dev_set_reset_ops);

/* ---- PCI bus notifier: candidate discovery ------------------------------ */
static int mango_dev_pci_notify(struct notifier_block *nb, unsigned long action,
				void *data)
{
	struct device *dev = data;
	struct pci_dev *pdev;

	if (!dev_is_pci(dev))
		return NOTIFY_DONE;
	pdev = to_pci_dev(dev);
	if (pdev->vendor != MANGO_DEV_VENDOR_ID)
		return NOTIFY_DONE;
	/* VFs are not independent unit functions: they share the PF's bus and
	 * are torn down with it, so tracking them as candidates/members would
	 * wrongly claim a bus and pull SR-IOV children into the unit's bus reset.
	 * The PF alone represents the hw for reset purposes. */
	if (pdev->is_virtfn)
		return NOTIFY_DONE;

	switch (action) {
	case BUS_NOTIFY_ADD_DEVICE:
		mango_dev_cand_add(pdev);
		break;
	case BUS_NOTIFY_DEL_DEVICE:
		/* Members are retained (marked absent via unregister); only a
		 * never-promoted candidate is dropped here. */
		mango_dev_cand_remove(pdev);
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block mango_dev_pci_nb = {
	.notifier_call = mango_dev_pci_notify,
};

/* ---- platform bus notifier: MMIO candidate discovery (platform SoC) ---- */
static int mango_dev_plt_notify(struct notifier_block *nb, unsigned long action,
				void *data)
{
	struct device *dev = data;

	if (!mango_dev_dev_is_mango_plt(dev))
		return NOTIFY_DONE;

	switch (action) {
	case BUS_NOTIFY_ADD_DEVICE:
		mango_dev_cand_add_mmio(dev);
		break;
	case BUS_NOTIFY_DEL_DEVICE:
		mango_dev_cand_remove_id(dev_name(dev));
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block mango_dev_plt_nb = {
	.notifier_call = mango_dev_plt_notify,
};

static int mango_dev_seed_plt(struct device *dev, void *unused)
{
	if (mango_dev_dev_is_mango_plt(dev))
		mango_dev_cand_add_mmio(dev);
	return 0;
}

static void mango_dev_seed(void)
{
	struct pci_dev *pdev = NULL;

	/* Catch mango functions already present when we loaded -- both buses. */
	while ((pdev = pci_get_device(MANGO_DEV_VENDOR_ID, PCI_ANY_ID, pdev)))
		mango_dev_cand_add(pdev);
	bus_for_each_dev(&platform_bus_type, NULL, NULL, mango_dev_seed_plt);
}

int mango_dev_init(const struct mango_dev_card_ops *cards)
{
	int err;

	mango_dev_cards = cards;
	mango_dev_class = mango_compat_class_create("mango_dev");
	if (IS_ERR(mango_dev_class))
		return PTR_ERR(mango_dev_class);

	err = mango_dev_health_init();
	if (err)
		goto err_class;
	err = bus_register_notifier(&pci_bus_type, &mango_dev_pci_nb);
	if (err)
		goto err_health;
	err = bus_register_notifier(&platform_bus_type, &mango_dev_plt_nb);
	if (err)
		goto err_pci_nb;

	mango_dev_seed();
	pr_info("mango_dev: presence layer loaded\n");
	return 0;

err_pci_nb:
	bus_unregister_notifier(&pci_bus_type, &mango_dev_pci_nb);
err_health:
	mango_dev_health_exit();
err_class:
	class_destroy(mango_dev_class);
	return err;
}

void mango_dev_exit(void)
{
	struct mango_dev *hw;
	struct mango_dev_func *f, *tf;
	struct mango_dev_cand *c;

	bus_unregister_notifier(&platform_bus_type, &mango_dev_plt_nb);
	bus_unregister_notifier(&pci_bus_type, &mango_dev_pci_nb);

	/* Drain both inventories: unlink one entry under the lock, then tear
	 * down its sysfs device + free it with the lock dropped
	 * (device_unregister waits for an in-flight show() that takes the lock). */
	for (;;) {
		mutex_lock(&mango_dev_lock);
		c = list_first_entry_or_null(&mango_dev_cand_list,
					     struct mango_dev_cand, node);
		if (c)
			list_del(&c->node);
		mutex_unlock(&mango_dev_lock);
		if (!c)
			break;
		mango_dev_cand_free(c);
	}

	for (;;) {
		mutex_lock(&mango_dev_lock);
		hw = list_first_entry_or_null(&mango_dev_list, struct mango_dev,
					      node);
		if (hw)
			list_del(&hw->node);
		mutex_unlock(&mango_dev_lock);
		if (!hw)
			break;
		mango_dev_health_destroy(hw);
		if (hw->dev)
			device_unregister(hw->dev);
		list_for_each_entry_safe (f, tf, &hw->funcs, node) {
			pci_dev_put(f->bridge);
			kfree(f);
		}
		kfree(hw);
	}

	mango_dev_health_exit();
	class_destroy(mango_dev_class);
	pr_info("mango_dev: presence layer unloaded\n");
}
