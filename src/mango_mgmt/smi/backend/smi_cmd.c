// SPDX-License-Identifier: GPL-2.0
/*
 * smi_cmd.c - Message Layer Implementation
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved. */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/kernel.h> /* min_t */
#include <linux/slab.h>

#include "contract/smi/smi_abi.h"

#include "transport/smi_boot.h" /* SMI_BOOT_SERVE_BUDGET_MS */
#include "transport/smi_pub.h"
#include "transport/smi_transport.h"

#include "smi_cmd.h"
#include "smc_proxy.h"
#include "smi_log.h"
#include "smi_v2_cmd.h"
#include "smi_verb.h"

/*****************************************************************************/
/* Local Varaiables                                                          */
/*****************************************************************************/

/*****************************************************************************/
/* Defines                                                                   */
/*****************************************************************************/
/* Fallback response timeout for a verb the shared command list does not name;
 * every verb it names carries its own deadline there. */
#define REQUEST_MSQ_TIMEOUT (msecs_to_jiffies(5000)) /*  5 seconds */

/* How often to ask again while the card is silent. Coarser than the window
 * waiter's: an ask spends a staging slot, and a slot the card never answered
 * for comes back only on a transport start. */
#define SMI_SETTLE_RETRY_INTERVAL_MS (1000)
#define SMI_SETTLE_RETRIES                                                     \
	(SMI_BOOT_SERVE_BUDGET_MS / SMI_SETTLE_RETRY_INTERVAL_MS)

/* A kernel request the card took nothing of goes again for at most the verb's
 * deadline and this, pausing 10 ms, doubled up to 500 ms, in between. */
#define SMI_CMD_AGAIN_MS (20000)
#define SMI_CMD_PAUSE_MIN_MS (10)
#define SMI_CMD_PAUSE_MAX_MS (500)

/**
 * struct smi_cmd - Control-plane (message) per-device state.
 *
 * The handle smi_cmd_init() returns, kept in smc_control_ctx->cmd.
 * Owns the command-plane proxy and the log dump cursor, carries the device for
 * logging, and points at the data-plane transport. Body private to this file.
 */
struct smi_cmd {
	struct device *dev; /* owning device (for logging) */
	struct smi_transport *tp; /* data-plane transport (devm, opaque) */
	struct smc_proxy_instance *proxy; /* command-plane proxy (devm) */
	int last_printed_msg_index; /* dump_log cursor */
	/* The settled generation; NULL until smi_cmd_settle(). */
	const struct smi_wire_ops *wire_ops;
	/* A slice lent to an orphaned request, by cid: its answer gives it back. */
	struct smi_lent_slice {
		uint32_t addr;
		enum smi_region_type type;
		bool held;
	} lent[SMC_PROXY_MAX_CIDS];
	/* The card answered Resync: this session is not one it knows. */
	bool session_over;
};

/* The generations this driver can speak, newest first. */
static const struct smi_wire_ops *const smi_wires[] = {
	&smi_v2_wire_ops,
};

/* A generation's number is its nibble plus one. */
static unsigned int gen_of(const struct smi_wire_ops *ops)
{
	return (unsigned int)(ffs((int)ops->gens_bit));
}

struct device *smi_cmd_dev(const struct smi_cmd *m)
{
	return m ? m->dev : NULL;
}

struct smi_transport *smi_cmd_tp(const struct smi_cmd *m)
{
	return m ? m->tp : NULL;
}

bool smi_cmd_session_over(const struct smi_cmd *m)
{
	return m && READ_ONCE(m->session_over);
}

/* What the settled generation and the card state of one command's verb. */
static bool smi_cmd_verb_of(struct smi_cmd *m, int id,
			    struct smi_desc_verb *out)
{
	const struct smi_cmd_desc *desc;

	if (!m || !m->wire_ops)
		return false;

	desc = m->wire_ops->find(id);
	return desc && m->wire_ops->verb(m->tp, desc, desc->opcode, out);
}

bool smi_cmd_serves(struct smi_cmd *m, int id)
{
	struct smi_desc_verb verb;

	return smi_cmd_verb_of(m, id, &verb);
}

bool smi_cmd_verb(struct smi_cmd *m, uint16_t opcode, struct smi_desc_verb *out)
{
	const struct smi_desc_verb *v =
		m ? smi_transport_verb(m->tp, opcode) : NULL;

	if (v && out)
		*out = *v;
	return v;
}

