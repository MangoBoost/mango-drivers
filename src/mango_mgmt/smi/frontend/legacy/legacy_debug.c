// SPDX-License-Identifier: GPL-2.0
/*
 * legacy_debug.c - The card's debug facilities the legacy UAPI wraps.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include <linux/errno.h>

#include "contract/smi/smi_abi_v2.h" /* SMI_SHELL_LINE_BYTES */

#include "legacy_debug.h"
#include "legacy_prims.h"
#include "smi_top.h"

#define SMI_LEDCTL_MIN_FW_DATE 0x260928

int smi_debug_shell_exec(struct pf_dev_struct *pf_dev, uint8_t *buf,
			 uint32_t cap, uint32_t line_len, uint32_t *out_len,
			 uint32_t *dropped)
{
	uint32_t n = 0, lost = 0;
	int ret;

	/* The card's own buffer bounds the line, not the slice: the slice can
	 * be 64 KiB, and the wire carries cmd_len in a halfword. */
	if (!pf_dev || !buf || !cap || !line_len || (line_len > cap) ||
	    (line_len >= SMI_SHELL_LINE_BYTES))
		return -EINVAL;

	ret = smi_legacy_prims(pf_dev)->shell_exec(pf_dev, buf, cap, line_len,
						   &n, &lost);
	if (ret)
		return ret;

	if (out_len)
		*out_len = n;
	if (dropped)
		*dropped = lost;
	return 0;
}

int smi_debug_set_verbosity(struct pf_dev_struct *pf_dev, uint8_t level)
{
	if (!pf_dev)
		return -EINVAL;

	return smi_legacy_prims(pf_dev)->log_level(pf_dev, level);
}

int smi_debug_led(struct pf_dev_struct *pf_dev, uint8_t *payload, uint32_t len)
{
	const uint8_t *d;
	uint32_t date;

	if (!pf_dev)
		return -EINVAL;
	if (!pf_dev->facts.present)
		return -ENODEV;

	d = pf_dev->facts.version.ver_date;
	date = ((uint32_t)d[0] << 16) | ((uint32_t)d[1] << 8) | d[2];
	if (date < SMI_LEDCTL_MIN_FW_DATE)
		return -EOPNOTSUPP;

	return smi_legacy_prims(pf_dev)->led(pf_dev, payload, len);
}
