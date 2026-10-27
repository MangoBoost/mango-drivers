// SPDX-License-Identifier: GPL-2.0
/*
 * smi_cmd.h - Message Layer Definitions.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved. */

#ifndef SMI_CMD_H
#define SMI_CMD_H

#include <linux/types.h>

#include "contract/smi/smi_abi.h" /* struct smi_pub_status_head */

#include "fw_if.h"
#include "smi_cmd_id.h"
#include "smi_resource.h" /* struct smi_cmd_resource_view */
#include "smi_sensor_model.h" /* struct smi_repo -- what a repository is here */

struct smc_control_ctx;

/* Opaque control-plane handle; its body is file-local to smi_cmd.c. */
struct device;
struct smi_cmd;
struct smi_pub_layout;
struct smi_span;
struct smi_transport;

/* Message layer API. smi_cmd_init() returns the handle (ERR_PTR on failure)
 * that the caller stores and passes back to every entry point; the body stays
 * private to smi_cmd.c, and the engine logs on @dev and never touches struct
 * smc_control_ctx. */
struct smi_cmd *smi_cmd_init(struct device *dev,
			     struct smi_cmd_resource_view *view,
			     FW_IF_TYPE type);
void smi_cmd_deinit(struct smi_cmd *m);
struct smi_slice_result;
/* @arg: parameters @flags cannot hold, else NULL. @slice (optional): what the
 * answer said of its slice, for an answer the request does not bound. */
int smi_cmd_send_slice(struct smi_cmd *m, int cmd_id, uint32_t flags,
		       const void *arg, void *data, uint32_t size,
		       struct smi_slice_result *slice);
int smi_cmd_send(struct smi_cmd *m, int cmd_id, uint32_t flags, void *data,
		 uint32_t size);
void smi_cmd_dump_log(struct smi_cmd *m);
int smi_cmd_check_compatibility(struct smi_cmd *m, uint16_t major,
				uint16_t minor);

/**
 * smi_cmd_quiesce() - Stop new submissions and force-complete live commands.
 * @m: message handle whose command path should be quiesced.
 *
 * Halts the transport under its submit lock before draining the proxy queue, so
 * a command is either LIVE before the halt and drained or sees the halt and is
 * never submitted. Safe to call repeatedly.
 */
void smi_cmd_quiesce(struct smi_cmd *m);

/**
 * smi_cmd_deadline_pending() - true while a live command still has time left on
 *                              the deadline its row granted it.
 * @handle: the message-layer handle
 */
bool smi_cmd_deadline_pending(void *handle);

/**
 * smi_cmd_settle() - settle which generation this card speaks.
 * @m: message handle.
 *
 * Walks the compiled generations newest first, filtered by what the card's
 * window advertised, and installs the first one whose attach succeeds. Only a
 * card refusing a generation moves on to an older one: every other failure is
 * returned as itself, so a wedged or unreachable card is never mistaken for an
 * old one.
 *
 * Return: 0 with a generation settled, -EOPNOTSUPP when this driver and this
 *         card share none, or the attach's own errno.
 */
int smi_cmd_settle(struct smi_cmd *m);

/**
 * smi_cmd_dev() - the device this engine logs on.
 * @m: message handle.
 *
 * For a generation's own attach, which logs what it settled.
 */
struct device *smi_cmd_dev(const struct smi_cmd *m);

/**
 * smi_cmd_tp() - the transport this engine frames on.
 * @m: message handle.
 *
 * For a generation's own attach, which reads what the window states.
 */
struct smi_transport *smi_cmd_tp(const struct smi_cmd *m);

/**
 * smi_cmd_session_over() - Did the card say it holds no session for this host?
 * @m: message handle.
 *
 * Latched by an answer of Resync; the session ends the way a restart ends it.
 */
bool smi_cmd_session_over(const struct smi_cmd *m);

struct smi_verb_req;
struct smi_verb_ans;

/**
 * smi_cmd_send_verb() - Send a verb the request names, as the card states it.
 * @m: message handle.
 * @req: the verb, its payload and the slice's bytes both ways.
 * @ans: the card's answer head, filled whether it refused or not.
 *
 * The slice is never cut: one past the Verb's win_max or the slot is refused.
 *
 * Return: 0 when the card answered, whatever it said; an errno when it did
 *         not, -EMSGSIZE for a slice it cannot take, -EPROTO when it claims to
 *         have written past the slice it was given.
 */
int smi_cmd_send_verb(struct smi_cmd *m, const struct smi_verb_req *req,
		      struct smi_verb_ans *ans);

/* The card's Verb record for @opcode into @out (may be NULL); false where it
 * states none. */
bool smi_cmd_verb(struct smi_cmd *m, uint16_t opcode,
		  struct smi_desc_verb *out);