/*****************************************************************************/
/* Private functions                                                         */
/*****************************************************************************/

/**
 * smc_proxy_callback() - the event callback.
 * @proxy_id: the associated proxy id.
 * @event_id: the event that cause the callback to be invoked.
 * @arg: void ptr containing the proxy command used to raise request.
 *
 * Return: errno or success code.
 */
static int smc_proxy_callback(uint8_t proxy_id, uint8_t event_id, void *arg)
{
	struct smi_pending *smc_proxy_cmd = NULL;
	int ret = 0;

	if (!arg)
		return -EINVAL;

	smc_proxy_cmd = (struct smi_pending *)arg;

	switch (event_id) {
	case SMC_PROXY_EVENT_RESPONSE_COMPLETE:
		/* Signal condvar to unblock & fetch the response */
		complete(&smc_proxy_cmd->cmd_complete);
		break;

	case SMC_PROXY_EVENT_RESPONSE_TIMEOUT:
		/* Signal condvar to unblock & fetch the failed response */
		complete(&smc_proxy_cmd->cmd_complete);
		smc_proxy_cmd->timed_out = true;
		break;

	case SMC_PROXY_EVENT_ORPHAN_ANSWERED: {
		struct smi_cmd *m = smc_proxy_cmd->cmd_arg;
		struct smi_lent_slice *s = &m->lent[smc_proxy_cmd->cmd_cid];

		if (s->held) {
			smi_transport_put_region(m->tp, s->type, s->addr);
			s->held = false;
		}
	} break;

	default:
		/* Unknown response */
		ret = -EIO;
		break;
	}

	return ret;
}

/*
 * Give up waiting: a card that answers every request is owed its answer, so the
 * cid and slice wait for it, the slice noted first for an answer racing this.
 */
static bool smi_cmd_orphan(struct smi_cmd *m, struct smi_pending *cmd,
			   bool lent, enum smi_region_type type, uint32_t addr)
{
	struct smi_lent_slice *s = &m->lent[cmd->cmd_cid];

	*s = (struct smi_lent_slice){ .addr = addr,
				      .type = type,
				      .held = lent };
	if (smc_proxy_orphan(cmd))
		return true;

	s->held = false;
	return false;
}

/*****************************************************************************/
/* Public functions                                                          */
/*****************************************************************************/

/**
 * struct smi_cmd_call - What one submission is called and how it logs.
 * @opcode: the verb, whose record bounds the submission.
 * @name: what the log calls it.
 * @quiet_refusal: a refusal logs at debug, as the caller decides on it.
 * @region: the payload region it stages through.
 * @host_ctx: the host context the request names, 0 for the kernel's own.
 *
 * A row's own values, or what a raw request names.
 */
struct smi_cmd_call {
	uint16_t opcode;
	const char *name;
	bool quiet_refusal;
	enum smi_cmd_region region;
	uint8_t host_ctx;
};

/* The kernel's own request, not a user's: only it goes again, and only its
 * failures are logged -- a user's answer is that user's. */
static bool smi_cmd_kernel_call(const struct smi_cmd_call *call)
{
	return !call->host_ctx;
}

static bool smi_cmd_again(const struct smi_cmd_ctx *ctx,
			  const struct smi_cmd_call *call)
{
	return ctx->nothing_taken && smi_cmd_kernel_call(call);
}

/* Pause before going again, no longer than the budget left. A burn's chunk
 * sleeps through a signal; any other request stops on one with -EINTR. */
static int smi_cmd_pause(unsigned long until, unsigned int *pause_ms,
			 bool media)
{
	const unsigned int ms = min_t(unsigned int, *pause_ms,
				      jiffies_to_msecs(until - jiffies));

	*pause_ms = min_t(unsigned int, *pause_ms * 2, SMI_CMD_PAUSE_MAX_MS);
	if (media) {
		msleep(ms);
		return 0;
	}
	return msleep_interruptible(ms) ? -EINTR : 0;
}

/* The shared body. @slice_out is where a row's respond leaves what the return
 * code cannot say, and the caller owns the storage so two callers cannot share
 * one slot. */
