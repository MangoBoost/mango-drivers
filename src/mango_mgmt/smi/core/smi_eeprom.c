// SPDX-License-Identifier: GPL-2.0-only
/*
 * smi_eeprom.c - Read and write the AVED manufacturing EEPROM.
 *
 * Copyright (c) 2023-present Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include <linux/err.h>
#include <linux/types.h>

#include "backend/smi_cmd_flags.h"
#include "backend/smi_smc_control.h"

#include "smi_eeprom.h"
#include "smi_top.h"

/* Does the request fit the part this card has? @extent is what the session
 * settled, so a request past the end is refused before the round trip. */
static bool eeprom_range_ok(struct smc_control_ctx *smc_ctx, uint32_t extent,
			    uint16_t len, uint8_t offset)
{
	if ((uint32_t)offset + len <= extent)
		return true;

	/* Debug level, not error: a caller asking past the part made a bad
	 * request, and the errno is the answer. */
	SMI_VDBG(smc_ctx,
		 "EEPROM request past the part: offset:%d len:%d extent:%u",
		 offset, len, extent);
	return false;
}

/*
 * Read one or more values from the EEPROM. */
static int eeprom_read(struct smc_control_ctx *smc_ctx, uint32_t extent,
		       uint8_t *buf, uint16_t buf_len, uint8_t offset)
{
	int ret = SUCCESS;
	uint32_t eeprom_req_data = 0;

	if (!smc_ctx || !buf || (buf_len == 0))
		return -EINVAL;

	if (!eeprom_range_ok(smc_ctx, extent, buf_len, offset))
		return -EINVAL;

	SMI_VDBG(smc_ctx, "Attempting to read EEPROM at offset:%d len:%d",
		 offset, buf_len);

	eeprom_req_data = EEPROM_SET_TYPE(SmiRw_Read);
	eeprom_req_data |= EEPROM_SET_OFFSET(offset);
	ret = smi_submit_command(smc_ctx, SMC_CMD_ID_EEPROM_READ_WRITE,
				 eeprom_req_data, buf, buf_len);

	return ret;
}

/*
 * Write one or more values to the EEPROM, fenced against a reset: on an SP
 * board each page erases a boot flash sector, and a reset midway loses it. */
static int eeprom_write(struct pf_dev_struct *pf_dev, uint32_t extent,
			uint8_t *buf, uint16_t buf_len, uint8_t offset)
{
	struct smc_control_ctx *smc_ctx = pf_dev->smc_ctx;
	int ret = SUCCESS;
	uint32_t eeprom_req_data = 0;
	void *fence;

	if (!smc_ctx || !buf || (buf_len == 0))
		return -EINVAL;

	if (!eeprom_range_ok(smc_ctx, extent, buf_len, offset))
		return -EINVAL;

	SMI_VDBG(smc_ctx, "Attempting to write EEPROM at offset:%d len:%d",
		 offset, buf_len);

	fence = smi_reset_fence(pf_dev);
	if (IS_ERR(fence)) {
		SMI_WARN(
			smc_ctx,
			"EEPROM write refused (%ld): a reset of this unit is in flight",
			PTR_ERR(fence));
		return PTR_ERR(fence);
	}
	eeprom_req_data = EEPROM_SET_TYPE(SmiRw_Write);
	eeprom_req_data |= EEPROM_SET_OFFSET(offset);
	ret = smi_submit_command(smc_ctx, SMC_CMD_ID_EEPROM_READ_WRITE,
				 eeprom_req_data, buf, buf_len);
	smi_reset_unfence(pf_dev, fence);

	/* Only a write drained after submit can leave an unknown result. */
	if (smi_cmd_aborted_after_submit(ret))
		SMI_WARN(
			smc_ctx,
			"EEPROM write aborted (%d); contents may not reflect the request",
			ret);

	return ret;
}

int smi_pf_dev_eeprom_read(struct pf_dev_struct *pf_dev, uint8_t *buf,
			   uint16_t buf_len, uint8_t offset)
{
	int ret;

	if (!pf_dev || !buf)
		return -EINVAL;
	if (!smi_pf_dev_ready(pf_dev) || !pf_dev->smc_ctx)
		return -ENODEV;

	if (mutex_lock_interruptible(&pf_dev->eeprom_lock))
		return -EINTR;
	ret = eeprom_read(pf_dev->smc_ctx, pf_dev->facts.eeprom_bytes, buf,
			  buf_len, offset);
	mutex_unlock(&pf_dev->eeprom_lock);
	return ret;
}

int smi_pf_dev_eeprom_write(struct pf_dev_struct *pf_dev, uint8_t *buf,
			    uint16_t buf_len, uint8_t offset)
{
	int ret;

	if (!pf_dev || !buf)
		return -EINVAL;
	if (!smi_pf_dev_ready(pf_dev) || !pf_dev->smc_ctx)
		return -ENODEV;

	if (mutex_lock_interruptible(&pf_dev->eeprom_lock))
		return -EINTR;
	ret = eeprom_write(pf_dev, pf_dev->facts.eeprom_bytes, buf, buf_len,
			   offset);
	mutex_unlock(&pf_dev->eeprom_lock);
	return ret;
}

int smi_pf_dev_eeprom_update(struct pf_dev_struct *pf_dev, uint8_t offset,
			     uint8_t mask, uint8_t bits)
{
	const uint32_t extent = pf_dev ? pf_dev->facts.eeprom_bytes : 0;
	uint8_t val = 0;
	int ret;

	if (!pf_dev)
		return -EINVAL;
	if (!smi_pf_dev_ready(pf_dev) || !pf_dev->smc_ctx)
		return -ENODEV;

	if (mutex_lock_interruptible(&pf_dev->eeprom_lock))
		return -EINTR;
	ret = eeprom_read(pf_dev->smc_ctx, extent, &val, 1, offset);
	/* A byte that already says it is left alone. */
	if (!ret && ((val & mask) != (bits & mask))) {
		val = (val & ~mask) | (bits & mask);
		ret = eeprom_write(pf_dev, extent, &val, 1, offset);
	}
	mutex_unlock(&pf_dev->eeprom_lock);
	return ret;
}
