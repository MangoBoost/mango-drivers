// SPDX-License-Identifier: GPL-2.0
/*
 * smi_cmd_prep.c - the row callbacks that do not depend on the generation
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved. */

/* A row's prep sizes the slice, stages the caller's bytes into it and resolves
 * the sub-ids packed into flags. None of that touches a wire frame, so none of
 * it belongs to a generation -- which is what lets a generation's file go
 * whole. */

#include <linux/errno.h>

#include "contract/smi/smi_abi.h"

#include "transport/smi_transport.h"

#include "smi_cmd_flags.h"
#include "smi_cmd_row.h"
#include "smi_log.h"
#include "smi_sensor_model.h" /* smi_repo_by_rid(): is that a repository? */

int smi_cmd_prep_sensor(struct smi_cmd_ctx *ctx)
{
	if (ctx->region_len < ctx->data_size) {
		SMI_DEV_WARN(
			ctx->dev,
			"Sensor request length is %d but allocated length is %d",
			ctx->data_size, ctx->region_len);
		ctx->payload_size = ctx->region_len;
	} else {
		ctx->payload_size = ctx->data_size;
	}

	/* The rid IS the currency for a repository: flags carry it, and what a
	 * generation asks by is that generation's row to work out. */
	if (!smi_repo_by_rid((uint8_t)ctx->flags)) {
		SMI_DEV_ERR(ctx->dev,
			    "No sensor repo for command 0x%X flags 0x%X",
			    ctx->cmd_id, ctx->flags);
		return -EIO;
	}
	ctx->rid = (uint8_t)ctx->flags;
	return 0;
}

/* Size the slice to the request, clamped to the region, and stage the caller's
 * bytes in it when they travel to the card. */
int smi_cmd_prep_window(struct smi_cmd_ctx *ctx, bool out)
{
	SMI_DEV_VDBG(ctx->dev, "Payload size = %d, page length = %d",
		     ctx->data_size, ctx->region_len);
	if (ctx->region_len < ctx->data_size) {
		SMI_DEV_WARN(
			ctx->dev,
			"Data request length is %d but allocated length is %d",
			ctx->data_size, ctx->region_len);
		ctx->payload_size = ctx->region_len;
	} else {
		ctx->payload_size = ctx->data_size;
	}

	if (out && smi_transport_write(ctx->tp, ctx->payload_address,
				       ctx->data_buf, ctx->payload_size)) {
		SMI_DEV_ERR(ctx->dev, "Failed to write payload data");
		return -EIO;
	}
	return 0;
}

/* EEPROM read/write: the flags say which way the bytes travel. */
int smi_cmd_prep_eeprom(struct smi_cmd_ctx *ctx)
{
	return smi_cmd_prep_window(ctx,
				   EEPROM_GET_TYPE(ctx->flags) == SmiRw_Write);
}
