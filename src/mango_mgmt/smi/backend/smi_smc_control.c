// SPDX-License-Identifier: GPL-2.0-only
/*
 * smi_smc_control.c - This file contains SMC control implementation.
 *
 * Copyright (c) 2023 - 2025 Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/pci.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

#include "contract/smi/smi_abi.h"

#include "smi_cmd.h"
#include "smi_cmd_row.h" /* struct smi_slice_result */
#include "smi_smc_internal.h"
#include "smi_top.h"

/*****************************************************************************/
/* Defines                                                                   */
/*****************************************************************************/
#define HEARTBEAT_REQUEST_INTERVAL (5000)
#define LOGGING_SLEEP_INTERVAL (3000)

/* Number of permitted failures before raising a fatal event */
#define HEARTBEAT_FAIL_THRESHOLD (3)

/*****************************************************************************/
/* Private functions                                                         */
/*****************************************************************************/

/*
 * sleep_with_stop_check() - msleep() in 100 ms chunks, polling kthread_stop.
 *
 * msleep() is TASK_UNINTERRUPTIBLE and ignores kthread_stop(), so "msleep(N);
 * check stop" blocks unload for up to N ms per thread; slicing caps that at
 * ~100 ms.
 *
 * Return: true if a stop was requested (caller should exit). */
static bool sleep_with_stop_check(unsigned int total_ms)
{
	unsigned int slept;

	for (slept = 0; slept < total_ms; slept += 100) {
		if (kthread_should_stop())
			return true;
		msleep(100);
	}
	return kthread_should_stop();
}

/* The round trip is slowed to this many cycles rather than dropped: the page
 * proves the task runs, an answer proves the ring still carries one. */
#define SMI_PING_EVERY (6) /* 6 x 5 s = one round trip every 30 s */

/* Liveness without a command: the serving task raises the Status tick each
 * slice, so it tells stopped from busy, and another boot's Status a restart.
 *
 * Return: true when the card is live. */
static bool heartbeat_tick_advanced(struct smc_control_ctx *smc_ctx,
				    uint32_t *last_tick, bool *seeded,
				    unsigned int *interval, bool *restarted)
{
	struct smi_pub_status_head blk;
	int ret = smi_cmd_pub_status_head(smc_ctx->cmd, &blk);

	if (ret == -ESTALE) {
		*restarted = true;
		return true;
	}
	if (ret) {
		/* A read that could not be completed is not evidence of death;
		 * report live and let the periodic round trip decide. */
		SMI_VDBG(smc_ctx, "Status unreadable (%d)", ret);
		return true;
	}

	/* A tick that has not risen is evidence only once the card has had a
	 * slice to raise it in, and the card states how long that is: a couple
	 * of slices, or this driver's own interval, whichever is longer. A card
	 * that predates the field says 0 and nothing changes. */
	if (blk.tick_ms) {
		const unsigned int least = (unsigned int)blk.tick_ms * 2u;

		if (*interval < least) {
			SMI_INFO(smc_ctx,
				 "card ticks every %u ms; polling every %u ms",
				 blk.tick_ms, least);
			*interval = least;
		}
	}

	/* The first sample has nothing to compare against, and 0 is a tick a
	 * card fresh out of reset really publishes, so seed and report live --
	 * the next cycle is the first that can tell. */
	if (!*seeded) {
		*seeded = true;
		*last_tick = blk.tick;
		return true;
	}

	if (blk.tick == *last_tick)
		return false;

	*last_tick = blk.tick;
	return true;
}

/**
 * heartbeat_health_thread() - the heartbeat health thread
 * @data: the data pointer to the smc control context
 *
 * Return: the errno return code if thread exits
 */