static int smi_cmd_run(struct smi_cmd *m, const struct smi_cmd_desc *desc,
		       const struct smi_cmd_call *call, int cmd_id,
		       uint32_t flags, const void *arg, void *data_buf,
		       uint32_t data_size, struct smi_slice_result *slice_out)
{
	int ret = SUCCESS;
	int err;
	struct smi_desc_verb verb = { 0 };
	struct smi_cmd_ctx ctx = { 0 };
	struct smi_pending *smc_proxy_cmd = NULL;
	struct completion *req_complete = NULL;
	bool region_acquired = false;
	enum smi_region_type region = SMI_REGION_DATA;
	struct smi_transport *tp_ctx = m->tp;
	const bool media = (call->opcode == m->wire_ops->media_write_op);
	unsigned int pause_ms = SMI_CMD_PAUSE_MIN_MS;
	unsigned long deadline = 0;
	unsigned long budget = 0;
	unsigned long until = 0;
	unsigned long wait_timeout = 0;
	unsigned long now = 0;

	if (!m->wire_ops->verb(tp_ctx, desc, call->opcode, &verb)) {
		/* The card said it has no such verb: answer without a round
		 * trip. */
		SMI_DEV_DBG(m->dev, "%s: this card has no such verb",
			    call->name);
		return -EOPNOTSUPP;
	}

	/* Validate caller-supplied payload requirements. */
	if (desc->needs_data_buf && !data_buf) {
		ret = -EINVAL;
		goto done;
	}
	if (desc->needs_data_size && !data_size) {
		ret = -EINVAL;
		goto done;
	}

	/* The verb's, as the card states it; one stated as 0 falls back to the
	 * engine default. A slot waited for gets as long again, never forever. */
	deadline = verb.deadline_ms ? msecs_to_jiffies(verb.deadline_ms)
				    : REQUEST_MSQ_TIMEOUT;
	budget = min_t(unsigned long, deadline,
		       msecs_to_jiffies(SMI_CMD_AGAIN_MS));
	until = jiffies + budget;

again:
	memset(&ctx, 0, sizeof(ctx));
	smc_proxy_cmd = NULL;
	req_complete = NULL;
	region_acquired = false;

	ctx.dev = m->dev;
	ctx.tp = tp_ctx;
	ctx.verb = &verb;
	ctx.slice_out = slice_out;
	ctx.arg = arg;
	ctx.cmd_id = cmd_id;
	ctx.flags = flags;
	ctx.data_buf = data_buf;
	ctx.data_size = data_size;

	/* Acquire the payload region (if any) and run command-specific prep. A
   * prep failure bails without aborting: no request has been issued yet.
   */
	if (call->region != SMI_CMD_REGION_NONE) {
		ret = smi_transport_get_region(tp_ctx, region,
					       &ctx.payload_address,
					       &ctx.region_len, deadline,
					       media);
		if (ret) {
			/* -EINTR: a signal interrupted the wait for the region
       * semaphore -- a benign user-space abort, not a device
       * error, kept so callers can skip their own logging. */
			if (ret == -EINTR)
				SMI_DEV_DBG(
					m->dev,
					"data region wait interrupted by signal (%d)",
					ret);
			else
				SMI_DEV_ERR(m->dev,
					    "Failed to acquire data semaphore (%d)",
					    ret);
			goto done;
		}
		region_acquired = true;
	}

	/* The linearization point against quiesce: a command that gets the lock
   * first is published LIVE before quiesce can drain it, one that gets it
   * later is cancelled without touching hardware. */
	ret = smi_transport_submit_begin(tp_ctx);
	if (ret) {
		if (ret == -ECANCELED)
			SMI_DEV_DBG(
				m->dev,
				"%s canceled before submit (service quiesced)",
				call->name);
		else
			SMI_DEV_ERR(m->dev, "%s submit gate failed (%d)",
				    call->name, ret);
		goto done;
	}

	/* Prep may stage payload into transport memory, so keep it on the
   * running side of the same halt boundary as the FAL write. No CID yet.
   */
	if (desc->prep) {
		ret = desc->prep(&ctx);
		if (ret) {
			smi_transport_submit_end(tp_ctx);
			goto done;
		}
	}

	/* Claim the proxy slot only after the transport gate is known to be
   * open. This assigns cmd_cid, the owning instance and cmd_complete. */
	smc_proxy_cmd = smc_proxy_cid_claim(m->proxy);
	if (!smc_proxy_cmd) {
		/* The tag space is 64 wide and the ring holds four, so this is
     * exhaustion of names rather than of room -- it means responses
     * stopped coming back, not that the medium is busy. */
		smi_transport_submit_end(tp_ctx);
		SMI_DEV_ERR(m->dev, "Failed to claim a command slot");
		ret = -EBUSY;
		goto done;
	}
	ctx.proxy_cmd = smc_proxy_cmd;
	req_complete = &smc_proxy_cmd->cmd_complete;

	smc_proxy_cmd->cmd_timeout_jiffies = jiffies + deadline;
	smc_proxy_cmd->cmd_arg = m;
	smc_proxy_cmd->cmd_suppress_dbg = true;

	ret = desc->build(&ctx);
	smi_transport_submit_end(tp_ctx);

	if (ret) {
		SMI_DEV_ERR(m->dev, "%s submit failed (%d), abort", call->name,
			    ret);
		smc_proxy_request_abort(smc_proxy_cmd);
		goto done;
	}

	/* Wait until SMC responds, the proxy fires a timeout, or this upper
   * bound expires. _timeout and not _killable: kthread_stop() delivers no
   * signal, so TASK_KILLABLE would never release. The proxy owns the
   * primary timeout via complete_response_thread; this is a floor.
   *
   * +100 ms avoids racing the proxy callback at the same deadline;
   * time_after() guards a cmd_timeout_jiffies that already passed, with 1
   * jiffy keeping the wait non-zero. */
	now = jiffies;
	wait_timeout =
		time_after((unsigned long)smc_proxy_cmd->cmd_timeout_jiffies,
			   now)
			? ((unsigned long)smc_proxy_cmd->cmd_timeout_jiffies -
			   now) + msecs_to_jiffies(100)
			: msecs_to_jiffies(1);

	if (wait_for_completion_timeout(req_complete, wait_timeout) == 0) {
		SMI_DEV_ERR(m->dev, "%s timed out waiting for response",
			    call->name);
		ret = -ETIMEDOUT;
		goto gave_up;
	}

	/* Check shutdown before timed_out: smc_proxy_callback reuses TIMEOUT
   * for force-drain and always sets timed_out, so cmd_rcode is the only
   * discriminator (-ESHUTDOWN vs a real -ETIME). */
	if (smc_proxy_cmd->cmd_rcode == -ESHUTDOWN) {
		SMI_DEV_DBG(m->dev, "Request aborted (shutdown)");
		smc_proxy_request_abort(smc_proxy_cmd);
		ret = -ESHUTDOWN;
		goto done;
	}

	if (smc_proxy_cmd->timed_out) {
		/* Asking is what every attach does, and a card that does not
		 * serve the verb answers by not answering -- the caller settles
		 * the generation and says so. Every other verb has no such
		 * fallback, so its silence is an error here. */
		if (SMC_CMD_ID_NEGOTIATE == cmd_id)
			SMI_DEV_DBG(m->dev, "%s timed out", call->name);
		else
			SMI_DEV_ERR(m->dev, "%s timed out", call->name);
		ret = -ETIMEDOUT;
		goto gave_up;
	}

	/* Check the return code before reading the response. cmd_rcode is the
   * errno the proxy chose (-EIO once a live command was removed);
   * flattening it to -EINVAL made that indistinguishable from SMC's own
   * InvalidValue, which the repo diagnostics read as "no such repo".
   * -ETIMEDOUT reports the proxy's -ETIME, one condition one code. */
	if (smc_proxy_cmd->cmd_rcode != 0) {
		ret = (smc_proxy_cmd->cmd_rcode == -ETIME)
			      ? -ETIMEDOUT
			      : smc_proxy_cmd->cmd_rcode;
		SMI_DEV_ERR(m->dev, "%s request failed (%d)", call->name, ret);
		smc_proxy_request_abort(smc_proxy_cmd);
		goto done;
	}

	if (cmd_id != SMC_CMD_ID_HEARTBEAT)
		SMI_DEV_DBG(
			m->dev,
			"Command processed successfully, rcode: %d cmd_id: %d",
			smc_proxy_cmd->cmd_rcode, cmd_id);

	if (desc->respond) {
		ret = desc->respond(&ctx);
		if (ctx.session_over)
			WRITE_ONCE(m->session_over, true);
		err = ret ? ret : ctx.answer_errno;
		if (err && (!smi_cmd_kernel_call(call) ||
			    (call->quiet_refusal && ctx.refused) ||
			    smi_cmd_again(&ctx, call)))
			SMI_DEV_DBG(m->dev,
				    "%s response returned %d (detail %u)",
				    call->name, err, ctx.detail);
		else if (err)
			SMI_DEV_ERR_RATELIMITED(
				m->dev, "%s response failed (%d, detail %u)",
				call->name, err, ctx.detail);
	}

	goto done;

gave_up:
	if (smi_transport_states_verbs(tp_ctx) &&
	    smi_cmd_orphan(m, smc_proxy_cmd, region_acquired, region,
			   ctx.payload_address))
		return ret;
	smc_proxy_request_abort(smc_proxy_cmd);

done:
	/* Return the command slot (frees its cid/tag). The slot itself lives in
   * the proxy instance pool and is not freed here. */
	if (smc_proxy_cmd)
		smc_proxy_tag_release(smc_proxy_cmd);

	/* Where the card may not answer, an unanswered slot stays out: work may
	 * still write it. Where it answers everything, the answer freed it. */
	if (region_acquired) {
		if ((ret == -ETIMEDOUT) && !smi_transport_states_verbs(tp_ctx))
			smi_transport_retire_region(tp_ctx, region,
						    ctx.payload_address);
		else
			smi_transport_put_region(tp_ctx, region,
						 ctx.payload_address);
	}

	/* The card took nothing: the same request again, within the budget. */
	if (smi_cmd_again(&ctx, call)) {
		if (!time_before(jiffies, until)) {
			SMI_DEV_ERR(m->dev,
				    "%s: the card took nothing for %u ms",
				    call->name, jiffies_to_msecs(budget));
			return ret;
		}
		err = smi_cmd_pause(until, &pause_ms, media);
		if (err)
			return err;
		goto again;
	}

	return ret;
}

