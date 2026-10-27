// SPDX-License-Identifier: GPL-2.0
/*
 * smi_frontend.c - The UAPI axis: which surface a card presents.
 *
 * A registry of surfaces, never of pairs: adding one is a file and a row, and
 * no wire generation is named here. The binding a surface makes is recorded in
 * one place so a reader cannot see half of it.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved. */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>

#include "legacy/legacy.h" /* the legacy row and its driver attributes */
#include "smi_node.h"
#include "smi_frontend.h"
#include "smi_log.h"
#include "smi_name.h"
#include "smi_top.h"

/* The surfaces this build can present, newest first. */
static const struct smi_frontend_ops *const smi_frontends[] = {
	&smi_legacy_frontend_ops,
};

/* The module's own switch, then one line per surface that has any. A list the
 * bus drivers name at registration, so it is here rather than in a row. */
const struct attribute_group *smi_driver_groups[] = {
	&smi_driver_group,
	&smi_legacy_driver_group,
	NULL,
};

int smi_frontend_module_init(void)
{
	size_t i;
	int ret;

	ret = smi_node_class_create();
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(smi_frontends); i++) {
		if (!smi_frontends[i]->mod_init)
			continue;
		ret = smi_frontends[i]->mod_init();
		if (ret)
			goto undo;
	}

	return 0;

undo:
	while (i--) {
		if (smi_frontends[i]->mod_exit)
			smi_frontends[i]->mod_exit();
	}
	smi_node_class_destroy();
	return ret;
}

void smi_frontend_module_exit(void)
{
	size_t i = ARRAY_SIZE(smi_frontends);

	while (i--) {
		if (smi_frontends[i]->mod_exit)
			smi_frontends[i]->mod_exit();
	}
	smi_node_class_destroy();
}

/* The bound row's face, else the one asked for, else the oldest row's. */
static const struct smi_face_ops *
smi_frontend_face_of(const struct pf_dev_struct *pf_dev, enum smi_uapi_id want)
{
	const struct smi_frontend_ops *f =
		smp_load_acquire(&pf_dev->frontend.ops);

	if (!f)
		f = smi_frontend_of(want);
	if (!f)
		f = smi_frontends[ARRAY_SIZE(smi_frontends) - 1];

	return f->face;
}

int smi_frontend_present(struct pf_dev_struct *pf_dev, enum smi_uapi_id want)
{
	const struct smi_face_ops *face;
	int found, ret;

	if (!pf_dev)
		return -EINVAL;

	face = smi_frontend_face_of(pf_dev, want);
	found = face->present(pf_dev);
	if (found < 0)
		return found;

	ret = smi_node_create(&pf_dev->cdev, pf_dev->dev, face->fops,
			      face->node_mode);
	if (ret) {
		face->unpresent(pf_dev);
		return ret;
	}

	pf_dev->frontend.face = face;
	return found;
}

void smi_frontend_leave(struct pf_dev_struct *pf_dev)
{
	if (pf_dev && pf_dev->frontend.face)
		pf_dev->frontend.face->leave(pf_dev);
}

void smi_frontend_unpresent(struct pf_dev_struct *pf_dev)
{
	if (!pf_dev || !pf_dev->frontend.face)
		return;

	smi_node_destroy(&pf_dev->cdev);
	pf_dev->frontend.face->unpresent(pf_dev);
	pf_dev->frontend.face = NULL;
}

const struct smi_frontend_ops *
smi_frontend_paired(const struct pf_dev_struct *pf_dev)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(smi_frontends); i++)
		if (smi_frontends[i]->pairs(pf_dev))
			return smi_frontends[i];

	return NULL;
}

enum smi_uapi_id smi_uapi_wanted(const struct pf_dev_struct *pf_dev)
{
	const struct smi_frontend_ops *f;

	f = smi_frontend_paired(pf_dev);
	return f ? f->id : SMI_UAPI_AUTO;
}

/* AUTO reaches no row: it is a question the caller answers before asking. */
const struct smi_frontend_ops *smi_frontend_of(enum smi_uapi_id id)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(smi_frontends); i++)
		if (smi_frontends[i]->id == id)
			return smi_frontends[i];

	return NULL;
}

int smi_frontend_bind(struct pf_dev_struct *pf_dev, enum smi_uapi_id id)
{
	const struct smi_frontend_ops *f;
	int ret;

	if (!pf_dev || !pf_dev->dev)
		return -EINVAL;

	f = smi_frontend_of(id);
	if (!f) {
		SMI_DEV_ERR(pf_dev->dev, "no uapi surface %u in this build",
			    id);
		return -EINVAL;
	}

	ret = f->hwmon_register(pf_dev->dev, pf_dev);
	if (ret)
		return ret;

	SMI_DEV_INFO(pf_dev->dev, "uapi surface %s presented", f->name);
	return 0;
}

int smi_frontend_bound(struct pf_dev_struct *pf_dev,
		       const struct smi_frontend_ops *ops, struct device *hwmon,
		       void *priv)
{
	if (!pf_dev || !ops || !hwmon)
		return -EINVAL;

	pf_dev->frontend.hwmon = hwmon;
	pf_dev->frontend.priv = priv;
	/* Release: the marker a reader tests is written last, so acquiring it
	 * is what makes the two fields above visible with it. */
	smp_store_release(&pf_dev->frontend.ops, ops);
	return 0;
}

struct device *smi_frontend_hwmon(const struct pf_dev_struct *pf_dev)
{
	if (!pf_dev || !smp_load_acquire(&pf_dev->frontend.ops))
		return NULL;

	return pf_dev->frontend.hwmon;
}

void *smi_frontend_priv(const struct pf_dev_struct *pf_dev)
{
	/* Some readers arrive without a counted lookup, so the acquire is
	 * here rather than inherited from a reference. */
	if (!pf_dev || !smp_load_acquire(&pf_dev->frontend.ops))
		return NULL;

	return pf_dev->frontend.priv;
}

int smi_frontend_chan_resolve(struct pf_dev_struct *pf_dev,
			      enum hwmon_sensor_types type, int channel,
			      uint8_t *rid, uint8_t *id)
{
	const struct smi_frontend_ops *f =
		pf_dev ? smp_load_acquire(&pf_dev->frontend.ops) : NULL;

	if (!f)
		return -EOPNOTSUPP;

	return f->chan_resolve(pf_dev, type, channel, rid, id);
}

const char *smi_frontend_name(const struct pf_dev_struct *pf_dev)
{
	return (pf_dev && pf_dev->frontend.ops) ? pf_dev->frontend.ops->name
						: NULL;
}