static int heartbeat_health_thread(void *data)
{
	struct smc_control_ctx *smc_ctx = NULL;
	uint8_t request_id = 0;
	uint8_t response_id = 0;
	int ret = 0;
	int fail_count = 0;
	bool fatal_event_raised = false;
	bool end_announced = false;
	bool watch_tick = false;
	bool restarted = false;
	const char *ended = NULL;
	uint32_t last_tick = 0;
	bool tick_seeded = false;
	/* 0, so the first cycle is the round trip: attach already waited for
	 * the tick to move (smi_boot_await_service), leaving only the ring
	 * unproven. */
	unsigned int cycle = 0;
	unsigned int interval = HEARTBEAT_REQUEST_INTERVAL;

	if (!data) {
		/* Without smc_ctx nothing useful is possible; bail out so
		 * kthread_stop() can collect us. */
		PR_ERR("Heartbeat health thread null data arg");
		return -EINVAL;
	}
	smc_ctx = (struct smc_control_ctx *)data;

	/* Loop header doubles as a stop check, so a kthread_stop() between
	 * iterations exits without another round trip. The submit below may
	 * block in wait_for_completion_*(); smi_cmd_quiesce() unblocks it as
	 * -ESHUTDOWN. */
	/* Settled once, after the negotiation: whether this card publishes is
	 * not something that changes while it is bound. */
	watch_tick = smi_cmd_publishes(smc_ctx->cmd);
	if (watch_tick)
		SMI_DBG(smc_ctx,
			"liveness from the published tick; ping every %d s",
			(HEARTBEAT_REQUEST_INTERVAL / 1000) * SMI_PING_EVERY);

	while (!kthread_should_stop()) {
		if (!fatal_event_raised &&
		    (fail_count < HEARTBEAT_FAIL_THRESHOLD)) {
			/* The window answers before anything read from it
			 * is believed: a decode turned off reads as
			 * all-ones, which is not a slow card. */
			if (!smi_cmd_window_answers(smc_ctx->cmd)) {
				ended = "the window no longer answers";
			} else if (watch_tick &&
				   (0 != (cycle++ % SMI_PING_EVERY))) {
				if (heartbeat_tick_advanced(
					    smc_ctx, &last_tick, &tick_seeded,
					    &interval, &restarted)) {
					fail_count = 0;
				} else {
					SMI_ERR(smc_ctx,
						"published tick has not advanced (%u)",
						last_tick);
					if (smc_ctx->event_cb)
						smc_ctx->event_cb(
							SMC_EVENT_ID_HEARTBEAT_EXPIRED,
							smc_ctx->pf_dev);
					fail_count++;
				}
			} else {
				ret = smi_submit_command(smc_ctx,
							 SMC_CMD_ID_HEARTBEAT,
							 request_id,
							 &response_id,
							 sizeof(response_id));
				if (smi_cmd_aborted(ret)) {
					/* Planned cancellation: no fatal callback. */
					break;
				} else if (ret) {
					/* Silence inside a deadline this driver
					 * granted is owed time, not evidence of
					 * death: a 92 MB flash goes quiet for
					 * longer than the 3 x 5 s budget. */
					if (smi_cmd_deadline_pending(
						    smc_ctx->cmd)) {
						SMI_VDBG(
							smc_ctx,
							"heartbeat unanswered (%d) while a request still has time",
							ret);
					} else if (watch_tick &&
						   heartbeat_tick_advanced(
							   smc_ctx, &last_tick,
							   &tick_seeded,
							   &interval,
							   &restarted)) {
						/* Another host's request is not
						 * this one's to see, and the card
						 * serves one at a time. A rising
						 * tick says the task that would
						 * answer is running. */
						SMI_VDBG(
							smc_ctx,
							"heartbeat unanswered (%d) while the tick still rises",
							ret);
					} else {
						PR_ERR("Failed to get the heartbeat msg!");
						if (smc_ctx->event_cb) {
							smc_ctx->event_cb(
								SMC_EVENT_ID_HEARTBEAT_EXPIRED,
								smc_ctx->pf_dev);
						}
						fail_count++;
					}
				} else if (response_id != request_id) {
					PR_ERR("Heartbeat validation failed!");
					if (smc_ctx->event_cb) {
						smc_ctx->event_cb(
							SMC_EVENT_ID_HEARTBEAT_VALIDATION,
							smc_ctx->pf_dev);
					}
					fail_count++;
				} else {
					/* Reset fail count */
					fail_count = 0;
				}

				/* Increment or rollover counter */
				request_id += 1;
			}
		} else {
			if (!fatal_event_raised) {
				PR_ERR("Heartbeat fail count above threshold! Raising fatal event...");
				smc_ctx->event_cb(SMC_EVENT_ID_HEARTBEAT_FATAL,
						  smc_ctx->pf_dev);
				fatal_event_raised = true;
			}
			/* Stay idle for kthread_stop(); see its contract. */
		}

		/* Every fact the session stated belongs to a card that is
		 * gone. Ending it here drains live requests at once, rather
		 * than leaving them against a ring that no longer answers. */
		if (restarted && !ended)
			ended = "the card restarted";
		if (!ended && smi_cmd_session_over(smc_ctx->cmd))
			ended = "the card holds no session for this host";

		/* The end of a session is its own announcement, not the
		 * fatal's, and is made once. */
		if (ended && !end_announced) {
			SMI_ERR(smc_ctx, "session ended: %s (tick %u)", ended,
				last_tick);
			if (smc_ctx->event_cb)
				smc_ctx->event_cb(SMC_EVENT_ID_CARD_RESTARTED,
						  smc_ctx->pf_dev);
			end_announced = true;
			fatal_event_raised = true;
		}

		if (sleep_with_stop_check(interval))
			break;
	}
	return 0;
}