int smi_cmd_send_slice(struct smi_cmd *m, int cmd_id, uint32_t flags,
		       const void *arg, void *data_buf, uint32_t data_size,
		       struct smi_slice_result *slice_out)
{
	const struct smi_cmd_desc *desc;
	struct smi_cmd_call call;

	/* No settled generation means nothing can be framed yet; an attach
	 * installs its own ops before asking its question. */
	if (!m || !m->wire_ops)
		return -EINVAL;

	if (cmd_id == SMC_CMD_ID_UNKNOWN) {
		SMI_DEV_ERR(m->dev, "Unable to allocate command id");
		return -EIO;
	}

	/* Verb, flags and length, for comparing two runs over dyndbg. */
	if (cmd_id != SMC_CMD_ID_HEARTBEAT)
		SMI_DEV_DBG(m->dev,
			    "Submitting command [%d] flags 0x%08x resp len %d",
			    cmd_id, flags, data_size);

	desc = m->wire_ops->find(cmd_id);
	if (!desc) {
		/* The generation this card speaks owns no row for that id --
		 * which is a verb this card does not have, not a wire fault.
		 * Debug, not error: a userspace loop asking for a verb its card
		 * lacks would otherwise fill the log. */
		SMI_DEV_DBG(m->dev, "command id %d: no row in this generation",
			    cmd_id);
		return -EOPNOTSUPP;
	}

