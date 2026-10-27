/**
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * This header file containing the FW IF RPMsg abstraction definitions.
 *
 * @file fw_if_rpmsg.h
 *
 */

#ifndef _FW_IF_RPMSG_H_
#define _FW_IF_RPMSG_H_

/*****************************************************************************/
/* Includes                                                                  */
/*****************************************************************************/

#include "fw_if.h"

/*****************************************************************************/
/* Structs                                                                   */
/*****************************************************************************/

/**
 * @struct  FW_IF_RPMSG_CFG
 * @brief   config options for RPMsg interfaces
 */
typedef struct _FW_IF_RPMSG_CFG {
	/** Kernel: struct rpmsg_device * stored for rpmsg_send / RX callback */
	void *rpdev;
} FW_IF_RPMSG_CFG;

/*****************************************************************************/
/* Public Functions                                                          */
/*****************************************************************************/

/******************************************************************************
 * @brief   creates an instance of the RPMsg interface
 *
 * @param   pxFwIf      fw_if handle to the interface instance
 * @param   pxRPMsgCfg  unique data of this instance
 *
 * @return  See FW_IF_ERRORS
 ******************************************************************************/
uint32_t ulFW_IF_RPMsg_Create(FW_IF_CFG *pxFwIf, FW_IF_RPMSG_CFG *pxRPMsgCfg);

/**
 * fw_if_rpmsg_rx_push() - Push RX payload into FAL queue
 * Call from rpmsg driver callback so that rpmsg_read() can return data.
 * @data: payload from rpmsg callback
 * @len:  payload length
 */
void fw_if_rpmsg_rx_push(const uint8_t *data, uint32_t len);

#endif
