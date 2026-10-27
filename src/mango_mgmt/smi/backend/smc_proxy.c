// SPDX-License-Identifier: GPL-2.0-only
/*
 * smc_proxy.c - This file contains SMC proxy layer implementation.
 *
 * Copyright (c) 2023 - 2025 Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

/*****************************************************************************/
/* Includes                                                                  */
/*****************************************************************************/
#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/slab.h>

#include "smc_proxy.h" /* request arguments */
#include "fw_if_gcq.h" /* the medium's own "no room" answer */

/*****************************************************************************/
/* Defines                                                                   */
/*****************************************************************************/

#define SMC_PROXY_MSLEEP_1S (1000)

/* How long to pause before re-offering a request the ring had no room for.
 * The consumer drains every slice, so this is the granularity of noticing that
 * it has. */
#define SMC_PROXY_RING_FULL_US (1000)

/*
 * Composite (instance, cid) identifier for proxy log lines. A per-instance cid
 * is not globally unique, so logs pair it with the instance id to stay
 * unambiguous across devices.
 */
#define SMC_CID_FMT "inst%u/cid%u"
#define SMC_CID_ARG(inst, c) (unsigned int)(inst)->proxy_id, (unsigned int)(c)

/* Monotonic source for per-instance ids (logging/debug only). */
static atomic_t smc_proxy_next_id = ATOMIC_INIT(0);

/*****************************************************************************/
/* Enums                                                                     */
/*****************************************************************************/

/*****************************************************************************/
/* Structs                                                                   */
/*****************************************************************************/

/*
 * The wire itself -- frames, opcodes, payloads, results -- is declared in
 * contract/smi/smi_abi.h, which the SMC firmware compiles too.
 */

/**
 * struct smc_proxy_instance - internal proxy instance
 *
 * @fw_if_handle: handle to the FW IF
 * @proxy_id: per-instance id (logging/debug; auto-assigned at init)
 * @event_cb: the event callback to be invoked when event generated
 * @lifecycle_lock: serialises bind/submit/drain/close lifecycle operations
 * @lock: lock protecting @cmds / @tags / @draining
 * @response_thread: thread to poll and handle responses
 * @cmds: pre-allocated command pool, indexed by cid (tag). Each slot's state
 *        (enum smc_cmd_state) tracks its lifecycle. Commands are never freed
 *        individually; the pool lives for the instance's lifetime.
 * @tags: cid reservation bitmap (bit set = cid in use, alloc..release). Kept set
 *        past the matchable window so a late response for a just-handled cid is
 *        rejected; cross-cycle reuse-collision is only bounded by the cyclic
 *        allocation distance (probabilistic, as in the legacy cid pool).
 * @response_thread: completion worker, NULL when none is running.
 * @next_cid: cyclic allocation cursor for @tags.
 * @initialised: flag to indicate layer has been init
 * @draining: set once a force-drain (shutdown) has run; late HW responses for
 *            already-drained CIDs are then expected, not errors. Guarded by @lock.
 */
struct smc_proxy_instance {
	FW_IF_CFG *fw_if_handle;
	uint8_t proxy_id;
	smc_proxy_event_callback *event_cb;
	struct mutex lifecycle_lock;
	struct mutex lock;
	struct task_struct *response_thread;
	struct smi_pending cmds[SMC_PROXY_MAX_CIDS];
	DECLARE_BITMAP(tags, SMC_PROXY_MAX_CIDS);
	uint16_t next_cid;
	/* The largest frame this medium carries, from the transport that owns
	 * that fact. A longer request is refused here, where its cid is known. */
	uint32_t tx_cap;
	bool initialised;
	bool draining;
};

/*****************************************************************************/
/* Local Functions                                                           */
/*****************************************************************************/

/* A completion arrives as bytes, so a wider load through that buffer is
 * undefined. memcpy into a local is the portable read and compiles to the
 * same instruction the kernel's accessor would. */
