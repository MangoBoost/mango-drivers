/**
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * This file containing the FW IF RPMsg abstraction implementation.
 *
 * @file fw_if_rpmsg.c
 *
 */

/*****************************************************************************/
/* Includes                                                                  */
/*****************************************************************************/
#include <linux/wait.h>
#include <linux/kfifo.h>
#include <linux/slab.h>
#include <linux/export.h>

#include "fw_if_rpmsg.h"

#include "contract/smi/smi_abi.h" /* SMI_COMPLETION_FRAME_BYTES -- the completion slot */

#ifdef CONFIG_OF
#include <linux/rpmsg.h>

/*****************************************************************************/
/* Public Functions                                                          */
/*****************************************************************************/

/*
 * RPMSG_RX_ITEM_MAX_LEN: maximum payload size per RX item (bytes).
 * The protocol response size itself, read from the shared ABI header that owns
 * it rather than restated here: the kfifo footprint stays minimal and an
 * oversized payload surfaces as early as possible via pr_warn. The response
 * frame IS a completion slot, which smi_abi.h asserts.
 */
#define RPMSG_RX_ITEM_MAX_LEN SMI_COMPLETION_FRAME_BYTES
#define RPMSG_RX_FIFO_COUNT 16

struct rpmsg_rx_item {
	uint32_t len;
	uint8_t data[RPMSG_RX_ITEM_MAX_LEN];
};

static DEFINE_SPINLOCK(rpmsg_rx_lock);
static DEFINE_KFIFO(rpmsg_rx_fifo, struct rpmsg_rx_item, RPMSG_RX_FIFO_COUNT);
static DECLARE_WAIT_QUEUE_HEAD(rpmsg_rx_wq);
static bool rpmsg_rx_fifo_init;

/**
 * fw_if_rpmsg_rx_push() - Push received payload into FAL RX queue (for read() path).
 * Called from smi_rpmsg_cb() so that rpmsg_read() can return data.
 *
 * @data: payload pointer from rpmsg callback
 * @len:  payload length
 */
void fw_if_rpmsg_rx_push(const uint8_t *data, uint32_t len)
{
	struct rpmsg_rx_item item;
	unsigned long flags;

	if (!data || !rpmsg_rx_fifo_init)
		return;

	if (len > RPMSG_RX_ITEM_MAX_LEN) {
		pr_warn_ratelimited(
			"fw_if_rpmsg: RX payload too large (%u > %u), dropping\n",
			len, RPMSG_RX_ITEM_MAX_LEN);
		return;
	}

	item.len = len;
	memcpy(item.data, data, len);

	spin_lock_irqsave(&rpmsg_rx_lock, flags);
	kfifo_in(&rpmsg_rx_fifo, &item, 1);
	spin_unlock_irqrestore(&rpmsg_rx_lock, flags);
	wake_up_interruptible(&rpmsg_rx_wq);
}

/*****************************************************************************/
/* Helpers                                                                   */
/*****************************************************************************/

static void rpmsg_log_hex(const char *label, const uint8_t *data, uint32_t len)
{
	/* %*ph prints up to 64 bytes as "aa bb cc ..."-style hex */
	pr_debug("%s(%u) : %*ph%s\n", label, len, min_t(uint32_t, len, 64),
		 data, len > 64 ? " ..." : "");
}

/*****************************************************************************/
/* FAL ops                                                                   */
/*****************************************************************************/

static uint32_t rpmsg_open(void *pvFwIf)
{
	unsigned long flags;

	spin_lock_irqsave(&rpmsg_rx_lock, flags);
	kfifo_reset(&rpmsg_rx_fifo);
	rpmsg_rx_fifo_init = true;
	spin_unlock_irqrestore(&rpmsg_rx_lock, flags);

	return FW_IF_ERRORS_NONE;
}

static uint32_t rpmsg_close(void *pvFwIf)
{
	unsigned long flags;

	spin_lock_irqsave(&rpmsg_rx_lock, flags);
	kfifo_reset(&rpmsg_rx_fifo);
	rpmsg_rx_fifo_init = false;
	spin_unlock_irqrestore(&rpmsg_rx_lock, flags);

	return FW_IF_ERRORS_NONE;
}

