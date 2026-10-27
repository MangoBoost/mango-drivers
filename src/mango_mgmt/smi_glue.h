// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - SMI component of a management device
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_SMI_GLUE_H_
#define _MANGO_MGMT_SMI_GLUE_H_

#include <linux/types.h>

#include "smi/smi_api.h"

struct mango_dev_card_ops;
struct mango_mgmt_dev;
struct pf_dev_struct;

/* The SMI's word on a member's card for mango_dev's reset fence: the card's
 * Status head. */
extern const struct mango_dev_card_ops mango_mgmt_smi_card_ops;

/* Bring the SMI command plane up on the function's GCQ blocks. */
int mango_mgmt_smi_attach(struct mango_mgmt_dev *mgmt);
void mango_mgmt_smi_detach(struct mango_mgmt_dev *mgmt);

/* Stop submissions to a card that may be gone (AER). */
void mango_mgmt_smi_quiesce(struct mango_mgmt_dev *mgmt);

/* Reads answered by the SMI serving @mgmt: its own, else a sibling
 * function's on the same slot. */
int mango_mgmt_smi_dev_board_info(struct mango_mgmt_dev *mgmt,
				  enum smi_board_info type, u8 *buf,
				  size_t len);
int mango_mgmt_smi_dev_sensor(struct mango_mgmt_dev *mgmt,
			      enum smi_sensor_type type,
			      struct smi_sensor_reading *out);
int mango_mgmt_smi_dev_qsfp(struct mango_mgmt_dev *mgmt, u8 link,
			    struct smi_qsfp_status *status, u8 *num_links);
int mango_mgmt_smi_dev_eeprom_read(struct mango_mgmt_dev *mgmt, u8 *buf,
				   u16 len, u8 offset);
int mango_mgmt_smi_dev_eeprom_write(struct mango_mgmt_dev *mgmt, u8 *buf,
				    u16 len, u8 offset);
int mango_mgmt_smi_dev_eeprom_update(struct mango_mgmt_dev *mgmt, u8 offset,
				     u8 mask, u8 bits);

#endif
