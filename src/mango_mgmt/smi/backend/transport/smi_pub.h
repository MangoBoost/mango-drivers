/* SPDX-License-Identifier: GPL-2.0 */
/*
 * smi_pub.h - Reading the sections a card publishes unasked.
 *
 * Each section opens with a prefix the card writes odd before its body and even
 * after, each flushed; the Region records say where each one is.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved. */

#ifndef SMI_PUB_H
#define SMI_PUB_H

#include <linux/types.h>

#include "contract/smi/smi_abi.h"

#include "smi_boot.h" /* struct smi_span, struct smi_pub_layout */

struct smi_transport;

/**
 * smi_pub_read() - One section, copied out under its own sequence.
 * @tp: the transport whose read-only region holds it.
 * @sec: where the section is, as its Region record states.
 * @boot: the boot this host attached to.
 * @dst: receives the section's first @len bytes, its prefix first.
 * @len: at least the prefix, at most the section.
 *
 * None of the three reads is retried on its own error, so a read that fails
 * at detach is never taken for a card mid-write.
 *
 * Return: 0; -ENODATA unwritten this boot; -ESTALE another boot's; -EAGAIN torn
 * every try; -EOPNOTSUPP not published; -EINVAL bad args; or the read's error.
 */
int smi_pub_read(struct smi_transport *tp, const struct smi_span *sec,
		 uint32_t boot, void *dst, size_t len);

/**
 * smi_pub_status_head() - The head of the Status section, which the kernel
 * reads each slice.
 * @tp: the transport whose read-only region holds it.
 * @pub: the layout the card stated, whether or not @tp has taken it yet.
 * @out: receives the head.
 *
 * Return: 0; -EOPNOTSUPP also for an untaught format or a section short of its
 * baseline, which this host does not have; else smi_pub_read()'s answer.
 */
int smi_pub_status_head(struct smi_transport *tp,
			const struct smi_pub_layout *pub,
			struct smi_pub_status_head *out);

#endif /* SMI_PUB_H */
