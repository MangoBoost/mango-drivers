/* SPDX-License-Identifier: GPL-2.0 */
/*
 * smi_boot_v2.h - The SMID dialect's registry row.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved. */

#ifndef SMI_BOOT_V2_H
#define SMI_BOOT_V2_H

#include "smi_boot.h"

struct device;

extern const struct smi_boot_dialect smi_boot_dialect_v2;

/**
 * smi_boot_v2_verbs() - The Verb records and the lane Limits of one chain.
 * @dev: where a chain this host cannot trust is logged.
 * @recs: the descriptor's records, copied out of the window.
 * @len: bytes of @recs.
 * @out: receives the verbs and the limits; what the chain does not state
 *       reads zero, and a chain that cannot be trusted states nothing.
 */
void smi_boot_v2_verbs(struct device *dev, const uint8_t *recs, int len,
		       struct smi_layout *out);

/**
 * smi_boot_v2_pub() - The published sections one chain states, and whose boot.
 * @dev: where a chain this host cannot trust is logged.
 * @recs: the descriptor's records, copied out of the window.
 * @len: bytes of @recs.
 * @out: receives each section's place; a chain that states no boot publishes
 *       nothing this host can judge, and leaves @out empty.
 */
void smi_boot_v2_pub(struct device *dev, const uint8_t *recs, int len,
		     struct smi_pub_layout *out);

/**
 * smi_boot_v2_tool_copy() - The records of one chain a tool reads.
 * @recs: the descriptor's records, copied out of the window.
 * @len: bytes of @recs.
 * @out: receives the kept records whole, in chain order; @len bytes suffice.
 *
 * Return: the bytes written, or -EPROTO for a tail that is no whole record.
 *	   The carve, the map and the log stay behind.
 */
int smi_boot_v2_tool_copy(const uint8_t *recs, int len, uint8_t *out);

#endif /* SMI_BOOT_V2_H */
