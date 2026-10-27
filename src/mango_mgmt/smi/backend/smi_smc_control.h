// SPDX-License-Identifier: GPL-2.0-only
/*
 * smi_smc_control.h - This file contains SMC control defintions.
 *
 * Copyright (c) 2023 - 2025 Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_SMC_CONTROL_H
#define SMI_SMC_CONTROL_H

#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/types.h>

#include "transport/smi_boot.h" /* struct smi_span */

#include "smc_proxy.h"
#include "smi_api.h"
#include "smi_cmd_id.h" /* SMC_CMD_ID_* (shared, S3-neutral) */
#include "smi_facts.h" /* what the session settles on */
#include "smi_log.h" /* SMI_DEV_* logging macros */
#include "smi_sensor_model.h" /* what a card's sensors are here */
#include "smi_verb.h" /* a verb by name, and what its errno means */

struct smi_init_data;
struct smi_cmd;
struct pf_dev_struct;

/* The session. Its body is the backend's own; see smi_smc_internal.h. */
struct smc_control_ctx;

/******************************************************************************/
/* Defines                                                                    */
/******************************************************************************/

/* Call sites keep the SMI_*(ctxt, ...) spelling over the device-based SMI_DEV_*
 * macros (smi_log.h), so the command engine can log on a bare struct device *
 * without this context. */
/* The body of struct smc_control_ctx is the backend's own (smi_smc_internal.h);
 * above it the session is a handle, and this is the one thing read off it. */
struct device *smi_smc_dev(struct smc_control_ctx *smc);

