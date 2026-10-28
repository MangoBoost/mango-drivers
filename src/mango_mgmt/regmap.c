// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include <linux/io.h>
#include <linux/mm.h>
#include <linux/overflow.h>

#include "hw_contract.h"
#include "regmap.h"
#include "transport/transport.h"

/* Which side drives a register; the list's last column. */
enum mango_mgmt_reg_side {
	MGMT_REG_SIDE_ANY,
	MGMT_REG_SIDE_HOST,
	MGMT_REG_SIDE_SOC,
};

/* Register placement inside its IP block, the same on every card. */
#define MGMT_X_REG_ROW(name, blk, off, side)                                   \
	[MGMT_REG_##name] = { MGMT_BLK_##blk, (off), MGMT_REG_SIDE_##side },

static const struct {
	enum mango_mgmt_block blk;
	u32 off;
	enum mango_mgmt_reg_side side;
} reg_layout[MGMT_REG_NUM] = { MANGO_MGMT_REG_LIST(MGMT_X_REG_ROW) };
#undef MGMT_X_REG_ROW

/* Block bases per card, from the hardware contract. */
#define BLK(card, block)                                                       \
	{                                                                      \
		.idx = MANGO_HW_##card##_IDX,                                  \
		.off = MANGO_HW_##card##_##block##_OFF,                        \
		.len = MANGO_HW_##card##_##block##_LEN                         \
	}

/* MPIPE and the SMI command plane share one function; the identification
 * block says whether the card wires MPIPE at all. */
const struct mango_mgmt_regmap mango_mgmt_regmap_pcie_v2 = {
	.blk = {
		[MGMT_BLK_V2_SYSMAN] = BLK(MGMT_V2, SYSMAN),
		[MGMT_BLK_MPIPE] = BLK(MGMT_V2, MPIPE),
		[MGMT_BLK_GCQ] = BLK(MGMT_V2, GCQ),
		[MGMT_BLK_GCQ_PAYLOAD] = BLK(MGMT_V2, GCQ_PAYLOAD),
	},
};

/* The SoC side carries the identification block and MPIPE, at the host
 * offsets; its SMI rides a device of its own. */
const struct mango_mgmt_regmap mango_mgmt_regmap_mmio_v2 = {
	.blk = {
		[MGMT_BLK_V2_SYSMAN] = { .idx = MANGO_HW_MMIO_IDX,
					 .off = MANGO_HW_MGMT_V2_SYSMAN_OFF,
					 .len = MANGO_HW_MGMT_V2_SYSMAN_LEN },
		[MGMT_BLK_MPIPE] = { .idx = MANGO_HW_MMIO_IDX,
				     .off = MANGO_HW_MGMT_V2_MPIPE_OFF,
				     .len = MANGO_HW_MMIO_V2_MPIPE_LEN },
	},
};

static const struct mango_mgmt_block_cfg *
mango_mgmt_regmap_blk(struct mango_mgmt_dev *mgmt, enum mango_mgmt_block blk)
{
	const struct mango_mgmt_regmap *regmap = mgmt->desc->regmap;

	if (!regmap->blk[blk].len)
		return NULL;

	return &regmap->blk[blk];
}

bool mango_mgmt_regmap_has(struct mango_mgmt_dev *mgmt,
			   enum mango_mgmt_block blk)
{
	return mango_mgmt_regmap_blk(mgmt, blk) != NULL;
}

unsigned long
mango_mgmt_regmap_region_mask(const struct mango_mgmt_regmap *regmap)
{
	unsigned long mask = 0;
	int i;

	for (i = 0; i < MGMT_BLK_NUM; i++)
		if (regmap->blk[i].len)
			mask |= BIT(regmap->blk[i].idx);

	return mask;
}

/* The MPIPE bank this side drives; the other bank may lie outside its
 * block (the host maps only its own bank). */
static bool mango_mgmt_regmap_reg_used(struct mango_mgmt_dev *mgmt,
				       enum mango_mgmt_reg reg)
{
	bool host = mgmt->desc->side == MGMT_SIDE_HOST;

	switch (reg_layout[reg].side) {
	case MGMT_REG_SIDE_HOST:
		return host;
	case MGMT_REG_SIDE_SOC:
		return !host;
	default:
		return true;
	}
}

int mango_mgmt_regmap_validate(struct mango_mgmt_dev *mgmt)
{
	const struct mango_mgmt_regmap *regmap = mgmt->desc->regmap;
	struct mango_mgmt_region region;
	int i;

	for (i = 0; i < MGMT_BLK_NUM; i++) {
		const struct mango_mgmt_block_cfg *blk = &regmap->blk[i];

		if (!blk->len)
			continue;
		if (mgmt->transport->region_get(mgmt, blk->idx, &region) ||
		    blk->off > region.size ||
		    blk->len > region.size - blk->off) {
			dev_err(mgmt->dev,
				"block %d [0x%llx, +0x%llx) exceeds region %u",
				i, blk->off, blk->len, blk->idx);
			return -ERANGE;
		}
	}

	/* Every register this side reads sits inside its block. */
	for (i = 0; i < MGMT_REG_NUM; i++) {
		const struct mango_mgmt_block_cfg *blk =
			&regmap->blk[reg_layout[i].blk];

		if (!blk->len || !mango_mgmt_regmap_reg_used(mgmt, i))
			continue;
		if (reg_layout[i].off + sizeof(u32) > blk->len) {
			dev_err(mgmt->dev,
				"register %d at +0x%x exceeds block %d (0x%llx)",
				i, reg_layout[i].off, reg_layout[i].blk,
				blk->len);
			return -ERANGE;
		}
	}

	return 0;
}

/* Block base + IP-local offset, bounds-checked against the mapped region. */
static void __iomem *mango_mgmt_regmap_addr(struct mango_mgmt_dev *mgmt,
					    enum mango_mgmt_reg reg,
					    u64 extra_off)
{
	const struct mango_mgmt_block_cfg *blk;
	struct mango_mgmt_region region;
	u64 off;

	if (reg >= MGMT_REG_NUM)
		return NULL;

	blk = mango_mgmt_regmap_blk(mgmt, reg_layout[reg].blk);
	if (!blk)
		return NULL;

	if (check_add_overflow(blk->off + reg_layout[reg].off, extra_off,
			       &off) ||
	    mgmt->transport->region_get(mgmt, blk->idx, &region) ||
	    off > region.size - sizeof(u32)) {
		dev_warn(mgmt->dev, "invalid mmio access: region%u 0x%llx",
			 blk->idx, off);
		return NULL;
	}

	return region.base + off;
}

void mango_mgmt_regmap_write_offset(struct mango_mgmt_dev *mgmt,
				    enum mango_mgmt_reg reg, u64 extra_off,
				    u32 val)
{
	void __iomem *addr = mango_mgmt_regmap_addr(mgmt, reg, extra_off);

	if (addr)
		writel(val, addr);
}

u32 mango_mgmt_regmap_read_offset(struct mango_mgmt_dev *mgmt,
				  enum mango_mgmt_reg reg, u64 extra_off)
{
	void __iomem *addr = mango_mgmt_regmap_addr(mgmt, reg, extra_off);

	return addr ? readl(addr) : VALUE_UNDEFINED;
}

void mango_mgmt_regmap_write(struct mango_mgmt_dev *mgmt,
			     enum mango_mgmt_reg reg, u32 val)
{
	mango_mgmt_regmap_write_offset(mgmt, reg, 0, val);
}

u32 mango_mgmt_regmap_read(struct mango_mgmt_dev *mgmt, enum mango_mgmt_reg reg)
{
	return mango_mgmt_regmap_read_offset(mgmt, reg, 0);
}

int mango_mgmt_regmap_block(struct mango_mgmt_dev *mgmt,
			    enum mango_mgmt_block blk, void __iomem **base,
			    u64 *len)
{
	const struct mango_mgmt_block_cfg *cfg =
		mango_mgmt_regmap_blk(mgmt, blk);
	struct mango_mgmt_region region;

	if (!cfg || mgmt->transport->region_get(mgmt, cfg->idx, &region))
		return -ENODEV;

	*base = region.base + cfg->off;
	*len = cfg->len;
	return 0;
}

int mango_mgmt_regmap_mpipe_phys(struct mango_mgmt_dev *mgmt, phys_addr_t *phys,
				 size_t *size)
{
	const struct mango_mgmt_block_cfg *blk;
	struct mango_mgmt_region region;

	blk = mango_mgmt_regmap_blk(mgmt, MGMT_BLK_MPIPE);
	if (!blk || mgmt->transport->region_get(mgmt, blk->idx, &region))
		return -ENODEV;

	/* Pages are the mmap unit: a block that is not whole pages would hand
	 * userspace the neighbouring windows too. */
	if (offset_in_page(blk->off) || blk->len != PAGE_ALIGN(blk->len)) {
		dev_warn(
			mgmt->dev,
			"MPIPE block +0x%llx/0x%llx is not whole 0x%lx pages; no raw mmap",
			blk->off, blk->len, PAGE_SIZE);
		return -EOPNOTSUPP;
	}

	*phys = region.phys + blk->off;
	*size = blk->len;
	return 0;
}
