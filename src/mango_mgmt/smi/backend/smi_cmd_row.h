// SPDX-License-Identifier: GPL-2.0
/*
 * smi_cmd_row.h - the shape of one command row, shared by both generations
 *
 * One row per command holds its validation, payload staging, request build and
 * response handling; smi_cmd_send() runs one command-agnostic engine over it.
 * This header is the engine/command contract.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved. */

#ifndef SMI_CMD_ROW_H
#define SMI_CMD_ROW_H

#include <linux/types.h>

#include "contract/smi/smi_abi.h" /* struct smi_identify_rsp */

#include "smi_cmd_id.h" /* SMC_CMD_ID_* */
#include "smi_sensor_model.h" /* struct smi_repo -- what a repository is here */

struct smc_control_ctx;

/* Forward declarations (pointer members only). */
struct device;
struct smi_transport;
struct smi_pending;
struct smi_cmd;

/**
 * struct smi_slice_result - what an answer said about the slice it was given.
 *
 * @used: bytes the card wrote into it. This bounds what a caller reads back,
 *        and for a chain it is where the chain ends.
 * @val: the verb's own scalar: what a shell line dropped, the room a refused
 *       repository needs, the generation a QSFP answer carries.
 */
struct smi_slice_result {
	uint32_t used;
	uint32_t val;
};

/**
 * enum smi_cmd_region - Payload region a command stages through.
 * @SMI_CMD_REGION_NONE: command carries no shared-memory payload.
 * @SMI_CMD_REGION_DATA: bulk data payload region.
 *
 * NONE is 0 so an omitted field defaults to "no region".
 */
enum smi_cmd_region {
	SMI_CMD_REGION_NONE = 0,
	SMI_CMD_REGION_DATA,
};

/**
 * struct smi_cmd_ctx - Per-invocation state for a single command submission.
 *
 * The values the engine threads between staging, building and responding.
 * Zero-initialised by the engine; prep/build/respond refine it.
 *
 * @dev:             owning device (for logging).
 * @tp:              transport context (data plane).
 * @proxy_cmd:       in-flight proxy command for this submission.
 * @cmd_id:          the caller's command id: SMC_CMD_ID_*, or one the settled
 *                   generation names for itself.
 * @flags:           packed per-command flags.
 * @data_buf:        caller payload buffer (may be NULL for some commands).
 * @data_size:       size of @data_buf.
 * @payload_address: device-side payload address, set from get_region().
 * @payload_size:    payload length staged to/from the device.
 * @region_len:      region capacity returned by get_region().
 * @rid:             the driver's sensor repo byte, resolved in prep and used
 *                   in build.
 * @slice_out:       where respond leaves what the return code cannot say (the
 *                   bytes the card wrote, the verb's val); NULL if not asked.
 * @arg:             parameters that do not fit @flags, in the shape the verb's
 *                   row knows; NULL for every verb that fits.
 * @verb:            what the card states about this verb: its opcode, the
 *                   payload it understands, its deadline and bounds.
 * @detail:          the card's syndrome for a refusal, 0 when it gave none;
 *                   the engine logs it with the failure.
 * @answer_errno:    what the card's status means, for a row that hands every
 *                   answer to its caller: the engine logs it as a failure.
 * @refused:         set where the generation reads the card's status, when the
 *                   card declined the request as posed rather than failed it.
 * @nothing_taken:   set there too, when the card took nothing of the request:
 *                   the same request may go again.
 * @session_over:    set where the card said it holds no session for this host.
 */
struct smi_cmd_ctx {
	struct device *dev;
	struct smi_transport *tp;
	struct smi_pending *proxy_cmd;
	int cmd_id;
	uint32_t flags;
	void *data_buf;
	uint32_t data_size;
	uint32_t payload_address;
	uint32_t payload_size;
	uint32_t region_len;
	int rid;
	struct smi_slice_result *slice_out;
	const void *arg;
	const struct smi_desc_verb *verb;
	uint32_t detail;
	int answer_errno;
	bool refused;
	bool nothing_taken;
	bool session_over;
};

/**
 * struct smi_cmd_desc - Declarative description of one SMC command.
 *
 * @id:              the command id this row describes (table key).
 * @name:            human-readable name for logging.
 * @opcode:          the wire verb this command sends.
 * @deadline_ms:     how long the answer may take, for a generation whose card
 *                   states no Verb records: its frozen list's deadline column.
 * @win_max:         the largest slice, for such a generation: its frozen unit.
 * @needs_data_buf:  reject a NULL data_buf (the CALLER's buffer: a row such as
 *                   heartbeat answers into it while claiming no window).
 * @needs_data_size: reject submission with zero data_size, same sense.
 * @region:          payload region to acquire (NONE = none).
 * @prep:            optional; between get_region() and CID alloc: length
 *                   checks, stage-in, sub-id lookup. A failure needs no abort.
 * @build:           required; builds the request and issues the matching
 *                   smc_proxy_request_* call. On failure the engine aborts.
 * @respond:         optional; parses the response and stages any read-out.
 *                   NULL means the rcode check is the only response handling.
 * @respond_error_is_data: a refusal (@refused) logs at debug, as the caller
 *                   decides on it; any other failure is a fault, at error.
 */
struct smi_cmd_desc {
	int id;
	const char *name;

	uint16_t opcode;
	unsigned int deadline_ms;
	uint32_t win_max;

	bool needs_data_buf;
	bool needs_data_size;

	enum smi_cmd_region region;

	int (*prep)(struct smi_cmd_ctx *c);
	int (*build)(struct smi_cmd_ctx *c);
	int (*respond)(struct smi_cmd_ctx *c);

	bool respond_error_is_data;
};

/**
 * struct smi_wire_ops - One wire generation, as the engine asks about it.
 *
 * A generation owns its rows and answer vocabulary; what a card serves is its
 * window's. Nothing here names a generation, so one more is a row and a file.
 *
 * @gens_bit: SMI_GEN_BIT_* this generation answers to.
 * @media_write_op: the verb that writes one chunk of a boot image. Its waits do
 *                  not end on a signal, so a burn that began runs to its end.
 * @find: the row for a command id, or NULL when this generation has no such
 *        verb -- which is the answer, not a fault.
 * @verb: the card's statement of @opcode (deadline, bounds) into @out, or false
 *        if it has none; with no verbs stated, @desc's own values. @opcode is
 *        the row's, or the one a request names.
 * @attach: settle this generation on this card by its handshake. -EOPNOTSUPP
 *          only when the card does not speak it.
 * @repo_read: read one repository whole into the neutral model. How a card
 *             says what it holds is this generation's alone -- the store above
 *             holds only the answer.
 */
struct smi_wire_ops {
	uint16_t gens_bit;
	uint16_t media_write_op;
	const struct smi_cmd_desc *(*find)(int id);
	bool (*verb)(struct smi_transport *tp, const struct smi_cmd_desc *desc,
		     uint16_t opcode, struct smi_desc_verb *out);
	int (*attach)(struct smi_cmd *cmd);
	int (*repo_read)(struct smc_control_ctx *smc_ctx,
			 const struct smi_repo_desc *desc,
			 struct smi_repo *repo);
};

/* The generation-neutral row callbacks (smi_cmd_prep.c), for a row whose prep
 * is only sizing, staging and id resolution. */
int smi_cmd_prep_sensor(struct smi_cmd_ctx *ctx);
int smi_cmd_prep_window(struct smi_cmd_ctx *ctx, bool out);
int smi_cmd_prep_eeprom(struct smi_cmd_ctx *ctx);

#endif /* SMI_CMD_ROW_H */