	call = (struct smi_cmd_call){ .opcode = desc->opcode,
				      .name = desc->name,
				      .quiet_refusal =
					      desc->respond_error_is_data,
				      .region = desc->region };
	return smi_cmd_run(m, desc, &call, cmd_id, flags, arg, data_buf,
			   data_size, slice_out);
}

/* The raw row takes a slice only where the request stages or reads bytes, so a
 * verb with no window waits on no region. */
int smi_cmd_send_verb(struct smi_cmd *m, const struct smi_verb_req *req,
		      struct smi_verb_ans *ans)
{
	const struct smi_cmd_desc *desc;
	struct smi_cmd_call call;

	if (!m || !m->wire_ops || !req || !ans)
		return -EINVAL;

	desc = m->wire_ops->find(SMC_CMD_ID_RAW);
	if (!desc)
		return -EOPNOTSUPP;

	SMI_DEV_DBG(m->dev, "Submitting verb 0x%04x in %u out %u", req->opcode,
		    req->in_len, req->out_cap);

	call = (struct smi_cmd_call){ .opcode = req->opcode,
				      .name = req->name ? req->name
							: desc->name,
				      .quiet_refusal = req->refusal_is_answer,
				      .region = (req->in_len || req->out_cap)
							? SMI_CMD_REGION_DATA
							: SMI_CMD_REGION_NONE,
				      .host_ctx = req->ctx };
	return smi_cmd_run(m, desc, &call, SMC_CMD_ID_RAW, 0, req, ans,
			   sizeof(*ans), NULL);
}

