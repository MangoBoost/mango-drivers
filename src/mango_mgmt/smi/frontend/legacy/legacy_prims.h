/* SPDX-License-Identifier: GPL-2.0 */
/*
 * legacy_prims.h - What the legacy UAPI asks a card.
 *
 * The set speaks the card's own verbs through the boundary.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_LEGACY_PRIMS_H
#define SMI_LEGACY_PRIMS_H

#include <linux/types.h>

struct pf_dev_struct;

/* QSFP_RW's request; each field means what struct smi_ioc_qsfp_payload's
 * does. */
struct smi_qsfp_rw_arg {
	uint32_t epoch;
	uint16_t control_gen;
	uint8_t cage;
	uint8_t bank;
	uint8_t page;
	uint8_t offset;
	uint8_t len;
	uint8_t op;
};

/**
 * struct smi_legacy_prims - The card's answers to the legacy UAPI, the flash
 *			     verbs aside (the burn service's own).
 * @qsfp_rw: one window of one cage at @buf; @val takes the answer's val, a
 *	     refusal's too, and @used the bytes written back, @arg->len or none.
 * @shell_exec: a line at @buf[0, @line_len), answered over it in at most @cap
 *		bytes: @out_len what came back, @dropped what did not fit.
 * @log_level: how much the firmware logs.
 * @led: the LED request, @len bytes as the card takes them.
 * @module_rw: one module read or write; cards reach their cages through
 *	       @qsfp_rw instead.
 */
struct smi_legacy_prims {
	int (*qsfp_rw)(struct pf_dev_struct *pf_dev,
		       const struct smi_qsfp_rw_arg *arg, uint8_t *buf,
		       uint32_t *val, uint32_t *used);
	int (*shell_exec)(struct pf_dev_struct *pf_dev, uint8_t *buf,
			  uint32_t cap, uint32_t line_len, uint32_t *out_len,
			  uint32_t *dropped);
	int (*log_level)(struct pf_dev_struct *pf_dev, uint8_t level);
	int (*led)(struct pf_dev_struct *pf_dev, const uint8_t *payload,
		   uint32_t len);
	int (*module_rw)(struct pf_dev_struct *pf_dev, bool write,
			 uint8_t device_id, uint8_t page, uint8_t offset,
			 uint8_t *buf, uint8_t len);
};

extern const struct smi_legacy_prims smi_legacy_v2_prims;

/* The set this card's settled wire takes. */
const struct smi_legacy_prims *smi_legacy_prims(struct pf_dev_struct *pf_dev);

#endif /* SMI_LEGACY_PRIMS_H */