static inline uint16_t smc_proxy_rd16(const uint8_t *p)
{
	__le16 v;

	memcpy(&v, p, sizeof(v));
	return le16_to_cpu(v);
}

static inline uint32_t smc_proxy_rd32(const uint8_t *p)
{
	__le32 v;

	memcpy(&v, p, sizeof(v));
	return le32_to_cpu(v);
}

/**
 * smc_proxy_drain_inherited() - consume answers a previous session left behind.
 * @inst: the freshly created instance, before its reader runs.
 *
 * Only a firmware boot clears the card's completion ring, so a driver that
 * attaches without one inherits whatever the last session did not collect.
 * This pool is empty and its cid cursor starts at zero, so such an answer
 * carries a cid the first request is about to claim -- and would be applied to
 * a request that never sent it. Taken here, where nothing is waiting and every
 * entry is by definition stale.
 */
static void smc_proxy_drain_inherited(struct smc_proxy_instance *inst)
{
	uint8_t entry[SMI_COMPLETION_FRAME_BYTES];
	unsigned int taken = 0;

	/* A ring cannot hold more unanswered requests than there are cids. */
	while (taken < SMC_PROXY_MAX_CIDS) {
		uint32_t len = sizeof(entry);

		if (inst->fw_if_handle->read(inst->fw_if_handle, 0, entry, &len,
					     0) != FW_IF_ERRORS_NONE)
			break;
		taken++;
	}

	if (taken)
		PR_INFO("inst%u: dropped %u answer(s) an earlier session left",
			inst->proxy_id, taken);
}

/**
 * smc_proxy_cmd_malformed() - end a request whose answer was not an envelope.
 * @inst: the proxy instance
 * @entry: what did arrive
 * @len: how many bytes of it there were
 *
 * Such a frame still names its request as long as the cid halfword arrived,
 * so that request ends with -EPROTO; one too short for a cid is only a line.
 */
static void smc_proxy_cmd_malformed(struct smc_proxy_instance *inst,
				    const uint8_t *entry, uint32_t len)
{
	struct smi_pending *cmd = NULL;
	uint16_t cid;

	if (!inst || !entry || len < sizeof(uint16_t)) {
		PR_ERR("Response of %u bytes carries no cid", len);
		return;
	}

	cid = smc_proxy_rd16(entry);
	if (cid >= SMC_PROXY_MAX_CIDS) {
		PR_ERR("Malformed response (%u bytes) names cid %u", len, cid);
		return;
	}

	mutex_lock(&(inst->lock));
	cmd = &inst->cmds[cid];
	if (cmd->state == SMC_CMD_LIVE) {
		cmd->cmd_rcode = -EPROTO;
		cmd->state = SMC_CMD_DONE;
		PR_ERR("[" SMC_CID_FMT
		       "] answer was %u bytes, not one envelope",
		       SMC_CID_ARG(inst, cid), len);
		if (inst->event_cb)
			inst->event_cb(inst->proxy_id,
				       SMC_PROXY_EVENT_RESPONSE_COMPLETE, cmd);
	}
	mutex_unlock(&(inst->lock));
}

/**
 * smc_proxy_cmd_complete() - handle a completion-queue response.
 * @inst: the proxy instance
 * @entry: one completion envelope, unparsed
 *
 * Looks up the command slot by cid (O(1)) and, if it is live, copies the
 * response and notifies the waiter. Runs under inst->lock so it serialises
 * against abort/drain; the slot only leaves SMC_CMD_LIVE here.
 */
