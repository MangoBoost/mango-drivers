// SPDX-License-Identifier: GPL-2.0
/*
 * smi_v2_cmd.c - the second wire generation: rows, frames and the negotiation
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved. */

/*
 * v2 answers every verb in one shape, so two helpers -- one stamps a frame, one
 * reads the status -- carry what every row shares, and a row fills only its
 * payload.
 *
 * The slice rides the header, so no row does bounds arithmetic; firmware checks
 * it once per verb. The one calculation left is the coordinate change, since v2
 * states the slice relative to the data region.
 *
 * The negotiation is a row like any other, so the engine claims the slot,
 * applies the deadline and waits; probing by hand would copy the walk. */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/version.h>

#include "transport/smi_transport.h"

#include "smi_v2_cmd.h"
#include "smi_build_id.h" /* generated: this build, as git names it */
#include "smi_cmd.h" /* smi_cmd_send(): the attach handshake is a row */
#include "smi_cmd_flags.h"
#include "smi_log.h"
#include "smi_v2_repo.h"
#include "smi_verb.h"

/* A refusal or a retry declines the request as posed: the card is sound and
 * the caller decides. By class, so a code this build does not know reads right. */
static bool smi_v2_refused(uint16_t status)
{
	return status && (status <= 0xFF) &&
	       (SMI_V2_STATUS_CLASS(status) <= SmiV2Class_Retry);
}

/* Congested alone says the card took nothing: the same request may go again. */
bool smi_v2_nothing_taken(uint16_t status)
{
	return SmiV2Status_Congested == status;
}

/* The answer, read as v2 shaped it. One cast, in one place. */
static inline const struct smi_v2_rsp *
smi_v2_answer(const struct smi_pending *cmd)
{
	return (const struct smi_v2_rsp *)cmd->completion;
}

/**
 * smi_v2_submit_as() - stamp the v2 header around a payload and submit.
 * @ctx: the command; its verb record names the opcode and what the card takes.
 * @host: the host context the header names; 0 is the kernel.
 * @pay: the verb's payload.
 * @pay_len: payload bytes this build fills.
 * @win_off: slice offset FROM THE DATA REGION, not from the window base.
 * @win_len: slice length, 0 when the verb claims none.
 *
 * The card is sent as much payload as it understands (smi_v2_pay_fit()); a call
 * that needs a field past that is refused before the wire.
 */
/* A request as it rides an SQ slot: the header, then the payload the verb fills
 * from its start (the card's union of every payload is in no contract copy). */
struct smi_v2_frame {
	struct smi_v2_req_hdr hdr;
	uint8_t pay[SMI_V2_PAY_MAX];
};
static_assert(offsetof(struct smi_v2_frame, pay) == SMI_V2_REQ_HDR_BYTES,
	      "the payload follows the header");

static int smi_v2_submit_as(struct smi_cmd_ctx *ctx, uint8_t host,
			    const void *pay, uint16_t pay_len, uint32_t win_off,
			    uint32_t win_len)
{
	struct smi_v2_frame req = { 0 };
	int n;

	if (!ctx->proxy_cmd || !ctx->verb || pay_len > SMI_V2_PAY_MAX)
		return -EINVAL;

	n = smi_v2_pay_fit(pay, pay_len, ctx->verb->pay_bytes);
	if (n < 0)
		return n;
	req.hdr.opcode = ctx->verb->opcode;
	req.hdr.cid = ctx->proxy_cmd->cmd_cid;
	req.hdr.ctx = host;
	req.hdr.pay_len = n;
	req.hdr.win_off = win_off;
	req.hdr.win_len = win_len;
	memcpy(req.pay, pay, n);

	return smc_proxy_submit(ctx->proxy_cmd, &req, SMI_V2_REQ_HDR_BYTES + n);
}

/* Every row but the raw one sends as the kernel. */
static int smi_v2_submit(struct smi_cmd_ctx *ctx, const void *pay,
			 uint16_t pay_len, uint32_t win_off, uint32_t win_len)
{
	return smi_v2_submit_as(ctx, 0, pay, pay_len, win_off, win_len);
}

/* What any v2 answer tells the engine beyond its errno. The card's detail is
 * never branched on: the engine logs it, to match the card's log. */
