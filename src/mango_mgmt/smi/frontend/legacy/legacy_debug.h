/* SPDX-License-Identifier: GPL-2.0 */
/*
 * legacy_debug.h - The card's debug facilities the legacy UAPI wraps.
 *
 * A shell line, the firmware's log verbosity and the LEDs. Each is a verb the
 * card understands; naming one is this layer's business, not the ioctl's.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_LEGACY_DEBUG_H
#define SMI_LEGACY_DEBUG_H

#include <linux/types.h>

struct pf_dev_struct;

/**
 * smi_debug_shell_exec() - Run one debug-shell line on the card.
 * @buf: holds the line at [0, line_len) on entry, the answer on exit.
 * @cap: bytes @buf holds -- also the slice the card is asked for.
 * @out_len: receives the answer's byte count.
 * @dropped: receives what did not fit, so a caller that wants all of it
 *	     resubmits at @out_len + @dropped.
 *
 * Return: 0, or an errno. -ENODEV when the card has no session.
 */
int smi_debug_shell_exec(struct pf_dev_struct *pf_dev, uint8_t *buf,
			 uint32_t cap, uint32_t line_len, uint32_t *out_len,
			 uint32_t *dropped);

/**
 * smi_debug_set_verbosity() - Set how much the firmware logs.
 *
 * Return: 0 or an errno.
 */
int smi_debug_set_verbosity(struct pf_dev_struct *pf_dev, uint8_t level);

/**
 * smi_debug_led() - Drive the card's LEDs: the locate beacon, or force one LED.
 * @payload: the LED request, passed to the card as is.
 * @len: its length.
 *
 * Return: 0 once the card has applied the change, or an errno. -ENODEV when
 * the card has no session, -EOPNOTSUPP when its firmware predates the verb.
 */
int smi_debug_led(struct pf_dev_struct *pf_dev, uint8_t *payload, uint32_t len);

#endif /* SMI_LEGACY_DEBUG_H */