static void smc_proxy_cmd_complete(struct smc_proxy_instance *inst,
				   const uint8_t *entry)
{
	struct smi_pending *cmd = NULL;
	uint16_t cid;

	if (!inst || !entry)
		return;

	/* The cid is the first halfword in EVERY generation, which is why this
	 * layer matches a slot without knowing which one answered. */
	cid = smc_proxy_rd16(entry);
	if (cid >= SMC_PROXY_MAX_CIDS) {
		PR_ERR("Response cid %u out of range", cid);
		return;
	}

	mutex_lock(&(inst->lock));
	cmd = &inst->cmds[cid];

	/* An answer nobody waits for any more: it frees the cid, and the owner
	 * takes back what it lent the request. */
	if ((cmd->state == SMC_CMD_LIVE) && cmd->orphaned) {
		cmd->orphaned = false;
		cmd->state = SMC_CMD_FREE;
		clear_bit(cid, inst->tags);
		PR_DBG("[" SMC_CID_FMT "] answered after its waiter left",
		       SMC_CID_ARG(inst, cid));
		if (inst->event_cb)
			inst->event_cb(inst->proxy_id,
				       SMC_PROXY_EVENT_ORPHAN_ANSWERED, cmd);
		mutex_unlock(&(inst->lock));
		return;
	}

	/* Only a live (published, not-yet-handled) command can be completed. A
	 * RESERVED/FREE/DONE slot means the response raced publish or arrived
	 * after the command was already handled/released - drop it.
	 */
	if (cmd->state != SMC_CMD_LIVE) {
		if (inst->draining)
			PR_DBG("[" SMC_CID_FMT
			       "] late response after shutdown drain",
			       SMC_CID_ARG(inst, cid));
		else if (cmd->expired)
			PR_DBG("[" SMC_CID_FMT
			       "] answer arrived after we gave up on it",
			       SMC_CID_ARG(inst, cid));
		else if (cmd->state == SMC_CMD_FREE)
			/* A cid this session never claimed: an answer left by
			 * an earlier one, arriving after the attach drain. */
			PR_INFO("[" SMC_CID_FMT
				"] answer for a request this session never sent",
				SMC_CID_ARG(inst, cid));
		else
			PR_ERR("[" SMC_CID_FMT "] unexpected response",
			       SMC_CID_ARG(inst, cid));
		mutex_unlock(&(inst->lock));
		return;
	}

	/* Copy the completion before ending the matchable window -- as it
	 * arrived, not parsed. Which fields those bytes have is the generation's
	 * answer, and this layer does not know the generation: it matched the
	 * slot by the cid at bytes [0,2), which both generations put there, and
	 * that is all it needs to know.
	 */
	memcpy(cmd->completion, entry, sizeof(cmd->completion));

	/* Suppress heartbeat message so as not to flood dmesg. Logged as words
	 * for the same reason: naming the fields would be reading them. */
	if (cmd->cmd_suppress_dbg == false)
		PR_DBG("[" SMC_CID_FMT "] completion %08x %08x %08x %08x",
		       SMC_CID_ARG(inst, cid), smc_proxy_rd32(entry),
		       smc_proxy_rd32(entry + 4), smc_proxy_rd32(entry + 8),
		       smc_proxy_rd32(entry + 12));

	/* End the matchable window (LIVE -> DONE). The cid stays reserved until
	 * the owner releases the slot, so a late/duplicate response for THIS use
	 * is rejected (slot no longer LIVE). A cid reused after release+re-claim
	 * is only protected by the cyclic reuse distance (probabilistic).
	 */
	cmd->state = SMC_CMD_DONE;

	if (inst->event_cb)
		inst->event_cb(inst->proxy_id,
			       SMC_PROXY_EVENT_RESPONSE_COMPLETE, cmd);

	mutex_unlock(&(inst->lock));
}

/**
 * smc_proxy_deadline_pending() - is a command still inside its granted deadline?
 *
 * A request carries the deadline its row names -- 30 s for a download, an hour
 * for a partition copy. While one of those is still running, silence is time
 * this driver granted, not evidence of death.
 *
 * Return: true while any live command has time left.
 */
