/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * smi_v2_repo.h - How the second generation answers what a repository holds.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_V2_REPO_H
#define SMI_V2_REPO_H

#include "contract/smi/smi_abi_v2.h" /* SmiTlv_* -- this generation's records */

#include "smi_sensor_model.h"

struct smc_control_ctx;

/**
 * smi_v2_describe() - Ask a repository what it holds, sized as the card says.
 * @smc_ctx: The top level SMC struct instance.
 * @rid: The repository.
 * @chain: Takes the answer's chain, vmalloc()ed; the caller vfree()s it.
 * @len: Takes the bytes the answer wrote.
 *
 * Asked at a first size, and once more at the size a refusal for room names.
 *
 * Return: 0, or an errno; -E2BIG for an answer no slice can carry.
 */
int smi_v2_describe(struct smc_control_ctx *smc_ctx, uint8_t rid,
		    uint8_t **chain, uint32_t *len);

/**
 * smi_v2_repo_read() - Read one repository's description into the model.
 * @smc_ctx: The top level SMC struct instance.
 * @desc: Descriptor for the repository to read.
 * @repo: Slot to populate; owns the payload allocation on success.
 *
 * Return: 0, -ENOMSG for a card that described nothing, else an errno.
 */
int smi_v2_repo_read(struct smc_control_ctx *smc_ctx,
		     const struct smi_repo_desc *desc, struct smi_repo *repo);

/**
 * smi_v2_repo_parse() - Turn one answer's chain into one repository.
 * @smc_ctx: The top level SMC struct instance.
 * @desc: Descriptor for the repository being read.
 * @buf: The chain's first byte.
 * @cap: Bytes the answer said it wrote -- where the chain ends.
 * @repo: Slot to populate; owns @repo->block until it returns 0, so a refusal
 *	  leaves the caller no allocation and no view into one.
 *
 * Separate from the read so a test can hold both ends: bytes in, model out.
 *
 * Return: 0, -ENOMSG for a chain that described nothing, else an errno.
 */
int smi_v2_repo_parse(struct smc_control_ctx *smc_ctx,
		      const struct smi_repo_desc *desc, const uint8_t *buf,
		      size_t cap, struct smi_repo *repo);

#endif /* SMI_V2_REPO_H */