/**
 * smi_cmd_serves() - Does this card serve the verb a command id names?
 * @m: message handle.
 * @id: the command.
 *
 * Both halves: the settled generation has a row for it, and the card states
 * the verb.
 */
bool smi_cmd_serves(struct smi_cmd *m, int id);

/**
 * smi_cmd_eeprom_bytes() - the addressable extent of this card's EEPROM.
 * @m: the command layer.
 *
 * What the card's window states. A window that states none gets the extent
 * every part shipped so far has.
 */
uint32_t smi_cmd_eeprom_bytes(struct smi_cmd *m);

/**
 * smi_cmd_identity() - The build the card's window states.
 * @m: the command engine.
 * @out: filled when the card stated its build.
 *
 * Return: 0 when @out was filled, -EOPNOTSUPP where the window states none.
 */
int smi_cmd_identity(struct smi_cmd *m, struct smi_identify_rsp *out);

/**
 * smi_cmd_publishes() - Does this card publish sections unasked?
 * @m: the command layer.
 */
bool smi_cmd_publishes(struct smi_cmd *m);

/**
 * smi_cmd_pub() - The sections this card publishes, as it stated them.
 * @m: the command layer.
 *
 * Return: the layout; an empty one where the card publishes nothing.
 */
const struct smi_pub_layout *smi_cmd_pub(struct smi_cmd *m);

/**
 * smi_cmd_pub_read() - One published section, under its own sequence.
 * @m: the command layer.
 * @sec: the section, one of smi_cmd_pub()'s.
 * @dst: receives its first @len bytes, the prefix first.
 * @len: at least the prefix, at most the section.
 *
 * Return: 0, -EOPNOTSUPP if this card does not publish, else smi_pub_read()'s
 *         (-ENODATA not written, -ESTALE another boot's, -EAGAIN always torn).
 */
int smi_cmd_pub_read(struct smi_cmd *m, const struct smi_span *sec, void *dst,
		     size_t len);

/**
 * smi_cmd_pub_status_head() - The Status head, as far as this build knows it.
 * @m: the command layer.
 * @out: filled on success.
 *
 * Return: 0, -EOPNOTSUPP if this card publishes no Status this build reads,
 *         else smi_cmd_pub_read()'s answer.
 */
int smi_cmd_pub_status_head(struct smi_cmd *m, struct smi_pub_status_head *out);

/* The descriptor records a tool reads: see smi_boot_tool_records(). */
int smi_cmd_tool_records(struct smi_cmd *m, uint8_t **out, size_t *len);

/**
 * smi_cmd_build_tag() - The name the card's tree gave its build.
 * @m: Command engine.
 *
 * Return: A NUL terminated name, empty where the card states none.
 */
const char *smi_cmd_build_tag(struct smi_cmd *m);

/**
 * smi_cmd_gens_bit() - Which generation this card settled on.
 * @m: Command engine.
 *
 * The axis a surface is chosen by, and the one a test asks about: the
 * generation that settled, not the window's dialect.
 *
 * Return: SMI_GEN_BIT_*, or 0 before a generation is settled.
 */
uint16_t smi_cmd_gens_bit(const struct smi_cmd *m);

/**
 * smi_cmd_repo_read() - Read one repository whole, into the neutral model.
 * @m: Command engine.
 * @smc_ctx: The context its generation submits through.
 * @desc: Descriptor for the repository to read.
 * @repo: Slot to populate.
 *
 * Return: 0, -ENOMSG for a repository that described nothing, -EOPNOTSUPP
 *         before a generation is settled, else an errno.
 */
int smi_cmd_repo_read(struct smi_cmd *m, struct smc_control_ctx *smc_ctx,
		      const struct smi_repo_desc *desc, struct smi_repo *repo);

/**
 * smi_cmd_window_answers() - Is the card's window still readable?
 * @m: Command engine.
 *
 * Return: false when the window states no magic this build knows -- a decode
 *         that was turned off, or a card mid-boot. Nothing read from it then
 *         is data.
 */
bool smi_cmd_window_answers(struct smi_cmd *m);

/**
 * smi_cmd_publishes_values() - Are sensor values read from the published plane?
 * @m: the command layer.
 *
 * Where the card publishes a Sensors section, the values are read there.
 */
bool smi_cmd_publishes_values(struct smi_cmd *m);

/**
 * smi_cmd_data_bytes() - The largest slice one request can claim.
 * @m: Command layer.
 *
 * What a card-stated answer size is measured against before a host allocates
 * for it: an answer rides one slot, so a size past this cannot be carried.
 *
 * Return: the slice ceiling, or 0 where this card offers no data region.
 */
uint32_t smi_cmd_data_bytes(struct smi_cmd *m);

#endif /* SMI_CMD_H */
