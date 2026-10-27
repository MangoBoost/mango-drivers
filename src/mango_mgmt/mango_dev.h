/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mango_dev — thin, mango_core-independent HW-presence layer.
 *
 * A "mango hardware unit" (hw) is the set of PCIe functions that belong to one
 * physical mango part, identified by the shared HW_ID. This module owns the
 * inventory (candidates + members), exposed at /sys/class/mango_dev/, so
 * userspace never has to walk /sys/bus/pci.
 *
 * mango_core is the registrar: it calls mango_dev_register() once a function has
 * worked as a mango-device (bound + HW_ID read), and mango_dev_unregister() from
 * its .remove. This is the only coupling — one-way, thin; the layer never
 * touches regmaps or the PCIe tree for members.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */
#ifndef MANGO_DEV_H
#define MANGO_DEV_H

#include <linux/pci.h>

/*
 * Register @pdev as a member function of its hw. @hw_id groups same-hw
 * functions (mango_core passes its cached hw_id; 0 == unresolved, treated as a
 * singleton). @role is a short label ("data", "mgmt", "smi"). Idempotent.
 * A prior candidate for @pdev (if any) is promoted. Returns 0 or -errno.
 */
int mango_dev_register(struct pci_dev *pdev, u32 hw_id, const char *role);

/*
 * Mark @pdev's function absent. The hw and its slot-anchor (bridge) are
 * retained for recovery (a removed endpoint must be re-enumerable through the
 * same bridge). They are freed only at module unload; there is no per-slot
 * reaping.
 */
void mango_dev_unregister(struct pci_dev *pdev);

/**
 * struct mango_dev_health_detail - decoded context for one health report
 * @source: label for what raised the fault, NULL when the reporter has none.
 * @src_aux: label for the aux that raised it, NULL when not aux-sourced.
 * @code: source-specific status word, such as a PCIe AER status.
 * @has_code: @code carries a value. A zero status is meaningful.
 *
 * The labels arrive already decoded. This layer sits below mango_core and
 * cannot include its headers to decode the enums itself. Both strings are
 * copied on report. The reporting module may unload before anyone reads them.
 */
struct mango_dev_health_detail {
	const char *source;
	const char *src_aux;
	u64 code;
	bool has_code;
};

/*
 * Health reporting. mango_core detects and latches faults. The only remedy
 * the mango datapath engine responds to is a full PCIe re-enumeration, which
 * only this layer can drive. A report on slot @domain:@bus reaches the unit's
 * devlink fatal reporter, which is what escalates. With devlink auto_recover
 * on (the default from v5.6) that becomes an autonomous remove+rescan of the
 * WHOLE unit -- every function, both ports. Report only a fault worth that.
 *
 * Callable from process context only. A no-op when the slot has no registered
 * unit, or when the unit has no devlink instance.
 */

void mango_dev_report_health(u16 domain, u8 bus, const char *reason,
			     const struct mango_dev_health_detail *detail);

/*
 * Reset fence. A caller holds one around work a reset must not cut; while any
 * is held, or the card behind a member publishes Busy, the unit refuses every
 * reset, and a recovery it holds back runs once it clears.
 */
struct mango_dev;
struct mango_dev *mango_dev_reset_fence(struct device *dev);
void mango_dev_reset_unfence(struct mango_dev *hw);

/* The host module's word on a member's card, asked before every reset: whether
 * the card behind @dev publishes Busy. */
struct mango_dev_card_ops {
	bool (*card_busy)(struct device *dev);
};

/*
 * MMIO/platform members (platform-bus SoC). A mango device on such a SoC is a
 * platform device (a "mangoboost,*" DT node), not a PCIe function: there is no
 * BDF, no bridge and no HW_ID register. By construction every mango device the
 * SoC sees is on the one unit it lives on, so they all group into a single
 * "soc" hw node. @role is a short label; pass NULL to derive it from the
 * device's compatible (e.g. "net"). recover/reset are host/PCIe-only and are
 * not exposed on the soc node. Idempotent.
 */
int mango_dev_register_mmio(struct device *dev, const char *role);
void mango_dev_unregister_mmio(struct device *dev);

/*
 * mango_dev -> mango_core delegate. mango_dev is the lower module (mango_core
 * depends on it), so it cannot call mango_core directly; mango_core registers
 * this op and mango_dev invokes it after a remove-mode bus reset has fenced DMA.
 */
struct mango_dev_reset_ops {
	/* Reclaim F_DMA buffers quarantined on the slot @domain:@bus once a
	 * remove-mode bus reset has fenced DMA (the owning core is already gone,
	 * so the by-core release cannot match). Optional. */
	void (*reclaim_fenced)(u16 domain, u8 bus);
};
void mango_dev_set_reset_ops(const struct mango_dev_reset_ops *ops);

/*
 * Presence-layer lifecycle, driven from mango_mgmt's module init/exit (this
 * layer is hosted in mango-mgmt.ko, not a module of its own): create the
 * /sys/class/mango_dev/ class + bus notifiers, and tear them down. @cards is
 * the host's word on its members' cards; NULL asks no card.
 */
int mango_dev_init(const struct mango_dev_card_ops *cards);
void mango_dev_exit(void);

#endif /* MANGO_DEV_H */