/**
 * logging_thread() - the SMC logging thread
 * @data: the data pointer to the smc control context
 *
 * Periodically checks for incoming SMC logs and prints them to dmesg
 *
 * Return: 0 if the thread exits
 */
static int logging_thread(void *data)
{
	struct smc_control_ctx *smc_ctx = (struct smc_control_ctx *)data;

	struct pf_dev_struct *pf_dev = NULL;

	if (!smc_ctx) {
		PR_ERR("SMC Logging thread null data arg");
		/* Nothing to do but stay alive until kthread_stop(). */
		while (!sleep_with_stop_check(LOGGING_SLEEP_INTERVAL)) {
			/* idle */
		}
		return -EINVAL;
	}

	pf_dev = smc_ctx->pf_dev;

	/* The kthread_should_stop() term is what lets kthread_stop()'s
	 * wake_up_process() exit this wait; without it the wake re-evaluates
	 * log_init_done == false and re-parks. */
	wait_event_interruptible(smc_ctx->log_init_wq,
				 smc_ctx->log_init_done ||
					 kthread_should_stop());
	if (kthread_should_stop())
		return 0;

	while (!kthread_should_stop()) {
		/* Skip BAR I/O when pf_dev is gone or SMC is dead; stay alive
		 * (no return) so kthread_stop() can collect us. */
		if (!pf_dev || smi_pf_dev_refusal(pf_dev)) {
			if (sleep_with_stop_check(LOGGING_SLEEP_INTERVAL))
				break;
			continue;
		}

		smi_cmd_dump_log(smc_ctx->cmd);

		/* One tick has passed. What the host makes of it -- an alarm
		 * sweep today -- is decided above, not here. */
		if (smc_ctx->event_cb)
			smc_ctx->event_cb(SMC_EVENT_ID_POLL, pf_dev);

		if (sleep_with_stop_check(LOGGING_SLEEP_INTERVAL))
			break;
	}
	return 0;
}

/*****************************************************************************/
/* Public functions                                                          */
/*****************************************************************************/
/*
 * Top level function to submit a request and wait on response */
/*----------------------------------------------------------------------------
 * The session, as the layers above ask about it
 *
 * Each forwards one question to the command plane and answers a session that
 * is not there rather than making its caller check for one.
 *--------------------------------------------------------------------------*/

struct device *smi_smc_dev(struct smc_control_ctx *smc)
{
	return smc ? smc->dev : NULL;
}