static void smi_v2_note(struct smi_cmd_ctx *ctx, const struct smi_v2_rsp *rsp)
{
	ctx->detail = rsp->detail;
	ctx->refused = smi_v2_refused(rsp->status);
	ctx->nothing_taken = smi_v2_nothing_taken(rsp->status);
	if (SmiV2Status_Resync == rsp->status)
		ctx->session_over = true;
}

/* Every v2 row's respond starts here: one status space, one mapping. */
static int smi_v2_rcode(struct smi_cmd_ctx *ctx)
{
	const struct smi_v2_rsp *rsp;

	if (!ctx || !ctx->proxy_cmd)
		return -EINVAL;

	rsp = smi_v2_answer(ctx->proxy_cmd);
	smi_v2_note(ctx, rsp);
	return smi_v2_status_to_errno(rsp->status);
}

/* The engine resolves a region as an absolute offset from the window base,
 * so this subtracts the declared data region -- the whole
 * coordinate change. */
static uint32_t smi_v2_win_off(const struct smi_cmd_ctx *ctx)
{
	/* The data region's start comes from the window, not from the
	 * negotiated answer: that answer is written INTO the region, so a host
	 * learning the region only from it could not say where to put it. Taken
	 * from the answer, the probe wrote one data_start away from where the
	 * host read and every later verb was refused. */
	const uint32_t data_start = smi_transport_data_start(ctx->tp);

	return (ctx->payload_address >= data_start)
		       ? (ctx->payload_address - data_start)
		       : 0;
}

/*----------------------------------------------------------------------------
 * Per-verb rows
 *--------------------------------------------------------------------------*/

/* Ask a repository what it holds. The values are published, so the request
 * names the repository and nothing else. */
static int describe_build(struct smi_cmd_ctx *ctx)
{
	struct smi_v2_desc_pay pay = { 0 };

	pay.rid = (uint8_t)ctx->rid;

	return smi_v2_submit(ctx, &pay, sizeof(pay), smi_v2_win_off(ctx),
			     ctx->payload_size);
}

/* Read back what the answer says it wrote, not what the request offered. */
static int window_respond(struct smi_cmd_ctx *ctx)
{
	const struct smi_v2_rsp *rsp = smi_v2_answer(ctx->proxy_cmd);
	int ret;

	/* What the answer said about the slice, before what it said about the
	 * request: a refusal carries the numbers a caller needs to decide, and
	 * what they mean is the verb's. */
	if (ctx->slice_out) {
		ctx->slice_out->used = rsp->win_used;
		ctx->slice_out->val = rsp->val;
	}

	ret = smi_v2_rcode(ctx);
	if (ret)
		return ret;

	/* The card cannot have written more than the slice it was offered, so a
	 * caller copying out on @used is never told otherwise. */
	if (rsp->win_used > ctx->payload_size)
		return -EPROTO;

	if (!ctx->data_buf || !rsp->win_used)
		return 0;

	return smi_transport_read(ctx->tp, ctx->payload_address, ctx->data_buf,
				  rsp->win_used);
}

/* Nothing but the status: one answer shape means most rows end here. */
static int status_respond(struct smi_cmd_ctx *ctx)
{
	return smi_v2_rcode(ctx);
}

/* The seconds ride flags, as LedCtl's beacon: 0 turns it off, any other holds it
 * that long, and the card judges the range. */
static int locate_build(struct smi_cmd_ctx *ctx)
{
	struct smi_v2_led_pay pay = { 0 };

	pay.op = ctx->flags ? SmiLedOp_BeaconOn : SmiLedOp_BeaconOff;
	pay.timeout_s = (uint32_t)ctx->flags;

	return smi_v2_submit(ctx, &pay, sizeof(pay), 0, 0);
}

static int eeprom_build(struct smi_cmd_ctx *ctx)
{
	struct smi_v2_rw_pay pay = { 0 };

	pay.dev_off = EEPROM_GET_OFFSET(ctx->flags);
	pay.rw = EEPROM_GET_TYPE(ctx->flags);

	return smi_v2_submit(ctx, &pay, sizeof(pay), smi_v2_win_off(ctx),
			     ctx->payload_size);
}

/* A verb the caller names: the slice carries its bytes both ways and is never
 * cut, so a request larger than the verb or the slot takes is refused here. */