#define SMI_ERR(smc_ctx, fmt, arg...)                                          \
	SMI_DEV_ERR(smi_smc_dev(smc_ctx), fmt, ##arg)
#define SMI_ERR_ONCE(smc_ctx, fmt, arg...)                                     \
	SMI_DEV_ERR_ONCE(smi_smc_dev(smc_ctx), fmt, ##arg)
#define SMI_ERR_RATELIMITED(smc_ctx, fmt, arg...)                              \
	SMI_DEV_ERR_RATELIMITED(smi_smc_dev(smc_ctx), fmt, ##arg)
#define SMI_INFO(smc_ctx, fmt, arg...)                                         \
	SMI_DEV_INFO(smi_smc_dev(smc_ctx), fmt, ##arg)
#define SMI_WARN(smc_ctx, fmt, arg...)                                         \
	SMI_DEV_WARN(smi_smc_dev(smc_ctx), fmt, ##arg)
#define SMI_DBG(smc_ctx, fmt, arg...)                                          \
	SMI_DEV_DBG(smi_smc_dev(smc_ctx), fmt, ##arg)
#define SMI_VDBG(smc_ctx, fmt, arg...)                                         \
	SMI_DEV_VDBG(smi_smc_dev(smc_ctx), fmt, ##arg)
#define SMI_SMC_LOG(smc_ctx, fmt, arg...)                                      \
	SMI_DEV_SMC_LOG(smi_smc_dev(smc_ctx), fmt, ##arg)

/******************************************************************************/
/* Enums                                                                      */
/******************************************************************************/

/**
 * enum smc_event_id - the event id
 * @SMC_EVENT_ID_HEARTBEAT_EXPIRED: heartbeat expired
 * @SMC_EVENT_ID_HEARTBEAT_VALIDATION: heartbeat validated
 * @SMC_EVENT_ID_HEARTBEAT_FATAL: heartbeat failure count has reached threshold
 * @SMC_EVENT_ID_CARD_RESTARTED: the card rebooted under this session
 * @SMC_EVENT_ID_POLL: the periodic tick, for whatever the host does with it
 */
enum smc_event_id {
	SMC_EVENT_ID_HEARTBEAT_EXPIRED = 0,
	SMC_EVENT_ID_HEARTBEAT_VALIDATION,
	SMC_EVENT_ID_HEARTBEAT_FATAL,
	SMC_EVENT_ID_CARD_RESTARTED,
	SMC_EVENT_ID_POLL,
	SMC_EVENT_ID_MAX,
};

/*****************************************************************************/
/* Typedefs                                                                  */
/*****************************************************************************/

/**
 * typedef smc_event_callback - the event callback's signature.
 * @id: the event id
 * @data: private callback data, optional
 */
typedef void (*smc_event_callback)(enum smc_event_id id, void *data);

/******************************************************************************/
/* Public Functions                                                           */
/******************************************************************************/

struct smi_slice_result;

/**
 * smi_submit_command_slice() - Submit a request and wait for the response.
 * @smc_ctx: Pointer to top level SMC data struct.
 * @cmd_id: SMC_CMD_ID_*, or an id the settled generation names for itself;
 *	    selects the descriptor and payload.
 * @flags: The command specific flags; sensor traffic carries the rid here.
 * @arg: Parameters @flags cannot hold, in the shape the row knows, else NULL.
 * @data_buf: Data buffer to either store payload data or response data.
 * @data_size: Data buffer size.
 * @slice: Optional; what the answer said about the slice it was given.
 *
 * Return: 0 or negative error code.
 */
int smi_submit_command_slice(struct smc_control_ctx *smc_ctx, int cmd_id,
			     uint32_t flags, const void *arg, uint8_t *data_buf,
			     uint32_t data_size,
			     struct smi_slice_result *slice);

/* The same, for a caller whose answer is bounded by what it asked for. */
static inline int smi_submit_command(struct smc_control_ctx *smc_ctx,
				     int cmd_id, uint32_t flags,
				     uint8_t *data_buf, uint32_t data_size)
{
	return smi_submit_command_slice(smc_ctx, cmd_id, flags, NULL, data_buf,
					data_size, NULL);
}

/* A verb the request names, through the same gate: see smi_cmd_send_verb(). */
int smi_smc_call(struct smc_control_ctx *smc, const struct smi_verb_req *req,
		 struct smi_verb_ans *ans);

/* The card's Verb record for @opcode, and the largest slice one request can
 * claim. */
bool smi_smc_verb(struct smc_control_ctx *smc, uint16_t opcode,
		  struct smi_desc_verb *out);
uint32_t smi_smc_slice_bytes(struct smc_control_ctx *smc);

/* A command whose flags word carries nothing. */
#define SMC_CMD_FLAG_NONE 0x00

/*----------------------------------------------------------------------------
 * The session, as the layers above ask about it
 *
 * Everything a card can be asked or told goes through these. The context body
 * is the backend's own, and a NULL session is answered here rather than by
 * every caller: -ENODEV where there is a request to refuse, and otherwise
 * the empty answer -- zero, false, or nothing done.
 *--------------------------------------------------------------------------*/

/* Fills @out with what the session settled on; zeroed where there is none. */
void smi_smc_facts(struct smc_control_ctx *smc, struct smi_smc_facts *out);

/* The published plane: the sections the card leaves in the window unasked,
 * each named by its Region kind and instance. */
const struct smi_span *smi_smc_pub_span(struct smc_control_ctx *smc,
					uint16_t kind, uint16_t inst);
int smi_smc_pub_read(struct smc_control_ctx *smc, const struct smi_span *sec,
		     void *dst, size_t len);
int smi_smc_pub_status_head(struct smc_control_ctx *smc,
			    struct smi_pub_status_head *out);
int smi_smc_tool_records(struct smc_control_ctx *smc, uint8_t **out,
			 size_t *len);

/* Repositories, into the neutral model the store holds. */
int smi_smc_repo_read(struct smc_control_ctx *smc,
		      const struct smi_repo_desc *desc, struct smi_repo *repo);
bool smi_smc_publishes_values(struct smc_control_ctx *smc);

/* Blink the locate beacon for @seconds, 0 turning it off. */
int smi_smc_locate(struct smc_control_ctx *smc, uint16_t seconds);

/* Session lifetime, for the one caller that owns it. */
void smi_smc_quiesce(struct smc_control_ctx *smc);
void smi_smc_disconnect(struct smc_control_ctx *smc);
void smi_smc_discovery_done(struct smc_control_ctx *smc);

/* Is a request still inside its deadline? A sweep that failed while one was
 * in flight has learnt nothing about the card. */
bool smi_smc_deadline_pending(struct smc_control_ctx *smc);

/**
 * smi_smc_teardown() - Stop the session and free pf_dev->smc_ctx.
 * @pf_dev: The SMI device.
 *
 * Return: 0, or -EINVAL without a device.
 */
int smi_smc_teardown(struct pf_dev_struct *pf_dev);

/**
 * smi_smc_setup() - Bring up the SMC session into pf_dev->smc_ctx.
 * @pf_dev: The SMI device; handed to @event_cb as its data.
 * @init_data: Message-layer windows and backend type.
 * @event_cb: Callback to be invoked when event occurs.
 *
 * Return: 0 or negative error code.
 */
int smi_smc_setup(struct pf_dev_struct *pf_dev, struct smi_init_data *init_data,
		  smc_event_callback event_cb);

#endif /* SMI_SMC_CONTROL_H */