void smi_smc_facts(struct smc_control_ctx *smc, struct smi_smc_facts *out)
{
	if (!out)
		return;

	memset(out, 0, sizeof(*out));
	if (!smc)
		return;

	out->version = smc->version;
	strscpy(out->build_tag, smi_cmd_build_tag(smc->cmd),
		sizeof(out->build_tag));
	out->gens_bit = smi_cmd_gens_bit(smc->cmd);
	out->eeprom_bytes = smi_cmd_eeprom_bytes(smc->cmd);
	out->publishes = smi_cmd_publishes(smc->cmd);
	out->boot_id = smi_cmd_pub(smc->cmd)->boot;
	out->locates = smi_cmd_serves(smc->cmd, SMC_CMD_ID_LOCATE);
	out->present = true;
}

const struct smi_span *smi_smc_pub_span(struct smc_control_ctx *smc,
					uint16_t kind, uint16_t inst)
{
	return smi_pub_find(smi_cmd_pub(smc ? smc->cmd : NULL), kind, inst);
}

int smi_smc_pub_read(struct smc_control_ctx *smc, const struct smi_span *sec,
		     void *dst, size_t len)
{
	return smc ? smi_cmd_pub_read(smc->cmd, sec, dst, len) : -ENODEV;
}

int smi_smc_pub_status_head(struct smc_control_ctx *smc,
			    struct smi_pub_status_head *out)
{
	return smc ? smi_cmd_pub_status_head(smc->cmd, out) : -ENODEV;
}

int smi_smc_tool_records(struct smc_control_ctx *smc, uint8_t **out,
			 size_t *len)
{
	return smc ? smi_cmd_tool_records(smc->cmd, out, len) : -ENODEV;
}

int smi_smc_repo_read(struct smc_control_ctx *smc,
		      const struct smi_repo_desc *desc, struct smi_repo *repo)
{
	return smc ? smi_cmd_repo_read(smc->cmd, smc, desc, repo) : -ENODEV;
}

bool smi_smc_publishes_values(struct smc_control_ctx *smc)
{
	return smc && smi_cmd_publishes_values(smc->cmd);
}

void smi_smc_quiesce(struct smc_control_ctx *smc)
{
	if (smc)
		smi_cmd_quiesce(smc->cmd);
}

void smi_smc_disconnect(struct smc_control_ctx *smc)
{
	if (smc)
		smi_cmd_deinit(smc->cmd);
}

bool smi_smc_deadline_pending(struct smc_control_ctx *smc)
{
	return smc && smi_cmd_deadline_pending(smc->cmd);
}

/* Discovery has run: the log worker may start reading, and the first alarm
 * sweep has a store to read from. */
void smi_smc_discovery_done(struct smc_control_ctx *smc)
{
	if (!smc)
		return;

	smc->log_init_done = true;
	wake_up_interruptible(&smc->log_init_wq);
}

/* A planned abort, counted for the card; a code smi_cmd_aborted() refuses is
 * not one. */
static void smi_cmd_abort_count(struct pf_dev_struct *pf_dev, int ret)
{
	const enum smi_cmd_abort_reason reason = smi_cmd_abort_reason(ret);

	if (pf_dev && (reason != SMI_CMD_ABORT_MAX))
		atomic64_inc(&pf_dev->cmd_aborted[reason]);
}

/* Policy fast paths every request passes, counted: correctness at teardown is
 * the locked transport gate's, these only avoid needless tag/region work. */
static int smc_admit(struct smc_control_ctx *smc_ctx)
{
	int ret;

	if (!smc_ctx)
		return -ENODEV;

	ret = smi_pf_dev_refusal(smc_ctx->pf_dev);
	if (ret)
		smi_cmd_abort_count(smc_ctx->pf_dev, ret);
	return ret;
}

/* Covers every caller, including the ones that log nothing. */
static int smc_counted(struct smc_control_ctx *smc_ctx, int ret)
{
	if (smi_cmd_aborted(ret))
		smi_cmd_abort_count(smc_ctx->pf_dev, ret);

	return ret;
}

/* Admitted, and each planned abort counted, as every request is: none reaches
 * the engine past a closed state. */
int smi_submit_command_slice(struct smc_control_ctx *smc_ctx, int cmd_id,
			     uint32_t flags, const void *arg, uint8_t *data_buf,
			     uint32_t data_size, struct smi_slice_result *slice)
{
	int ret = smc_admit(smc_ctx);

	if (ret)
		return ret;

	return smc_counted(smc_ctx,
			   smi_cmd_send_slice(smc_ctx->cmd, cmd_id, flags, arg,
					      data_buf, data_size, slice));
}

