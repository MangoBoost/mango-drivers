/* SPDX-License-Identifier: GPL-2.0 */
/**
 * @file legacy_qsfp.h
 * @brief The card's QSFP cage blocks, as this driver keeps them.
 *
 * One block per cage, published in the window this driver maps: the shape is
 * settled at attach, and a cage file reads a block through it. A cage request
 * is named here, whatever wire carries it.
 *
 * Copyright (c) 2026 MangoBoost Inc.
 */
#ifndef SMI_LEGACY_QSFP_H
#define SMI_LEGACY_QSFP_H

#include <linux/types.h>

#include "legacy_prims.h" /* struct smi_qsfp_rw_arg */

struct pf_dev_struct;

/* The most cage sections this driver keeps: a link record names its cage in
 * four bits, so a card has no use for more. */
#define SMI_PUB_CAGES_MAX (16)

/**
 * struct smi_qsfp_store - the published cage plane of one card
 * @len: each cage's section length, as its Region record states it; also what
 *	 one attribute serves
 * @cnt: how many the card publishes; 0 means this card seats none
 * @decoded: whether this build knows the block format cage 0 carried at attach
 */
struct smi_qsfp_store {
	uint32_t len[SMI_PUB_CAGES_MAX];
	uint8_t cnt;
	bool decoded;
};

/* Whether a cage's section can back a file: from the header baseline to
 * SMI_QSFP_BLK_MAX, so one read serves it. */
bool smi_qsfp_len_ok(uint32_t len);

/* Settle each cage's section from the records and whether cage 0's format
 * decodes; no cage, or a failure logged here once, leaves the store empty. */
void smi_qsfp_attach(struct pf_dev_struct *pf_dev);
/* Forget the plane: a cage file answers -ENODEV from here on. */
void smi_qsfp_detach(struct pf_dev_struct *pf_dev);

/* @len bytes at @at of one cage's block, within its section, under the block's
 * own seq. -ENODEV once the plane is detached. */
int smi_qsfp_block(struct pf_dev_struct *pf_dev, uint8_t cage, uint32_t at,
		   void *buf, size_t len);

/**
 * smi_qsfp_rw() - one window of one cage's map, on behalf of a caller.
 * @pf_dev: the card
 * @arg: which cage, which bytes, and the generations the caller decided against
 * @buf: the window, @arg->len bytes: sent for every op, and overwritten where
 *       the answer fills it
 * @val: [out] the answer's val, as smi_ioc_qsfp_payload states it
 * @used: [out] the bytes the answer wrote into @buf: @arg->len or 0
 *
 * Nothing here judges the request beyond its shape: state, the two generations
 * and write permission are the card's, which is where the module actually is.
 *
 * Return: 0, or a negative errno: the card's status mapped, or the driver's
 * own when the request never reached it (smi_ioctl.h lists both).
 */
int smi_qsfp_rw(struct pf_dev_struct *pf_dev, const struct smi_qsfp_rw_arg *arg,
		uint8_t *buf, uint32_t *val, uint32_t *used);

/* QSFP module access for a caller holding a counted device; both check that
 * the SMC session is up. @page and @offset name the byte inside the module. */
int smi_pf_dev_module_read(struct pf_dev_struct *pf_dev, uint8_t device_id,
			   uint8_t page, uint8_t offset, uint8_t *buf,
			   uint8_t buf_len);
int smi_pf_dev_module_write(struct pf_dev_struct *pf_dev, uint8_t device_id,
			    uint8_t page, uint8_t offset, uint8_t *buf,
			    uint8_t buf_len);

/* The contract's word for a state, an op or a power policy; a value this build
 * has no word for prints as its number. */
const char *smi_qsfp_state_word(uint8_t state, char *tmp, size_t n);
const char *smi_qsfp_op_word(uint8_t op, char *tmp, size_t n);
const char *smi_qsfp_power_word(uint8_t policy, char *tmp, size_t n);

#endif /* SMI_LEGACY_QSFP_H */
