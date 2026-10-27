// SPDX-License-Identifier: GPL-2.0
/*
 * smi_v2_cmd.h - the second wire generation, as rows this driver walks
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved. */

/* Everything v2. The engine walks whichever table the registry settled on and
 * never learns the generation; v2's rows are thin because one answer shape
 * serves every verb. */

#ifndef SMI_V2_CMD_H
#define SMI_V2_CMD_H

#include <linux/types.h>

#include "contract/smi/smi_abi.h"
#include "contract/smi/smi_abi_v2.h"

#include "smc_proxy.h"
#include "smi_cmd_row.h"

/** smi_v2_wire_ops - this generation, as the engine's registry names it. */
extern const struct smi_wire_ops smi_v2_wire_ops;

/* What a Negotiate answered: the nonce decides whose answer it is. */
struct smi_v2_nego_ans {
	uint16_t status;
	uint32_t val;
	uint32_t detail;
};

/* One attach's walk: foreign answers in a row, the last one's refusal, and the
 * time Quiescing took. */
struct smi_v2_nego_walk {
	uint32_t bound_ms; /* the longest an earlier request may outlive its host */
	uint32_t waited_ms;
	uint32_t idle_ms; /* waited while the card said 0 ms were left */
	unsigned int foreign;
	uint16_t refused; /* its status, 0 for an answer that refused nothing */
};

/**
 * smi_v2_nego_step() - What one Negotiate answer means for the attach.
 * @ans: the answer.
 * @nonce: the nonce the ask carried.
 * @walk: the attach's state, zeroed but for @bound_ms before the first ask.
 *
 * Return: 0 settled, ms to wait before asking again, or the errno ending the
 *         attach: a refusal's, -EPROTO no nonce, -EBUSY an earlier one stuck.
 */
int smi_v2_nego_step(const struct smi_v2_nego_ans *ans, uint32_t nonce,
		     struct smi_v2_nego_walk *walk);

/* A v2 status saying the card took nothing: the request may go again. */
bool smi_v2_nothing_taken(uint16_t status);

#endif /* SMI_V2_CMD_H */