int smi_smc_call(struct smc_control_ctx *smc, const struct smi_verb_req *req,
		 struct smi_verb_ans *ans)
{
	int ret = smc_admit(smc);

	if (ret)
		return ret;

	return smc_counted(smc, smi_cmd_send_verb(smc->cmd, req, ans));
}

bool smi_smc_verb(struct smc_control_ctx *smc, uint16_t opcode,
		  struct smi_desc_verb *out)
{
	return smc && smi_cmd_verb(smc->cmd, opcode, out);
}

uint32_t smi_smc_slice_bytes(struct smc_control_ctx *smc)
{
	return smc ? smi_cmd_data_bytes(smc->cmd) : 0u;
}

int smi_smc_locate(struct smc_control_ctx *smc, uint16_t seconds)
{
	return smi_submit_command(smc, SMC_CMD_ID_LOCATE, seconds, NULL, 0);
}

/*
 * Init setup & configuration for the SMC */
int smi_smc_setup(struct pf_dev_struct *pf_dev, struct smi_init_data *init_data,
		  smc_event_callback event_cb)
{
	struct device *dev = pf_dev->dev;
	struct smc_control_ctx **smc_ctx = &pf_dev->smc_ctx;
	int ret = 0;
	int id_ret;
	struct smi_identify_rsp identity = { 0 };
	uint8_t link_major = 0;
	uint8_t link_minor = 0;
	const char *smc_hb_thread_name = "smc heartbeat";
	const char *smc_log_thread_name = "smc logging";

	if (!init_data)
		return -EINVAL;

	dev_vdbg(dev, "Setting up SMC\n");

	*smc_ctx =
		devm_kzalloc(dev, sizeof(struct smc_control_ctx), GFP_KERNEL);
	if (!(*smc_ctx)) {
		dev_err(dev, "Failed to allocate kernel memory for smc_ctx\n");
		ret = -ENOMEM;
		goto fail;
	}

	(*smc_ctx)->dev = dev;
	(*smc_ctx)->pf_dev = pf_dev;

	init_waitqueue_head(&((*smc_ctx)->log_init_wq));

	/* Message layer (GCQ or RPMSG per glue): smi_cmd_init() returns ->cmd,
	 * ERR_PTR on failure. NULL it on error so teardown treats the layer as
	 * absent. */
	(*smc_ctx)->cmd = smi_cmd_init(dev, init_data->msg_view,
				       init_data->transport_type);
	if (IS_ERR((*smc_ctx)->cmd)) {
		ret = PTR_ERR((*smc_ctx)->cmd);
		(*smc_ctx)->cmd = NULL;
		dev_err(dev, "Message Layer Init failed %d\n", ret);
		goto fail;
	}

	/* The card's facts, not the medium's (log by its record, build by the
	 * window, liveness by the Status tick), so every lane runs this. */

	/* Spawn logging thread. */
	(*smc_ctx)->logging_thread =
		kthread_create(logging_thread, *smc_ctx, smc_log_thread_name);

	if (IS_ERR((*smc_ctx)->logging_thread)) {
		dev_err(dev, "Unable to create the %s thread\n",
			smc_log_thread_name);
		ret = PTR_ERR((*smc_ctx)->logging_thread);
		/* The pointer says whether a thread is running, so a failed
		 * create leaves nothing to stop. */
		(*smc_ctx)->logging_thread = NULL;
		goto fail;
	} else {
		dev_vdbg(dev, "Successfully created %s thread\n",
			 smc_log_thread_name);
		wake_up_process((*smc_ctx)->logging_thread);
	}

	/* The generation first: every failure here is a failure to bring the
	 * lane up. */
	ret = smi_cmd_settle((*smc_ctx)->cmd);
	if (ret) {
		dev_err(dev, "Failed to settle a wire generation (%d)\n", ret);
		goto fail;
	}

	/* Telemetry, not a gate: the version answers no question this driver
	 * asks, so a card that will not tell costs a log line and nothing else. */
	id_ret = smi_cmd_identity((*smc_ctx)->cmd, &identity);
	if (id_ret && !smi_cmd_aborted(id_ret))
		SMI_WARN(*smc_ctx,
			 "no version from this card (%d); reporting none",
			 id_ret);
	link_major = identity.link_ver_major;
	link_minor = identity.link_ver_minor;

	(*smc_ctx)->version.ver_major = identity.ver_major;
	(*smc_ctx)->version.ver_minor = identity.ver_minor;
	(*smc_ctx)->version.ver_patch = identity.ver_patch;
	memcpy((*smc_ctx)->version.ver_date, identity.ver_date,
	       sizeof((*smc_ctx)->version.ver_date));

	dev_vdbg(dev,
		 "smc version = %d.%d.%d-%02x%02x%02x link version = %d.%d\n",
		 identity.ver_major, identity.ver_minor, identity.ver_patch,
		 identity.ver_date[0], identity.ver_date[1],
		 identity.ver_date[2], link_major, link_minor);

	/* The link library's version is telemetry, not a gate: an exact-match
	 * requirement would tear the SMC context down over a library refactor
	 * with no wire change. Nothing is said about a version nobody stated --
	 * a window states the card's build, the link pair comes from the verb
	 * or the negotiation. */
	if (link_major || link_minor)
		(void)smi_cmd_check_compatibility((*smc_ctx)->cmd, link_major,
						  link_minor);

	/* Spawn the heartbeat thread once version is verified */
	(*smc_ctx)->event_cb = event_cb;
	(*smc_ctx)->heartbeat_thread = kthread_create(
		heartbeat_health_thread, *smc_ctx, smc_hb_thread_name);

	if (IS_ERR((*smc_ctx)->heartbeat_thread)) {
		dev_err(dev, "Unable to create the %s thread\n",
			smc_hb_thread_name);
		ret = PTR_ERR((*smc_ctx)->heartbeat_thread);
		(*smc_ctx)->heartbeat_thread = NULL;
		goto fail;
	} else {
		dev_vdbg(dev, "Successfully created %s thread\n",
			 smc_hb_thread_name);
		wake_up_process((*smc_ctx)->heartbeat_thread);
	}

	return SUCCESS;

fail:
	smi_smc_teardown(pf_dev);
	dev_err(dev, "Failed to set up the SMC services on this lane\n");

	return ret;
}

