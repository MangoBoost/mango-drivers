// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_DEV_H_
#define _MANGO_MGMT_DEV_H_

#include <linux/device.h>
#include <linux/cdev.h>

#define MANGO_MGMT_NAME "mango_mgmt"

struct mango_mgmt_transport;
struct mango_mgmt_monitor;
struct mango_mgmt_regmap;
struct pf_dev_struct;
struct mango_mgmt_mpipe_hw_device;
struct mango_mgmt_cdev;
struct mango_mgmt_virtio_dev;
struct net_device;

enum mango_mgmt_side {
	MGMT_SIDE_HOST = 0,
	MGMT_SIDE_SOC,
};

/* Whether the function carries the SMI command plane, and over what. */
enum mango_mgmt_smi_link {
	MGMT_SMI_NONE = 0,
	MGMT_SMI_GCQ,
};

/* How MPIPE is woken: the best source the function is known to wire.
 * irq_setup falls back from it (interrupt -> polling); NONE is polling. */
enum mango_mgmt_irq_mode {
	MANGO_MGMT_IRQ_MODE_NONE = 0,
	MANGO_MGMT_IRQ_MODE_LEGACY = 2, /* 1 was MSI-X */
	MANGO_MGMT_IRQ_MODE_POLLING,
};

/* What one device ID (or OF compatible) carries; bound as its match data. */
struct mango_mgmt_desc {
	const struct mango_mgmt_regmap *regmap;
	enum mango_mgmt_side side;
	enum mango_mgmt_smi_link smi;
	enum mango_mgmt_irq_mode irq_mode; /* unless the module parameter says */
	const char *role; /* mango_dev member role */
};

/* Module parameters (defined in mod.c) */
extern int mango_mgmt_irq_mode;
extern unsigned short mango_mgmt_rpc_port;
extern unsigned short mango_mgmt_term_port;

/* What the identification block said at probe; read once, immutable after. */
struct mango_mgmt_ident {
	u32 version;
	u32 configuration; /* MGMT_V2_CFG_* bits */
};

/* The MGMT component: the surface and what rides on it, brought up and torn
 * down as one by frontend.c. devid, ready and netdev are three different
 * facts and stay separate; each field is the allocating subsystem's. */
struct mango_mgmt_service {
	/* The node outlives this device: an open fd pins the wrapper, whose
	 * back-pointer is cleared at teardown. NULL when there is no surface. */
	struct mango_mgmt_cdev *cdev;
	int devid; /* class device index; -1 when there is no surface */
	atomic_t ready; /* MPIPE, RPC API and terminal are registered */

	struct mango_mgmt_mpipe_hw_device *hw;
	struct net_device *netdev;
	struct mango_mgmt_virtio_dev *term; /* SoC server only */
};

/* Per-device management context -- one per PCIe function or platform device.
 * Allocated by dev.c at probe, freed at remove. Carries what the function
 * shares plus the two components it can present. */
struct mango_mgmt_dev {
	struct device *dev; /* parent device (PCI or platform) */
	const struct mango_mgmt_desc *desc;

	struct mango_mgmt_ident ident;

	struct mango_mgmt_transport *transport; /* PCIe or MMIO backend */
	struct mango_mgmt_monitor *monitor; /* hardware monitoring context */

	struct mango_mgmt_service mgmt; /* embedded; no separate lifetime */
	struct pf_dev_struct *smi; /* SMI command plane; NULL without one */
};

struct mango_mgmt_dev *mango_mgmt_dev_alloc(struct device *,
					    struct mango_mgmt_transport *,
					    const struct mango_mgmt_desc *);
void mango_mgmt_dev_free(struct mango_mgmt_dev *);

/* Whether the card wires an MPIPE FIFO: the block is mapped and
 * CONFIGURATION.HAS_MPIPE says it was built in. */
bool mango_mgmt_dev_has_mpipe(struct mango_mgmt_dev *);

/* Resolve a management HW address to its device (intra-module helper,
 * defined in api/api.c). Returns NULL if no device matches. */
struct mango_mgmt_dev *mango_mgmt_dev_by_addr(const char *hw_addr);

#endif
