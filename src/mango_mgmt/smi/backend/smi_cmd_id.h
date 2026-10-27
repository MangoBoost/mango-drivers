// SPDX-License-Identifier: GPL-2.0
/*
 * smi_cmd_id.h - SMC command id enum, shared across libsmi layers.
 *
 * Separate from smi_smc_control.h so the command engine and the sensor helpers
 * can name commands without the S3 management context; the S3 facade and the
 * S4 features share the same ids through here.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved. */

#ifndef _SMI_CMD_ID_H_
#define _SMI_CMD_ID_H_

#include <linux/errno.h> /* SMC_CMD_ID_UNKNOWN = -EINVAL */

/**
 * enum smc_cmd_id - the commands the neutral layers ask for by name
 * @SMC_CMD_ID_UNKNOWN: default unknown command
 * @SMC_CMD_ID_GET_SDR: read one sensor repository
 * @SMC_CMD_ID_HEARTBEAT: heartbeat command
 * @SMC_CMD_ID_EEPROM_READ_WRITE: eeprom read/write command
 * @SMC_CMD_ID_NEGOTIATE: ask a card what generation and features it speaks
 * @SMC_CMD_ID_LOCATE: blink the card's locate beacon for some seconds
 * @SMC_CMD_ID_RAW: a verb the request names, sent as the card states it
 * @SMC_CMD_ID_WIRE: the first id a generation names for itself, in its file
 *
 * An id the settled generation has no row for is a verb that card lacks.
 */
enum smc_cmd_id {
	SMC_CMD_ID_UNKNOWN = -EINVAL,
	SMC_CMD_ID_GET_SDR = 0x0,
	SMC_CMD_ID_HEARTBEAT,
	SMC_CMD_ID_EEPROM_READ_WRITE,
	SMC_CMD_ID_NEGOTIATE,
	SMC_CMD_ID_LOCATE,
	SMC_CMD_ID_RAW,

	SMC_CMD_ID_WIRE
};

#endif /* _SMI_CMD_ID_H_ */