bool smc_proxy_deadline_pending(struct smc_proxy_instance *inst)
{
	bool pending = false;
	uint16_t cid;

	if (!inst)
		return false;

	mutex_lock(&(inst->lock));
	for (cid = 0; cid < SMC_PROXY_MAX_CIDS; cid++) {
		const struct smi_pending *cmd = &inst->cmds[cid];

		if ((cmd->state == SMC_CMD_LIVE) &&
		    time_before(jiffies,
				(unsigned long)cmd->cmd_timeout_jiffies)) {
			pending = true;
			break;
		}
	}
	mutex_unlock(&(inst->lock));

	return pending;
}

/**
 * smc_proxy_pending_drain() - end every live command in the pool (shutdown).
 *
 * @inst: the proxy instance
 *
 * Each waiter wakes with -ESHUTDOWN, LIVE -> DONE, and its owner frees the cid;
 * an orphaned command has no waiter, and its cid goes with the instance.
 */
static void smc_proxy_pending_drain(struct smc_proxy_instance *inst)
{
	struct smi_pending *cmd = NULL;
	uint16_t cid;

	if (!inst)
		return;

	mutex_lock(&(inst->lock));
	/* Latch shutdown so a late HW response for an already-drained CID is
	 * logged as expected drain noise instead of an unexpected-response error.
	 */
	inst->draining = true;

	for (cid = 0; cid < SMC_PROXY_MAX_CIDS; cid++) {
		cmd = &inst->cmds[cid];
		if ((cmd->state != SMC_CMD_LIVE) || cmd->orphaned)
			continue;

		/* Complete now so kthread_stop() waits no deadline; the TIMEOUT
		 * event carries it, and cmd_rcode says it was shutdown. */
		cmd->state = SMC_CMD_DONE;
		cmd->cmd_rcode = -ESHUTDOWN;
		PR_CRIT_WARN("[" SMC_CID_FMT "] aborted (shutdown drain)",
			     SMC_CID_ARG(inst, cid));

		if (inst->event_cb)
			inst->event_cb(inst->proxy_id,
				       SMC_PROXY_EVENT_RESPONSE_TIMEOUT, cmd);
	}
	mutex_unlock(&(inst->lock));
}

/**
 * smc_proxy_pending_remove() - abort a specific in-flight command.
 *
 * @inst: the proxy instance
 * @ccmd: the command to abort (a slot in this instance's pool)
 *
 * Ends the command's matchable window (LIVE -> DONE) without invoking the
 * callback, so the response thread will not touch it. Identity-safe: only acts
 * if the slot is still this command's live submission; if completion/drain
 * already handled it, it is left for the owner's release.
 */
static void smc_proxy_pending_remove(struct smc_proxy_instance *inst,
				     struct smi_pending *ccmd)
{
	if (!inst || !ccmd)
		return;
	if (ccmd->cmd_cid >= SMC_PROXY_MAX_CIDS)
		return;

	mutex_lock(&(inst->lock));
	if (ccmd->state == SMC_CMD_LIVE) {
		ccmd->state = SMC_CMD_DONE;
		ccmd->cmd_rcode = -EIO;
		PR_DBG("[" SMC_CID_FMT "] removed",
		       SMC_CID_ARG(inst, ccmd->cmd_cid));
	}
	mutex_unlock(&(inst->lock));
}

/**
 * smc_proxy_pending_check_timeout() - check for any timed out requests
 *
 * @inst: the proxy instance
 *
 * Notifies (but does not retire) live commands past their deadline; the waiter
 * then aborts the command, which transitions it to DONE.
 */