/**
 * smi_cmd_send() - Submit a command to the SMC and wait for its response.
 *
 * Validates, finds the settled generation's row, acquires the payload region
 * and runs its prep/build/respond around the frozen wait/abort/rcode handshake.
 */
int smi_cmd_send(struct smi_cmd *m, int cmd_id, uint32_t flags, void *data_buf,
		 uint32_t data_size)
{
	return smi_cmd_send_slice(m, cmd_id, flags, NULL, data_buf, data_size,
				  NULL);
}

void smi_cmd_dump_log(struct smi_cmd *m)
{
	uint32_t current_log_idx = 0;
	uint32_t records;
	int i = 0;

	if (!m)
		return;

	/* A window that states no magic states no records either: printing its
	 * all-ones is how a card that went away fills the log. */
	if (!smi_cmd_window_answers(m))
		return;

	/* -ENODEV means a concurrent smi_cmd_deinit() NULLed the payload base,
   * -EOPNOTSUPP that this window declares no log; nothing to dump. */
	if (smi_transport_log_index(m->tp, &current_log_idx))
		return;

	/* The ring's own size, as the window stated it. */
	records = smi_transport_log_records(m->tp);
	if (!records || current_log_idx >= records) {
		SMI_DEV_ERR_ONCE(
			m->dev,
			"Logging thread error - invalid PCI data read!");
		return;
	}

	i = m->last_printed_msg_index;

	while (i != current_log_idx) {
		/* This host's room, zeroed so a record that fills it still ends;
     * how wide a record is belongs to the window. */
		char msg[SMI_LOG_SNAPSHOT_BYTES] = { 0 };

		if (smi_transport_log_read_record(m->tp, i, msg,
						  sizeof(msg) - 1))
			return;

		if (msg[0])
			SMI_DEV_SMC_LOG(m->dev, "%s", msg);

		i = (i + 1) % records;
	}
	m->last_printed_msg_index = current_log_idx;
}

/*
 * Init setup & configuration for the SMC. */
struct smi_cmd *smi_cmd_init(struct device *dev,
			     struct smi_cmd_resource_view *view,
			     FW_IF_TYPE type)
{
	struct smi_cmd *m;
	struct smi_transport *tp;
	int ret;

	if (!dev)
		return ERR_PTR(-EINVAL);
	if (view &&
	    (view->queue_size > U32_MAX || view->payload_size > U32_MAX))
		return ERR_PTR(-EINVAL);

	m = devm_kzalloc(dev, sizeof(*m), GFP_KERNEL);
	if (!m)
		return ERR_PTR(-ENOMEM);
	m->dev = dev;

	/* The backend probes the device, fills the transport and opens the FAL;
   * the instance is devm-owned (lifetime = device). */
	{
		const struct smi_transport_res res = {
			view ? view->queue_base : NULL,
			view ? (uint32_t)view->queue_size : 0u,
			view ? view->payload_base : NULL,
			view ? (uint32_t)view->payload_size : 0u,
		};

		tp = smi_transport_create(dev, type, &res);
	}
	if (IS_ERR(tp))
		return ERR_CAST(tp);
	m->tp = tp;

	/* Start the service. */
	ret = smi_transport_start(tp);
	if (ret)
		return ERR_PTR(ret);

	/* Init proxy (devm-owned by this device) and bind in callback. */
	SMI_DEV_DBG(dev, "Initializing SMC proxy for command-plane");
	/* The medium's own frame limit, carried in once. */
	m->proxy = smc_proxy_init(dev, smi_transport_get_fal(tp),
				  smi_transport_tx_capacity(tp));
	if (!m->proxy)
		return ERR_PTR(-EIO);

	ret = smc_proxy_bind_callback(m->proxy, smc_proxy_callback);
	if (ret)
		return ERR_PTR(ret);

	return m;
}

