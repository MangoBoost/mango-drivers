// SPDX-License-Identifier: GPL-2.0
/*
 * smi_repo.c - The sensor repository table, expanded from the shared list.
 *
 * Membership, rid, payload kind and whether a card must have it all come from
 * SMI_REPO_LIST, so a repository is registered once, in the contract. Only the
 * display name is this side's, and a repository added without one does not
 * compile.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include <linux/device.h>
#include <linux/kernel.h> /* ARRAY_SIZE */
#include <linux/string.h>

#include "smi_sensor_model.h"

/* Display names, one per row of the shared list. */
#define SMI_REPO_NAME_Temp "temperature"
#define SMI_REPO_NAME_Voltage "voltage"
#define SMI_REPO_NAME_Current "current"
#define SMI_REPO_NAME_Power "power"
#define SMI_REPO_NAME_TotalPower "total-power"
#define SMI_REPO_NAME_BoardInfo "board-info"
#define SMI_REPO_NAME_Card "card"
#define SMI_REPO_NAME_Qsfp "qsfp"
#define SMI_REPO_NAME_Fpt "fpt"

/* List order is discovery order: the optional repositories come last, so row 0
 * -- what the service probe asks for -- is always a mandatory one. */
#define SMI_X(name, rid, kind, req)                                            \
	{ (rid), SmiRepoKind_##kind, SMI_REPO_NAME_##name, !(req) },
static const struct smi_repo_desc smi_repos[] = { SMI_REPO_LIST(SMI_X) };
#undef SMI_X

/**
 * smi_repo_by_rid() - Look a descriptor up by its repository byte.
 * @rid: SmiRepo_*.
 *
 * Return: The descriptor, or NULL if no repository carries this rid.
 */
const struct smi_repo_desc *smi_repo_by_rid(uint8_t rid)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(smi_repos); i++)
		if (smi_repos[i].rid == rid)
			return &smi_repos[i];

	return NULL;
}

/**
 * smi_repo_by_index() - Look a descriptor up by table position.
 * @i: Table index, which is both discovery order and slot order in
 *     pf_dev->sensor_repos.
 *
 * Return: The descriptor, or NULL if @i is out of range.
 */
const struct smi_repo_desc *smi_repo_by_index(size_t i)
{
	BUILD_BUG_ON(ARRAY_SIZE(smi_repos) != SMI_REPO_CNT);

	if (i >= ARRAY_SIZE(smi_repos))
		return NULL;

	return &smi_repos[i];
}

/**
 * smi_repo_name() - Human readable name for a rid.
 * @rid: SmiRepo_*.
 *
 * Return: The name, or "unknown" for a rid no repository claims.
 */
const char *smi_repo_name(uint8_t rid)
{
	const struct smi_repo_desc *desc = smi_repo_by_rid(rid);

	return desc ? desc->name : "unknown";
}

/**
 * smi_repo_free_payload() - Release a repository's payload and its views.
 * @dev: Device the payload was allocated against.
 * @repo: Repository to clear.
 *
 * One block owns the payload, so one free covers every typed view.
 */
void smi_repo_free_payload(struct device *dev, struct smi_repo *repo)
{
	if (!dev || !repo)
		return;

	if (repo->block) {
		devm_kfree(dev, repo->block);
		repo->block = NULL;
	}

	/* The views are pointers into the block, aliasing one union. */
	repo->sensors = NULL;
	repo->num_records = 0;
}