static void smc_proxy_pending_check_timeout(struct smc_proxy_instance *inst)
{
	struct smi_pending *cmd = NULL;
	uint16_t cid;

	if (!inst)
		return;

	mutex_lock(&(inst->lock));
	for (cid = 0; cid < SMC_PROXY_MAX_CIDS; cid++) {
		cmd = &inst->cmds[cid];
		if ((cmd->state != SMC_CMD_LIVE) || cmd->orphaned)
			continue;

		/* Finding timed out cmds */
		if (time_before(cmd->cmd_timeout_jiffies, jiffies)) {
			/* The caller names the verb and decides what a
			 * timeout means for it; this layer only knows a
			 * slot expired, and whether the lane needs a reset
			 * is not something one expiry establishes. */
			PR_DBG("[" SMC_CID_FMT "] timed out(timeout)",
			       SMC_CID_ARG(inst, cid));
			cmd->cmd_rcode = -ETIME;
			cmd->expired = true;
			if (inst->event_cb)
				inst->event_cb(inst->proxy_id,
					       SMC_PROXY_EVENT_RESPONSE_TIMEOUT,
					       cmd);
		}
	}
	mutex_unlock(&(inst->lock));
}

/**
 * complete_response_thread() - the response thread
 *
 * @data: the data pointer to the proxy instance
 *
 * Thread to check if response queue has new command to consume.
 * If there is one, completed it by reading entry, performing
 * callback and notifying peer
 *
 * Return: the errno return code
 */
static int complete_response_thread(void *data)
{
	uint8_t ccmd[SMI_COMPLETION_FRAME_BYTES];
	uint32_t ccmd_size;
	struct smc_proxy_instance *smc_proxy_inst = NULL;
	bool response_failed = false;

	if (!data) {
		PR_ERR("Response thread null data arg");
		response_failed = true;
	} else {
		smc_proxy_inst = (struct smc_proxy_instance *)data;
	}

	while (1) {
		if (response_failed == false) {
			/* read() reports what it delivered through this same
			 * word, so the room is restated every pass. */
			ccmd_size = sizeof(ccmd);

			/* Perform the read from the FW_IF */
			if (smc_proxy_inst->fw_if_handle->read(
				    smc_proxy_inst->fw_if_handle, 0, ccmd,
				    &ccmd_size, 0) == FW_IF_ERRORS_NONE) {
				/* A completion is exactly one envelope, so a
				 * short frame is not a small answer -- the
				 * bytes behind it are the last one's. */
				if (ccmd_size == sizeof(ccmd))
					smc_proxy_cmd_complete(smc_proxy_inst,
							       ccmd);
				else
					smc_proxy_cmd_malformed(smc_proxy_inst,
								ccmd,
								ccmd_size);
			}

			/* Check for any commands that might have timed out & notify via callback */
			smc_proxy_pending_check_timeout(smc_proxy_inst);
		}

		/* Check if a stop has been requested */
		if (kthread_should_stop()) {
			break;
		}

		usleep_range(1000, 2000);
	}

	/* Return will be passed to kthread_stop() */
	return 0;
}

/*****************************************************************************/
/* Public functions                                                          */
/*****************************************************************************/

/* Idempotent: smc_proxy_close() no-ops if the proxy is already closed. */
static void smc_proxy_devm_stop(void *data)
{
	smc_proxy_close((struct smc_proxy_instance *)data);
}

/*
 * Initialise the SMC proxy layer for one transport instance. The instance is
 * devm-allocated on @dev so its lifetime is the device's (see smc_proxy_close).
 */