/* Forget the settled generation; safe on one that never settled. */
static void smi_cmd_unsettle(struct smi_cmd *m)
{
	if (!m || !m->wire_ops)
		return;

	m->wire_ops = NULL;
}

void smi_cmd_deinit(struct smi_cmd *m)
{
	int ret;

	if (!m)
		return;

	/* Release the generation's state before the planes it settled over go. */
	smi_cmd_unsettle(m);

	smi_transport_stop(m->tp);

	/* smc_proxy_close() reaps response_thread BEFORE invalidate_io() NULLs
   * the BAR pointers, else it reads through a NULL. logging_thread and
   * heartbeat_health_thread stop later in teardown; the fatal-event path
   * relies on the readers' guards. The instance is devm-owned, so nothing
   * is freed here; -EPERM means already closed, which is expected. */
	ret = smc_proxy_close(m->proxy);
	if (ret && ret != -EPERM)
		dev_err(m->dev, "Failed to close the smc proxy %d\n", ret);

	/* smi_transport_invalidate_io() publishes NULL with WRITE_ONCE so
   * cyclic READ_ONCE readers (smi_cmd_dump_log) see a clean switch. */
	smi_transport_invalidate_io(m->tp);
}

/* smi_cmd_quiesce() - close the submit gate, then wake live waiters.
 * smi_transport_stop() takes submit_begin()'s cmd_lock, so no new FAL write
 * begins after it returns; LIVE commands are force-drained with -ESHUTDOWN.
 * Call before kthread_stop() on submitters. */
void smi_cmd_quiesce(struct smi_cmd *m)
{
	if (!m)
		return;

	smi_transport_stop(m->tp);
	smc_proxy_drain_all(m->proxy);
}

/**
 * smi_cmd_check_compatibility() - Is the GCQ library version compatible?
 * @ctxt: SMC data struct instance.
 * @major: The expected major version.
 * @minor: The expected minor version.
 *
 * Return: 0 on success, -ENOTSUPP if incompatible, or other error code.
 */
int smi_cmd_check_compatibility(struct smi_cmd *m, uint16_t major,
				uint16_t minor)
{
	if (!m)
		return -EINVAL;

	return smi_transport_check_compatibility(m->tp, major, minor);
}

/* The liveness watchdog asks before counting an unanswered ping: its budget
 * (3 x 5 s) is shorter than the longest deadline this driver hands out, so
 * without the question it kills transfers it permitted. */
bool smi_cmd_deadline_pending(void *handle)
{
	struct smi_cmd *m = (struct smi_cmd *)handle;

	return m ? smc_proxy_deadline_pending(m->proxy) : false;
}

/* The extent of a card whose window states none. */
#define SMI_EEPROM_BYTES_ASSUMED (256u)

uint32_t smi_cmd_eeprom_bytes(struct smi_cmd *m)
{
	uint32_t n = m ? smi_transport_limits(m->tp).eeprom_bytes : 0u;

	return n ? n : SMI_EEPROM_BYTES_ASSUMED;
}

int smi_cmd_identity(struct smi_cmd *m, struct smi_identify_rsp *out)
{
	if (!m || !out)
		return -EINVAL;

	/* What the read-only region states -- available before a frame is sent
	 * and on a lane that never negotiates. */
	return smi_transport_build(m->tp, out) ? -EOPNOTSUPP : 0;
}

bool smi_cmd_publishes(struct smi_cmd *m)
{
	/* The window's answer, not the negotiation's: a card that states a boot
	 * and a Status section publishes into it. */
	return m && smi_transport_publishes(m->tp);
}

const struct smi_pub_layout *smi_cmd_pub(struct smi_cmd *m)
{
	return smi_transport_pub(m ? m->tp : NULL);
}

int smi_cmd_pub_read(struct smi_cmd *m, const struct smi_span *sec, void *dst,
		     size_t len)
{
	if (!m || !sec || !dst)
		return -EINVAL;
	if (!smi_cmd_publishes(m))
		return -EOPNOTSUPP;

	return smi_pub_read(m->tp, sec, smi_transport_pub(m->tp)->boot, dst,
			    len);
}

