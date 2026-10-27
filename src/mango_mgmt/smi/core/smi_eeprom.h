// SPDX-License-Identifier: GPL-2.0-only
/*
 * smi_eeprom.h - This file contains functions to read/write from the AVED Manufacturing EEPROM.
 *
 * Copyright (c) 2023-present Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_EEPROM_H
#define SMI_EEPROM_H

#include <linux/types.h>

/* EEPROM `flags` wire codec lives in smi_cmd_flags.h. */

struct pf_dev_struct;

/* EEPROM access for a caller holding a counted device; each checks that the SMC
 * session is up, and each is whole against the others on this host. @offset is
 * from the EEPROM base, @buf_len the byte count. */
int smi_pf_dev_eeprom_read(struct pf_dev_struct *pf_dev, uint8_t *buf,
			   uint16_t buf_len, uint8_t offset);
int smi_pf_dev_eeprom_write(struct pf_dev_struct *pf_dev, uint8_t *buf,
			    uint16_t buf_len, uint8_t offset);

/* The bits of one byte under @mask set to @bits, the others kept: written only
 * where they differ. */
int smi_pf_dev_eeprom_update(struct pf_dev_struct *pf_dev, uint8_t offset,
			     uint8_t mask, uint8_t bits);

#endif /* SMI_EEPROM_H */