static int raw_prep(struct smi_cmd_ctx *ctx)
{
	const struct smi_verb_req *req = ctx->arg;
	const uint32_t slice = max(req->in_len, req->out_cap);

	if (slice > smi_verb_window(ctx->verb, ctx->region_len))
		return -EMSGSIZE;

	ctx->payload_size = slice;
	if (req->in_len && smi_transport_write(ctx->tp, ctx->payload_address,
					       req->in, req->in_len)) {
		SMI_DEV_ERR(ctx->dev, "Failed to write payload data");
		return -EIO;
	}
	return 0;
}

static int raw_build(struct smi_cmd_ctx *ctx)
{
	const struct smi_verb_req *req = ctx->arg;

	return smi_v2_submit_as(ctx, req->ctx, req->pay, req->pay_len,
				ctx->payload_size ? smi_v2_win_off(ctx) : 0,
				ctx->payload_size);
}

/* The answer as the card gave it, for the caller to judge; the row refuses only
 * a slice the card overran. A Progress answer is the verb's own outcome, not a
 * failure the engine logs. */
static int raw_respond(struct smi_cmd_ctx *ctx)
{
	const struct smi_verb_req *req = ctx->arg;
	const struct smi_v2_rsp *rsp = smi_v2_answer(ctx->proxy_cmd);
	struct smi_verb_ans *ans = ctx->data_buf;
	const bool progress =
		(rsp->status <= 0xFF) &&
		(SmiV2Class_Progress == SMI_V2_STATUS_CLASS(rsp->status));
	uint32_t n;

	ans->status = rsp->status;
	ans->val = rsp->val;
	ans->detail = rsp->detail;
	ans->win_used = rsp->win_used;

	smi_v2_note(ctx, rsp);
	ctx->answer_errno = progress ? 0 : smi_v2_status_to_errno(rsp->status);

	if (rsp->win_used > ctx->payload_size)
		return -EPROTO;

	n = min(rsp->win_used, req->out_cap);
	if (!req->out || !n)
		return 0;
	return smi_transport_read(ctx->tp, ctx->payload_address, req->out, n);
}

/* Liveness as a round trip, not a status: the caller's token must come back in
 * val, the one scalar every v2 answer has. */
static int ping_build(struct smi_cmd_ctx *ctx)
{
	struct smi_v2_ping_pay pay = { 0 };

	pay.nonce = ctx->flags;

	return smi_v2_submit(ctx, &pay, sizeof(pay), 0, 0);
}

static int ping_respond(struct smi_cmd_ctx *ctx)
{
	const struct smi_v2_rsp *rsp = smi_v2_answer(ctx->proxy_cmd);
	int ret = smi_v2_rcode(ctx);

	if (ret)
		return ret;

	/* The caller's buffer takes the echo in its own width, whatever that
	 * is. */
	if (ctx->data_buf && ctx->data_size) {
		const uint32_t echo = rsp->val;

		memcpy(ctx->data_buf, &echo,
		       min_t(uint32_t, ctx->data_size, sizeof(echo)));
	}
	return 0;
}

/*----------------------------------------------------------------------------
 * The negotiation, as a row
 *--------------------------------------------------------------------------*/

static int negotiate_build(struct smi_cmd_ctx *ctx)
{
	struct smi_v2_nego_pay pay = { 0 };

	pay.nonce = ctx->flags;
	/* Who is asking, in the card's log: the kernel's release, which every
	 * build has, and whatever this build is named by. */
	pay.host_ver = LINUX_VERSION_CODE;
	pay.drv_build = MANGO_BUILD_ID;

	return smi_v2_submit(ctx, &pay, sizeof(pay), 0, 0);
}

/*
 * What this answer means, decided in one place so no caller has to guess.
 *
 * The generation is read off one frozen halfword: older firmware keeps a
 * completion state where v2 keeps the status, and both spell "unknown opcode"
 * with the same value. Only that answer means unsupported firmware; every
 * other refusal is reported as itself, so a wedged card is not mistaken for
 * an old one. A
 * status with no errno that is not Success would read as a settled
 * negotiation, so it is a protocol error here.
 */
