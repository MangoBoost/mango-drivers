/* SPDX-License-Identifier: GPL-2.0 */
/*
 * smi_rpc.h - What the card states, as the kernel passes it through.
 *
 * One typed call that sends a verb as the card states it, the records that
 * bound it, and the sections it publishes, read under their own seq. Not
 * exported past this module: the card trusts context 0 whole.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_RPC_H
#define SMI_RPC_H

#include <linux/types.h>

#include "contract/smi/smi_abi.h" /* struct smi_desc_verb */

#include "smi_verb.h" /* struct smi_verb_req, struct smi_verb_ans */

struct pf_dev_struct;

/**
 * smi_verb_call() - Send one verb of the card's own.
 * @pf_dev: the card.
 * @req: the verb, its payload, the slice's bytes both ways, and the context it
 *	 is sent as (0, the kernel, for every kernel caller).
 * @ans: the card's answer head, filled whether it refused or not.
 *
 * Return: 0 when the card answered, whatever it said; an errno when it did
 *         not, and -EPROTO when it claims to have written past the slice it
 *         was given.
 */
int smi_verb_call(struct pf_dev_struct *pf_dev, const struct smi_verb_req *req,
		  struct smi_verb_ans *ans);

/* The card's Verb record for @opcode into @out (may be NULL); false where it
 * states none. */
bool smi_verb_record(struct pf_dev_struct *pf_dev, uint16_t opcode,
		     struct smi_desc_verb *out);

/* The slice @opcode may claim on this card, as requests that follow each other
 * take it (smi_verb_stride()); 0 where the card states no such verb. */
uint32_t smi_verb_cap(struct pf_dev_struct *pf_dev, uint16_t opcode);

/* smi_verb_call(), with a refusal read as its status's errno. */
int smi_verb_run(struct pf_dev_struct *pf_dev, const struct smi_verb_req *req,
		 struct smi_verb_ans *ans);

/* The descriptor records a tool reads, INFO's: kmalloc'd, with @length set;
 * the caller frees them. What each record means is its reader's. */
void *smi_desc_tool_records(struct pf_dev_struct *pf_dev, size_t *length);

/* The length of the section the card publishes as @kind/@inst, SmiDescRegion_*:
 * 0 where it names one this host cannot reach, -EOPNOTSUPP where it names none. */
int smi_pub_section_len(struct pf_dev_struct *pf_dev, uint16_t kind,
			uint16_t inst);

/**
 * smi_pub_section_read() - @len bytes at @at of one published section.
 * @pf_dev: the card.
 * @kind: SmiDescRegion_*.
 * @inst: which one of a repeatable kind; else 0.
 * @at: where in the section.
 * @buf: receives the bytes.
 * @len: at least one, within the section.
 *
 * Under the section's own seq: a span that opens it is read directly, any other
 * is cut from a whole read, so the bytes never mix two writes.
 *
 * Return: 0; -EINVAL a span outside the section; -EOPNOTSUPP not published;
 *         -ENODATA unwritten this boot; -ESTALE another boot's; -EAGAIN torn
 *         every try; or the read's error.
 */
int smi_pub_section_read(struct pf_dev_struct *pf_dev, uint16_t kind,
			 uint16_t inst, uint32_t at, void *buf, size_t len);

/**
 * smi_pub_status_read() - The Status section, as far as this build knows it.
 * @pf_dev: the card.
 * @out: receives the section; a field past what the card wrote reads zero.
 *
 * Return: 0; -EOPNOTSUPP also for an untaught format or a section short of its
 *         baseline; else the section read's answer.
 */
int smi_pub_status_read(struct pf_dev_struct *pf_dev,
			struct smi_pub_status *out);

/* Whether the card's Status head says Busy: false for a card the heartbeat
 * gave up on, and for a head that cannot be read or is another boot's. */
bool smi_pub_busy(struct pf_dev_struct *pf_dev);

#endif /* SMI_RPC_H */
