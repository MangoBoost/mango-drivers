/* SPDX-License-Identifier: GPL-2.0 */
/*
 * smi_pub_sect.h - A published section's head, as a reader takes it.
 *
 * The growth law smi_tlv_take() applies to a record, applied to the head a
 * section opens with: a head this build knows is read as far as the card
 * wrote it, and what the card did not write reads zero.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_PUB_SECT_H
#define SMI_PUB_SECT_H

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>

#include "contract/smi/smi_abi.h" /* struct smi_pub_sect */

/**
 * smi_pub_accept() - Judge a section head read into a build's own struct.
 * @head: the head, zeroed and then read as far as the section reached.
 * @cap: the struct's size.
 * @fmt: the format this build knows.
 * @baseline: the shortest head that format has.
 *
 * Return: 0, with the bytes past the card's head zeroed; -EOPNOTSUPP for a
 *         format this build was never taught or a head short of its floor.
 */
static inline int smi_pub_accept(void *head, size_t cap, uint16_t fmt,
				 uint16_t baseline)
{
	const struct smi_pub_sect *s = head;

	if ((cap < sizeof(*s)) || (s->fmt != fmt) || (s->hdr_len < baseline))
		return -EOPNOTSUPP;
	if (s->hdr_len < cap)
		memset((uint8_t *)head + s->hdr_len, 0, cap - s->hdr_len);

	return 0;
}

#endif /* SMI_PUB_SECT_H */