struct smc_proxy_instance *smc_proxy_init(struct device *dev,
					  FW_IF_CFG *fw_if_handle,
					  uint32_t tx_capacity)
{
	const char *ap_thread_name = "smc proxy";
	struct smc_proxy_instance *inst = NULL;
	int ret;

	if (!dev || !fw_if_handle || !tx_capacity)
		return NULL;

	if (fw_if_handle->open(fw_if_handle) != FW_IF_ERRORS_NONE) {
		PR_ERR("FW_IF open request failed");
		return NULL;
	}

	/* Device-lived: reclaimed by devm at detach, never by smc_proxy_close. */
	inst = devm_kzalloc(dev, sizeof(*inst), GFP_KERNEL);
	if (!inst) {
		PR_ERR("Unable to allocate memory for smc proxy instance");
		fw_if_handle->close(fw_if_handle);
		return NULL;
	}

	inst->fw_if_handle = fw_if_handle;
	inst->tx_cap = tx_capacity;
	/* Per-instance id for logging; unique across devices. The cmds[] pool
	 * and tags bitmap start zeroed (all FREE) from devm_kzalloc; each
	 * slot's completion is initialised on claim.
	 */
	inst->proxy_id = (uint8_t)(atomic_inc_return(&smc_proxy_next_id) - 1);
	mutex_init(&inst->lifecycle_lock);
	mutex_init(&inst->lock);

	/* Create thread to handle command responses */
	inst->response_thread =
		kthread_create(complete_response_thread, inst, ap_thread_name);
	if (IS_ERR(inst->response_thread)) {
		ret = PTR_ERR(inst->response_thread);
		PR_ERR("Unable to create the %s thread: %d", ap_thread_name,
		       ret);
		inst->response_thread = NULL;
		fw_if_handle->close(fw_if_handle);
		return NULL; /* inst stays devm-allocated but unused */
	}

	/* Before the reader runs and before anything can be submitted: what the
	 * ring already holds belongs to a session that is gone. */
	smc_proxy_drain_inherited(inst);

	wake_up_process(inst->response_thread);
	inst->initialised = true;

	/* Registered last so LIFO devres release stops the thread before the
	 * instance and the transport it uses are freed. */
	if (devm_add_action_or_reset(dev, smc_proxy_devm_stop, inst)) {
		PR_ERR("Unable to register proxy cleanup action");
		return NULL;
	}

	PR_DBG("Successfully created %s kernel thread", ap_thread_name);
	return inst;
}

/*
 * Bind in a callback to be invoked when an event occurs
 */
int smc_proxy_bind_callback(struct smc_proxy_instance *inst,
			    smc_proxy_event_callback *event_cb)
{
	if (!inst || !event_cb)
		return -EINVAL;

	mutex_lock(&inst->lifecycle_lock);
	if (!inst->initialised) {
		mutex_unlock(&inst->lifecycle_lock);
		return -EPERM;
	}

	inst->event_cb = event_cb;
	mutex_unlock(&inst->lifecycle_lock);
	return 0;
}

/*
 * Stop the proxy service: drain in-flight commands, stop the response thread
 * and close the FAL handle. Does NOT free the instance (devm-owned). Idempotent.
 */
int smc_proxy_close(struct smc_proxy_instance *inst)
{
	int ret = 0;

	if (!inst)
		return -EPERM;

	/* Lock order is lifecycle_lock -> lock. Submit/drain/close all use this
	 * order, while completion/tag-release use only the command-pool lock.
	 */
	mutex_lock(&inst->lifecycle_lock);
	if (!inst->initialised) {
		mutex_unlock(&inst->lifecycle_lock);
		return -EPERM; /* already closed or never initialised */
	}

	/* Mark closed first so a concurrent/second close is a no-op (the
	 * fatal-event and remove paths may both reach here).
	 */
	inst->initialised = false;

	/*
	 * Drain the submitted command queue before killing the worker thread.
	 * force=true so we do not wait up to cmd_timeout_jiffies per entry on
	 * the shutdown path.
	 */
	smc_proxy_pending_drain(inst);

	if (inst->response_thread) {
		/* Its result is the thread's own, or -EINTR if it never ran */
		kthread_stop(inst->response_thread);
		inst->response_thread = NULL;
	}

	/* Once per initialised instance, and @initialised gated the entry
	 * above: the handle is opened before the thread exists, and every
	 * failed bring-up closes it on its way out. */
	if (inst->fw_if_handle->close(inst->fw_if_handle)) {
		PR_ERR("FW_IF close request failed");
		ret = -EIO;
	}

	mutex_unlock(&inst->lifecycle_lock);

	/* No kfree: the instance is devm-owned and reclaimed at device detach. */
	return ret;
}