static int negotiate_verdict(uint16_t status)
{
	int ret;

	if (status == SmiV2Status_Success)
		return 0;
	if (status == SmiV2Status_BadOpcode)
		return -EOPNOTSUPP; /* the only reason to try an older one */

	ret = smi_v2_status_to_errno(status);
	return ret ? ret : -EPROTO;
}

/* The answer as it came, for the attach to judge: whose it is, by the nonce,
 * before what it says. */
static int negotiate_respond(struct smi_cmd_ctx *ctx)
{
	const struct smi_v2_rsp *rsp = smi_v2_answer(ctx->proxy_cmd);
	struct smi_v2_nego_ans *ans = ctx->data_buf;

	if (!ans || (ctx->data_size < sizeof(*ans)))
		return -EINVAL;

	ans->status = rsp->status;
	ans->val = rsp->val;
	ans->detail = rsp->detail;
	return 0;
}

/* Foreign answers in a row that mean the card does not return the nonce, the
 * margin over a stated wait, and one sleep's cap: that wait is a bound. */
#define SMI_V2_NONCE_TRIES (3)
#define SMI_V2_QUIESCE_MARGIN_MS (100u)
#define SMI_V2_QUIESCE_STEP_MS (500u)

int smi_v2_nego_step(const struct smi_v2_nego_ans *ans, uint32_t nonce,
		     struct smi_v2_nego_walk *walk)
{
	uint32_t ms;

	/* Another session's answer: ask again under a new cid. A refusal puts a
	 * syndrome for the nonce, so a run ending in one is this ask's. */
	if (ans->detail != nonce) {
		bool refusal = (SmiV2Status_Success != ans->status) &&
			       (SmiV2Status_Quiescing != ans->status);

		walk->refused = refusal ? ans->status : 0;
		if (++walk->foreign <= SMI_V2_NONCE_TRIES)
			return 1;
		return refusal ? negotiate_verdict(ans->status) : -EPROTO;
	}
	walk->foreign = 0;

	if (SmiV2Status_Success == ans->status)
		return 0;
	if (SmiV2Status_Quiescing != ans->status)
		return negotiate_verdict(ans->status);

	/* Earlier requests remain and the card states their latest deadline, so
	 * ask again within a step; bound_ms or a stuck 0 still ends it. */
	ms = min_t(uint32_t, ans->val + SMI_V2_QUIESCE_MARGIN_MS,
		   SMI_V2_QUIESCE_STEP_MS);
	walk->idle_ms = ans->val ? 0 : walk->idle_ms + ms;
	walk->waited_ms += ms;
	if ((walk->idle_ms > SMI_V2_QUIESCE_MARGIN_MS) ||
	    (walk->waited_ms > walk->bound_ms))
		return -EBUSY;
	return (int)ms;
}

/*----------------------------------------------------------------------------
 * The table
 *
 * A generation-neutral shape, so the engine walks it without learning which
 * generation it is walking.
 *--------------------------------------------------------------------------*/

static const struct smi_cmd_desc smi_v2_table[] = {
	{
		.id = SMC_CMD_ID_NEGOTIATE,
		.opcode = SmiV2Opcode_Negotiate,
		.name = "negotiate",
		.needs_data_buf = true,
		.needs_data_size = true,
		.build = negotiate_build,
		.respond = negotiate_respond,
		/* Older firmware refuses this verb, and that refusal is the
		 * probe's answer rather than a fault to log. */
		.respond_error_is_data = true,
	},
	{
		.id = SMC_CMD_ID_GET_SDR,
		.opcode = SmiV2Opcode_Describe,
		.name = "describe",
		.needs_data_buf = true,
		.region = SMI_CMD_REGION_DATA,
		.prep = smi_cmd_prep_sensor,
		.build = describe_build,
		.respond = window_respond,
		.respond_error_is_data = true,
	},
	{
		.id = SMC_CMD_ID_HEARTBEAT,
		.opcode = SmiV2Opcode_Ping,
		.name = "ping",
		.needs_data_buf = true,
		.build = ping_build,
		.respond = ping_respond,
	},
	{
		.id = SMC_CMD_ID_EEPROM_READ_WRITE,
		.opcode = SmiV2Opcode_EepromRw,
		.name = "eeprom_rw",
		.needs_data_buf = true,
		.needs_data_size = true,
		.region = SMI_CMD_REGION_DATA,
		.prep = smi_cmd_prep_eeprom,
		.build = eeprom_build,
		.respond = window_respond,
	},
	{
		/* The opcode, name and slice are the request's: see smi_verb.h. */
		.id = SMC_CMD_ID_RAW,
		.name = "raw",
		.needs_data_buf = true,
		.prep = raw_prep,
		.build = raw_build,
		.respond = raw_respond,
	},
	{
		.id = SMC_CMD_ID_LOCATE,
		.opcode = SmiV2Opcode_LedCtl,
		.name = "locate",
		.build = locate_build,
		.respond = status_respond,
	},
};

