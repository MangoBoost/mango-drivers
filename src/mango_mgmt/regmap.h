// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MGMT_REGMAP_H_
#define _MGMT_REGMAP_H_

#include "dev.h"

#define VALUE_UNDEFINED (0xFFFFFFFF)
#define IS_VALUE_DEFINED(val) ((val) != VALUE_UNDEFINED && (val) != 0xdeadbeef)

#define MPIPE_STS_FILL_LEVEL_MASK GENMASK(8, 0)
#define MPIPE_STS_MAX_ENT_SHIFT (16)
#define MPIPE_STS_MAX_ENT_MASK GENMASK(24, 16)
#define MPIPE_CTRL_LOW_WM_MASK GENMASK(8, 0)
#define MPIPE_CTRL_HIGH_WM_SHIFT (9)
#define MPIPE_CTRL_HIGH_WM_MASK GENMASK(17, 9)

/* Every register the driver touches: name, block, offset inside the block,
 * and the side that drives it. One list feeds both the enum and the layout
 * table, so a register added in the middle cannot land in the wrong side. */
#define MANGO_MGMT_REG_LIST(X)                                                 \
	X(HOST_H2C_TX_DATA, MPIPE, 0x0000, HOST)                               \
	X(HOST_C2H_RX_DATA, MPIPE, 0x0004, HOST)                               \
	X(HOST_H2C_STS, MPIPE, 0x0008, HOST)                                   \
	X(HOST_H2C_CTRL, MPIPE, 0x000C, HOST)                                  \
	X(HOST_C2H_STS, MPIPE, 0x0010, HOST)                                   \
	X(HOST_C2H_CTRL, MPIPE, 0x0014, HOST)                                  \
	X(SOC_C2H_TX_DATA, MPIPE, 0x1000, SOC)                                 \
	X(SOC_H2C_RX_DATA, MPIPE, 0x1004, SOC)                                 \
	X(SOC_C2H_STS, MPIPE, 0x1008, SOC)                                     \
	X(SOC_C2H_CTRL, MPIPE, 0x100C, SOC)                                    \
	X(SOC_H2C_STS, MPIPE, 0x1010, SOC)                                     \
	X(SOC_H2C_CTRL, MPIPE, 0x1014, SOC)

#define MGMT_X_REG_ENUM(name, blk, off, side) MGMT_REG_##name,
enum mango_mgmt_reg { MANGO_MGMT_REG_LIST(MGMT_X_REG_ENUM) MGMT_REG_NUM };
#undef MGMT_X_REG_ENUM

/* IP blocks. A card's regmap places each block; the register offsets inside a
 * block are the same on every card (regmap.c). */
enum mango_mgmt_block {
	MGMT_BLK_MPIPE,
	MGMT_BLK_GCQ,
	MGMT_BLK_GCQ_PAYLOAD,
	MGMT_BLK_NUM,
};

struct mango_mgmt_block_cfg {
	u32 idx; /* PCIe BAR or MMIO region */
	u64 off;
	u64 len; /* 0: this card has no such block */
};

struct mango_mgmt_regmap {
	struct mango_mgmt_block_cfg blk[MGMT_BLK_NUM];
};

extern const struct mango_mgmt_regmap mango_mgmt_regmap_pcie_v2;
extern const struct mango_mgmt_regmap mango_mgmt_regmap_mmio_v2;

void mango_mgmt_regmap_write_offset(struct mango_mgmt_dev *,
				    enum mango_mgmt_reg, u64 extra_off,
				    u32 val);
u32 mango_mgmt_regmap_read_offset(struct mango_mgmt_dev *, enum mango_mgmt_reg,
				  u64 extra_off);

void mango_mgmt_regmap_write(struct mango_mgmt_dev *, enum mango_mgmt_reg,
			     u32 val);
u32 mango_mgmt_regmap_read(struct mango_mgmt_dev *, enum mango_mgmt_reg);

bool mango_mgmt_regmap_has(struct mango_mgmt_dev *, enum mango_mgmt_block);

/* Regions the card's blocks live in, one bit per index. */
unsigned long mango_mgmt_regmap_region_mask(const struct mango_mgmt_regmap *);

/* Every present block fits its mapped region and every register this side
 * uses fits its block; -ERANGE otherwise. */
int mango_mgmt_regmap_validate(struct mango_mgmt_dev *);

/* Mapped base and length of a block; -ENODEV if the card has none. */
int mango_mgmt_regmap_block(struct mango_mgmt_dev *, enum mango_mgmt_block,
			    void __iomem **base, u64 *len);

/* Physical MPIPE block for the cdev mmap. -ENODEV if absent, -EOPNOTSUPP
 * when the block is not whole pages (the mmap would leak its neighbours). */
int mango_mgmt_regmap_mpipe_phys(struct mango_mgmt_dev *, phys_addr_t *phys,
				 size_t *size);

#endif