bool smc_proxy_orphan(struct smi_pending *cmd)
{
	struct smc_proxy_instance *inst = cmd ? cmd->inst : NULL;
	bool orphaned = false;

	if (!inst || cmd->cmd_cid >= SMC_PROXY_MAX_CIDS)
		return false;

	mutex_lock(&(inst->lock));
	if (cmd->state == SMC_CMD_LIVE) {
		cmd->orphaned = true;
		orphaned = true;
	}
	mutex_unlock(&(inst->lock));

	return orphaned;
}

/*
 * Abort an in progress request
 */
int smc_proxy_request_abort(struct smi_pending *cmd)
{
	if (!cmd)
		return -EINVAL;

	if (cmd->inst)
		smc_proxy_pending_remove(cmd->inst, cmd);

	return 0;
}

/*
 * Force-drain every in-flight command on @inst. Each waiter is woken with
 * -ESHUTDOWN via the registered event callback.
 */
int smc_proxy_drain_all(struct smc_proxy_instance *inst)
{
	if (!inst)
		return -EINVAL;

	mutex_lock(&inst->lifecycle_lock);
	if (!inst->initialised) {
		mutex_unlock(&inst->lifecycle_lock);
		return -ENODEV;
	}

	smc_proxy_pending_drain(inst);
	mutex_unlock(&inst->lifecycle_lock);
	return 0;
}

/*
 * Claim a command slot (tag) from the instance's pre-allocated pool. Allocates
 * a cid cyclically from @next_cid, re-initialises the slot, and returns it ready
 * to populate. The slot must be returned with smc_proxy_tag_release().
 */
struct smi_pending *smc_proxy_cid_claim(struct smc_proxy_instance *inst)
{
	struct smi_pending *cmd = NULL;
	unsigned long cid;

	if (!inst)
		return NULL;

	mutex_lock(&inst->lifecycle_lock);
	if (!inst->initialised) {
		mutex_unlock(&inst->lifecycle_lock);
		return NULL;
	}

	mutex_lock(&(inst->lock));

	/* Cyclic first-free search: from the cursor, then wrap to the start. */
	cid = find_next_zero_bit(inst->tags, SMC_PROXY_MAX_CIDS,
				 inst->next_cid);
	if (cid >= SMC_PROXY_MAX_CIDS)
		cid = find_first_zero_bit(inst->tags, SMC_PROXY_MAX_CIDS);
	if (cid >= SMC_PROXY_MAX_CIDS) {
		mutex_unlock(&(inst->lock));
		mutex_unlock(&inst->lifecycle_lock);
		PR_ERR("inst%u: command tag pool exhausted",
		       (unsigned int)inst->proxy_id);
		return NULL;
	}

	set_bit(cid, inst->tags);
	inst->next_cid = (cid + 1) % SMC_PROXY_MAX_CIDS;

	/* Reset the reused slot, then mark it reserved. */
	cmd = &inst->cmds[cid];
	*cmd = (struct smi_pending){ 0 };
	init_completion(&cmd->cmd_complete);
	cmd->cmd_cid = cid;
	cmd->inst = inst;
	cmd->expired = false;
	cmd->state = SMC_CMD_RESERVED;

	mutex_unlock(&(inst->lock));
	mutex_unlock(&inst->lifecycle_lock);
	return cmd;
}

/*
 * Release a command slot claimed via smc_proxy_cid_claim(), freeing its tag for
 * reuse. Must be the last proxy call for @cmd, and called exactly once per
 * claim: a second call after the tag was re-claimed would free another
 * command's tag.
 */
void smc_proxy_tag_release(struct smi_pending *cmd)
{
	struct smc_proxy_instance *inst;

	if (!cmd || !cmd->inst)
		return;
	inst = cmd->inst;

	if (cmd->cmd_cid >= SMC_PROXY_MAX_CIDS)
		return;

	mutex_lock(&(inst->lock));
	cmd->state = SMC_CMD_FREE;
	clear_bit(cmd->cmd_cid, inst->tags);
	mutex_unlock(&(inst->lock));
}