/*
 * Stop the service, close proxy and tidy up PCI */
int smi_smc_teardown(struct pf_dev_struct *pf_dev)
{
	struct smc_control_ctx **smc_ctx;

	if (!pf_dev)
		return -EINVAL;

	smc_ctx = &pf_dev->smc_ctx;

	if (*smc_ctx) {
		/* Teardown order:
		 *   1) quiesce commands -> halt new submits, wake live waiters
		 *   2) stop heartbeat
		 *   3) stop logging
		 *   4) smi_cmd_deinit()  -> stops complete_response_thread last
		 *
		 * The halt and the submit check share cmd_lock, so a command is
		 * either LIVE before the halt and drained, or sees it and
		 * returns -ECANCELED with no FAL write. The drain's synthetic
		 * completion lets kthread_stop() reap blocked submitters. */
		smi_cmd_quiesce((*smc_ctx)->cmd);

		/* kthread_stop() returns the thread's result, or -EINTR if
		 * it never ran; neither is a failure to stop. */
		if ((*smc_ctx)->heartbeat_thread) {
			kthread_stop((*smc_ctx)->heartbeat_thread);
			(*smc_ctx)->heartbeat_thread = NULL;
		}
		if ((*smc_ctx)->logging_thread) {
			kthread_stop((*smc_ctx)->logging_thread);
			(*smc_ctx)->logging_thread = NULL;
		}

		smi_cmd_deinit((*smc_ctx)->cmd);

		/* The context is devm-allocated (reclaimed at detach); clear
		 * the back-pointer so a repeat teardown and the -ENODEV
		 * guards see it as gone. */
		*smc_ctx = NULL;
	}

	return 0;
}