static uint32_t rpmsg_write(void *pvFwIf, uint64_t ullDstPort, uint8_t *pucData,
			    uint32_t ulSize, uint32_t ulTimeoutMs)
{
	FW_IF_CFG *cfg = (FW_IF_CFG *)pvFwIf;
	FW_IF_RPMSG_CFG *rpmsg_cfg;
	struct rpmsg_device *rpdev;

	if (!cfg || !cfg->cfg)
		return FW_IF_ERRORS_PARAMS;

	rpmsg_cfg = (FW_IF_RPMSG_CFG *)cfg->cfg;
	rpdev = (struct rpmsg_device *)rpmsg_cfg->rpdev;

	if (!rpdev || !rpdev->ept)
		return FW_IF_ERRORS_DRIVER_NOT_INITIALISED;

	rpmsg_log_hex("[RPMSG] Wr", pucData, ulSize);

	if (rpmsg_send(rpdev->ept, pucData, ulSize) < 0) {
		pr_err("RPMSG Send failed\n");
		return FW_IF_ERRORS_WRITE;
	}

	return FW_IF_ERRORS_NONE;
}

static uint32_t rpmsg_read(void *pvFwIf, uint64_t ullSrcPort, uint8_t *pucData,
			   uint32_t *pulSize, uint32_t ulTimeoutMs)
{
	struct rpmsg_rx_item item;
	unsigned long flags;
	int ret;

	(void)pvFwIf;
	(void)ullSrcPort;

	if (!pucData || !pulSize)
		return FW_IF_ERRORS_PARAMS;

	if (!rpmsg_rx_fifo_init)
		return FW_IF_ERRORS_TIMEOUT;

	ret = wait_event_interruptible_timeout(rpmsg_rx_wq,
					       !kfifo_is_empty(&rpmsg_rx_fifo),
					       msecs_to_jiffies(ulTimeoutMs));
	if (ret == 0)
		return FW_IF_ERRORS_TIMEOUT;

	if (ret < 0)
		return FW_IF_ERRORS_DRIVER_NOT_INITIALISED;

	spin_lock_irqsave(&rpmsg_rx_lock, flags);
	if (kfifo_out(&rpmsg_rx_fifo, &item, 1) != 1) {
		spin_unlock_irqrestore(&rpmsg_rx_lock, flags);
		return FW_IF_ERRORS_TIMEOUT;
	}
	spin_unlock_irqrestore(&rpmsg_rx_lock, flags);

	if (item.len > *pulSize) {
		*pulSize = item.len;
		return FW_IF_ERRORS_READ;
	}

	memcpy(pucData, item.data, item.len);
	*pulSize = item.len;

	rpmsg_log_hex("[RPMSG] Rd", item.data, item.len);

	return FW_IF_ERRORS_NONE;
}

static uint32_t rpmsg_ioctrl(void *pvFwIf, uint32_t ulOption, void *pvValue)
{
	return FW_IF_ERRORS_NONE;
}

static uint32_t rpmsg_bindCallback(void *pvFwIf, FW_IF_callback *pxNewFunc)
{
	if (pvFwIf && pxNewFunc)
		((FW_IF_CFG *)pvFwIf)->raiseEvent = pxNewFunc;

	return FW_IF_ERRORS_NONE;
}

/*****************************************************************************/
/* Public: Create                                                            */
/*****************************************************************************/

uint32_t ulFW_IF_RPMsg_Create(FW_IF_CFG *pxFwIf, FW_IF_RPMSG_CFG *pxRPMsgCfg)
{
	if (!pxFwIf || !pxRPMsgCfg)
		return FW_IF_ERRORS_PARAMS;

	pxRPMsgCfg->rpdev = NULL;
	pxFwIf->cfg = (void *)pxRPMsgCfg;
	pxFwIf->open = rpmsg_open;
	pxFwIf->close = rpmsg_close;
	pxFwIf->write = rpmsg_write;
	pxFwIf->read = rpmsg_read;
	pxFwIf->ioctrl = rpmsg_ioctrl;
	pxFwIf->bindCallback = rpmsg_bindCallback;

	return FW_IF_ERRORS_NONE;
}

#else /* !CONFIG_OF */

void fw_if_rpmsg_rx_push(const uint8_t *data, uint32_t len)
{
}

uint32_t ulFW_IF_RPMsg_Create(FW_IF_CFG *pxFwIf, FW_IF_RPMSG_CFG *pxRPMsgCfg)
{
	return FW_IF_ERRORS_DRIVER_NOT_INITIALISED;
}

#endif /* CONFIG_OF */