static const struct smi_cmd_desc *smi_v2_cmd_find(int id)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(smi_v2_table); i++) {
		if (smi_v2_table[i].id == id)
			return &smi_v2_table[i];
	}

	return NULL;
}

/* Which verbs this card has and at what cost: its descriptor's Verb records,
 * read before the first frame, so the negotiation is no exception. */
static bool smi_v2_verb(struct smi_transport *tp,
			const struct smi_cmd_desc *desc, uint16_t opcode,
			struct smi_desc_verb *out)
{
	const struct smi_desc_verb *v = smi_transport_verb(tp, opcode);

	(void)desc;

	if (!v)
		return false;

	*out = *v;
	return true;
}

/*
 * Settle this generation: ask until the answer is this ask's and the lane holds
 * nothing earlier, over the ordinary engine, so these ops must be installed.
 */
static int smi_v2_attach(struct smi_cmd *cmd)
{
	struct smi_transport *tp = smi_cmd_tp(cmd);
	struct smi_v2_nego_walk walk = {
		.bound_ms = smi_transport_verb_longest_ms(tp) +
			    SMI_V2_QUIESCE_MARGIN_MS,
	};
	int ret;

	/* Driven off the descriptor: a window that does not state v2 is asked
	 * nothing; one stating no verbs is a build this host cannot drive. */
	if (!(smi_transport_gens(tp) & SMI_GEN_BIT_V2))
		return -EOPNOTSUPP;
	if (!smi_transport_verb(tp, SmiV2Opcode_Negotiate)) {
		SMI_DEV_ERR(
			smi_cmd_dev(cmd),
			"the descriptor states no verbs: firmware predates this driver");
		return -EPROTO;
	}

	do {
		struct smi_v2_nego_ans ans = { 0 };
		uint32_t nonce;

		/* Never 0, which an earlier answer's detail may well be. */
		do {
			nonce = get_random_u32();
		} while (!nonce);

		ret = smi_cmd_send(cmd, SMC_CMD_ID_NEGOTIATE, nonce, &ans,
				   sizeof(ans));
		if (!ret) {
			ret = smi_v2_nego_step(&ans, nonce, &walk);
			if (ret > 0)
				msleep(ret);
		}
	} while (ret > 0);

	if ((walk.foreign > SMI_V2_NONCE_TRIES) && walk.refused)
		SMI_DEV_ERR(smi_cmd_dev(cmd), "negotiate refused: status 0x%x",
			    walk.refused);
	else if (walk.foreign > SMI_V2_NONCE_TRIES)
		SMI_DEV_ERR(smi_cmd_dev(cmd),
			    "negotiate: the card does not return the nonce");
	else if ((walk.idle_ms > SMI_V2_QUIESCE_MARGIN_MS) ||
		 (walk.waited_ms > walk.bound_ms))
		SMI_DEV_ERR(
			smi_cmd_dev(cmd),
			"negotiate: an earlier session's request does not end (%u ms)",
			walk.waited_ms);
	if (!ret && walk.waited_ms)
		SMI_DEV_INFO(smi_cmd_dev(cmd),
			     "negotiate: lane cleared after %u ms",
			     walk.waited_ms);
	return ret;
}

const struct smi_wire_ops smi_v2_wire_ops = {
	.gens_bit = SMI_GEN_BIT_V2,
	.media_write_op = SmiV2Opcode_FlashWrite,
	.find = smi_v2_cmd_find,
	.verb = smi_v2_verb,
	.attach = smi_v2_attach,
	.repo_read = smi_v2_repo_read,
};