int smi_cmd_pub_status_head(struct smi_cmd *m, struct smi_pub_status_head *out)
{
	if (!m || !out)
		return -EINVAL;
	if (!smi_cmd_publishes(m))
		return -EOPNOTSUPP;

	return smi_pub_status_head(m->tp, smi_transport_pub(m->tp), out);
}

int smi_cmd_tool_records(struct smi_cmd *m, uint8_t **out, size_t *len)
{
	return m ? smi_boot_tool_records(m->tp, out, len) : -EINVAL;
}

const char *smi_cmd_build_tag(struct smi_cmd *m)
{
	return m ? smi_transport_build_tag(m->tp) : "";
}

uint16_t smi_cmd_gens_bit(const struct smi_cmd *m)
{
	return (m && m->wire_ops) ? m->wire_ops->gens_bit : 0;
}

int smi_cmd_repo_read(struct smi_cmd *m, struct smc_control_ctx *smc_ctx,
		      const struct smi_repo_desc *desc, struct smi_repo *repo)
{
	if (!m || !m->wire_ops || !m->wire_ops->repo_read)
		return -EOPNOTSUPP;

	return m->wire_ops->repo_read(smc_ctx, desc, repo);
}

bool smi_cmd_window_answers(struct smi_cmd *m)
{
	return m && smi_boot_window_answers(m->tp);
}

bool smi_cmd_publishes_values(struct smi_cmd *m)
{
	const struct smi_span *sensors;

	/* A card that publishes a Sensors section reads its values there. */
	if (!smi_cmd_publishes(m))
		return false;
	sensors = smi_pub_find(smi_transport_pub(m->tp), SmiDescRegion_Sensors,
			       0);
	return sensors && sensors->len;
}

uint32_t smi_cmd_data_bytes(struct smi_cmd *m)
{
	return m ? smi_transport_slot_bytes(m->tp, SMI_REGION_DATA) : 0u;
}

static int smi_cmd_settle_once(struct smi_cmd *m)
{
	uint32_t gens;
	size_t i;
	int ret = -EOPNOTSUPP;

	/* The generations the window states; with no such field all stay open,
	 * and a generation that needs it refuses in its own attach. */
	gens = smi_transport_gens(m->tp);

	for (i = 0; i < ARRAY_SIZE(smi_wires); i++) {
		const struct smi_wire_ops *ops = smi_wires[i];

		if (gens && !(gens & ops->gens_bit))
			continue;

		/* Installed BEFORE the handshake: attach asks its question
     * through the ordinary send path, which picks the frame, the
     * deadline and the answer reader off these very ops. */
		m->wire_ops = ops;

		ret = ops->attach(m);
		if (!ret) {
			/* Said once, for every generation: an attach that
       * negotiates logs what it agreed, one that does not
       * would otherwise say nothing at all. */
			SMI_DEV_INFO(m->dev, "wire generation %u settled",
				     gen_of(ops));
			return 0;
		}

		smi_cmd_unsettle(m);

		/* Only "this card does not speak that" is a reason to try
		 * another generation: any other failure would be hidden behind
		 * a working-looking card. */
		if (ret != -EOPNOTSUPP)
			return ret;
	}

	SMI_DEV_ERR(m->dev, "no supported wire generation (card states 0x%x)",
		    gens);
	return ret;
}

int smi_cmd_settle(struct smi_cmd *m)
{
	unsigned long t0 = jiffies;
	unsigned int tries;
	int ret;

	if (!m)
		return -EINVAL;

	/* A described window is not a card that answers: the endpoint task starts
   * near the end of firmware's boot, so a probe sent right after a reset goes
   * unanswered for seconds. Silence is that gap, not a verdict on a generation,
   * and firmware in the field cannot say when it closes -- so it is waited out.
   * Every other failure stands at once. */
	for (tries = 0;; tries++) {
		ret = smi_cmd_settle_once(m);
		if (ret != -ETIMEDOUT || tries == SMI_SETTLE_RETRIES)
			break;
		msleep(SMI_SETTLE_RETRY_INTERVAL_MS);
	}

	/* Elapsed, not the sleeps: each silent ask also spent its own deadline. */
	if (!ret && tries)
		SMI_DEV_INFO(m->dev, "card answered after %u ms",
			     jiffies_to_msecs(jiffies - t0));

	return ret;
}