/**
 * smc_proxy_submit() - shared request submit path for every opcode.
 * @cmd: the proxy command structure (carries cid + FAL handle)
 * @opcode: the SMC opcode to frame into the request header
 * @req: caller-filled request (payload set; header filled here)
 * @count: payload byte count for the request header
 *
 * The per-opcode wrappers only populate @req's payload union member; this
 * helper stamps the common header (state/opcode/count/cid), writes the fixed-
 * size request to the FAL, and publishes the slot as live. The wire write is
 * always sizeof(*req) regardless of @count, matching the firmware contract.
 *
 * Return: 0 on success, -EPERM if the proxy instance is gone, -EIO on FAL write
 * failure.
 */
int smc_proxy_submit(struct smi_pending *cmd, const void *frame, uint16_t len)
{
	struct smc_proxy_instance *inst = NULL;
	int ret;

	if (!cmd || !frame || len == 0)
		return -EINVAL;

	inst = cmd->inst;
	if (!inst)
		return -EPERM;

	if (len > inst->tx_cap) {
		PR_ERR("[" SMC_CID_FMT "] %u B request, medium carries %u B",
		       SMC_CID_ARG(inst, cmd->cmd_cid), len, inst->tx_cap);
		return -EINVAL;
	}

	mutex_lock(&inst->lifecycle_lock);
	if (!inst->initialised) {
		mutex_unlock(&inst->lifecycle_lock);
		return -EPERM;
	}

	/* A full ring is backpressure, not a failure. The medium reports its own
	 * depth this way -- the GCQ ring holds four requests, and this driver has
	 * four concurrent senders in flash, heartbeat, logging and sysfs -- and
	 * the consumer empties it every slice, so the room comes back. Answering
	 * -EIO the moment it filled was the one place a transient full ring
	 * became a failed command.
	 *
	 * The wait is bounded by the deadline this command already has; no second
	 * clock. Any other error is immediate, as before.
	 *
	 * lifecycle_lock is DROPPED across the sleep: it serialises every submit plus
	 * bind, drain and close, and the wait is the command's own deadline -- 30s for
	 * a download, an hour for a partition copy. Held, one backpressured request
	 * would block the heartbeat's submit and hang close() for that long. The write
	 * itself still runs under the lock, so submits stay serialised; re-acquiring
	 * re-asks what the lock protects, since a close may have landed meanwhile.
	 */
	while (1) {
		ret = inst->fw_if_handle->write(inst->fw_if_handle, 0,
						(uint8_t *)frame, len, 0);
		if (ret == FW_IF_ERRORS_NONE)
			break;

		if (ret != FW_IF_GCQ_ERRORS_DRIVER_PRODUCER_NO_FREE_SLOTS ||
		    !time_before(jiffies,
				 (unsigned long)cmd->cmd_timeout_jiffies)) {
			PR_ERR("FW_IF write request failed: %d", ret);
			/* Slot stays RESERVED; freed by the owner's release. */
			mutex_unlock(&inst->lifecycle_lock);
			return (ret ==
				FW_IF_GCQ_ERRORS_DRIVER_PRODUCER_NO_FREE_SLOTS)
				       ? -EBUSY
				       : -EIO;
		}

		mutex_unlock(&inst->lifecycle_lock);
		usleep_range(SMC_PROXY_RING_FULL_US,
			     2 * SMC_PROXY_RING_FULL_US);
		mutex_lock(&inst->lifecycle_lock);

		if (!inst->initialised) {
			/* Closed while we waited. The slot stays RESERVED and
			 * the owner's release frees it, exactly as on the error
			 * path above.
			 */
			mutex_unlock(&inst->lifecycle_lock);
			return -EPERM;
		}
	}

	/* Publish: RESERVED -> LIVE, now matchable by a response. */
	mutex_lock(&(inst->lock));
	cmd->state = SMC_CMD_LIVE;
	mutex_unlock(&(inst->lock));
	mutex_unlock(&inst->lifecycle_lock);
	return 0;
}
