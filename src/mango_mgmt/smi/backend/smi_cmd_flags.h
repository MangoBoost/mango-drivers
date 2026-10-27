// SPDX-License-Identifier: GPL-2.0
/*
 * smi_cmd_flags.h - Wire-format codec for the `flags` word of the commands
 * every generation serves.
 *
 * Producers pack via the SET_ macros and the rows unpack via the GET_ ones, so
 * both sides of each wire format stay in lockstep here.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef _SMI_CMD_FLAGS_H_
#define _SMI_CMD_FLAGS_H_

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/types.h>

/* EEPROM read/write: BIT(0) = type, GENMASK(15, 8) = offset. */
#define EEPROM_TYPE_FIELD BIT(0)
#define EEPROM_OFFSET_FIELD GENMASK(15, 8)
#define EEPROM_GET_OFFSET(data) FIELD_GET(EEPROM_OFFSET_FIELD, data)
#define EEPROM_GET_TYPE(data) FIELD_GET(EEPROM_TYPE_FIELD, data)
#define EEPROM_SET_OFFSET(data) FIELD_PREP(EEPROM_OFFSET_FIELD, data)
#define EEPROM_SET_TYPE(data) FIELD_PREP(EEPROM_TYPE_FIELD, data)

#endif /* _SMI_CMD_FLAGS_H_ */
