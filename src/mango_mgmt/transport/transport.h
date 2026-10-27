// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - Transport layer
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_TRANSPORT_H_
#define _MANGO_MGMT_TRANSPORT_H_

#include <linux/types.h>

#include "dev.h"

/* One mapped PCIe BAR or MMIO region. */
struct mango_mgmt_region {
	void __iomem *base;
	phys_addr_t phys;
	size_t size;
};

struct mango_mgmt_transport {
	struct mango_mgmt_dev *mgmt;

	/* transport-specific data */
	void *data;

	/* transport-specific callbacks */
	int (*init)(struct mango_mgmt_dev *);
	void (*fini)(struct mango_mgmt_dev *);
	/* Region @idx as mapped at init; -ENODEV when the card has none. */
	int (*region_get)(struct mango_mgmt_dev *, u32 idx,
			  struct mango_mgmt_region *);
	/* MPIPE IRQ sources, called by mpipe around its HW context: the
	 * handlers read it. setup returns how many are in service, 0 when it
	 * got none, and the caller polls. */
	int (*irq_setup)(struct mango_mgmt_dev *);
	void (*irq_teardown)(struct mango_mgmt_dev *);
	const char *(*get_mgmt_addr)(struct mango_mgmt_dev *);
	const u8 *(*get_mgmt_addr_bin)(struct mango_mgmt_dev *, int *len);
};

int mango_mgmt_transport_register(void);
void mango_mgmt_transport_unregister(void);

bool mango_mgmt_transport_is_soc_device(struct mango_mgmt_dev *mgmt_dev);

#endif
