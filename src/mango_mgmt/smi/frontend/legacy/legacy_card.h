/* SPDX-License-Identifier: GPL-2.0 */
/*
 * legacy_card.h - What the legacy face keeps for one card.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_LEGACY_CARD_H
#define SMI_LEGACY_CARD_H

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/types.h>

#include "legacy_pub.h" /* struct smi_legacy_counters */
#include "legacy_qsfp.h" /* struct smi_qsfp_store */
#include "smi_fpt.h" /* struct smi_fpt_copy */
#include "smi_top.h"

struct smi_qsfp_attrs;

/**
 * struct smi_legacy_card - What the legacy face keeps for one card.
 * @apps: The applications registered on this card, told when it goes.
 * @app_lock: Guards @apps.
 * @qsfp: The published cage plane: where each cage's section is. Empty on a
 *	  card that seats none.
 * @qsfp_attrs: The files that plane presents, one pair per cage; NULL where
 *		none were built.
 * @qsfp_attrs_cnt: How many it built; its own count, as the plane may be gone
 *		    by the time they are removed.
 * @counters: The card's names for its counter slots, read once.
 * @fpt: The partition tables, read at present and after this face rewrote them.
 * @fpt_lock: Holds the tables still across one op that reads or rewrites them:
 *	      a copy and the check before it, a rewrite and the read after it.
 */
struct smi_legacy_card {
	struct list_head apps;
	struct mutex app_lock;
	struct smi_qsfp_store qsfp;
	struct smi_qsfp_attrs *qsfp_attrs;
	uint8_t qsfp_attrs_cnt;
	struct smi_legacy_counters counters;
	struct smi_fpt_copy fpt;
	struct mutex fpt_lock;
};

/* This card's legacy state; the face made it before any reader could come. */
static inline struct smi_legacy_card *
smi_legacy_card(const struct pf_dev_struct *pf_dev)
{
	return pf_dev->frontend.face_priv;
}

/* APP_SETUP: register or drop @task on this card. A repeat is a no-op. */
int smi_legacy_app_add(struct pf_dev_struct *pf_dev, struct task_struct *task);
int smi_legacy_app_del(struct pf_dev_struct *pf_dev, struct task_struct *task);

#endif /* SMI_LEGACY_CARD_H */
