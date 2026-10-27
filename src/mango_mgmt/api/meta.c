// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - Meta project
 *
 * mgmt's own project, serving introspection ops about the project
 * registry (is a given (prj, devid [, port]) currently attached?).
 * Wired up using the same public API that aux modules use — this
 * file is the minimal working reference for how a project is
 * registered, how a handler is written, and how instances are
 * attached/detached.
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/rcupdate.h>

#include "internal.h"
#include "meta.h"
#include "../mango_mgmt_rpc_macros.h"

/* Handler: bound per-devid via attach_devid(PRJ_META, devid, entry),
 * so `priv` is the device entry.  The dispatcher already holds
 * rcu_read_lock() around this call, so rcu_dereference() is valid. */
MANGO_MGMT_RPC_DEFINE(mango_mgmt_meta_introspect, MANGO_MGMT_SCOPE_DEVID)
{
	struct mango_mgmt_device_entry *entry = priv;
	struct mango_mgmt_project *proj;

	res->status = 0;
	res->ready = 0;

	if (args->target_prj_id >= MANGO_MGMT_PRJ_MAX) {
		res->status = -EINVAL;
		return 0;
	}

	proj = rcu_dereference(mango_mgmt_projects[args->target_prj_id]);
	if (!proj)
		return 0; /* not registered — ready stays 0 */

	switch ((enum mango_mgmt_rpc_scope)args->scope) {
	case MANGO_MGMT_SCOPE_DEVID:
		res->ready =
			rcu_dereference(entry->inst_devid[args->target_prj_id])
				? 1
				: 0;
		break;
	case MANGO_MGMT_SCOPE_PORT:
		if (args->target_port >= MANGO_MGMT_MAX_PORTS) {
			res->status = -EINVAL;
			break;
		}
		res->ready =
			rcu_dereference(entry->inst_port[args->target_prj_id]
							[args->target_port])
				? 1
				: 0;
		break;
	default:
		res->status = -EINVAL;
		break;
	}
	return 0;
}

static const struct mango_mgmt_proc_desc
	mango_mgmt_meta_ops[MANGO_MGMT_META_OP_MAX] = {
		[MANGO_MGMT_META_OP_INTROSPECT] =
			MANGO_MGMT_OP_ENTRY(mango_mgmt_meta_introspect),
	};

/* The project table is registered once at module init — mgmt owns the
 * meta project, so there's no aux-module refcount to manage.  Per-
 * device attach binds the entry as priv for PRJ_META / SCOPE_DEVID. */

int mango_mgmt_meta_module_init(void)
{
	return mango_mgmt_register_project(MANGO_MGMT_PRJ_META,
					   mango_mgmt_meta_ops,
					   MANGO_MGMT_META_OP_MAX);
}

void mango_mgmt_meta_module_exit(void)
{
	mango_mgmt_unregister_project(MANGO_MGMT_PRJ_META);
}

int mango_mgmt_meta_attach_dev(struct mango_mgmt_device_entry *entry)
{
	return mango_mgmt_attach_devid(MANGO_MGMT_PRJ_META, entry->devid,
				       entry);
}

void mango_mgmt_meta_detach_dev(struct mango_mgmt_device_entry *entry)
{
	/* Clear directly on the entry rather than going through the public
	 * mango_mgmt_detach_devid() helper: api_exit calls this AFTER
	 * xa_erase() has removed the entry from mango_mgmt_device_xa, so
	 * the helper's internal xa_load() would return NULL and silently
	 * skip the clear.  We hold the same lock the helper does, so the
	 * RCU publish semantics are identical. */
	mutex_lock(&mango_mgmt_projects_lock);
	rcu_assign_pointer(entry->inst_devid[MANGO_MGMT_PRJ_META], NULL);
	mutex_unlock(&mango_mgmt_projects_lock);
	synchronize_rcu();
}

/* ===== check_soc_prj helpers =====
 *
 * Thin wrappers over the meta introspect op — exported so aux modules
 * can gate Host-mode fallback on whether the SoC peer has the matching
 * project attached at devid or port scope. */

bool mango_mgmt_check_soc_prj_devid(int devid, u32 prj_id)
{
	u32 ready = 0;

	if (mango_mgmt_meta_introspect_rpc(devid, prj_id,
					   MANGO_MGMT_SCOPE_DEVID, 0, &ready))
		return false;
	return ready != 0;
}
EXPORT_SYMBOL_GPL(mango_mgmt_check_soc_prj_devid);

bool mango_mgmt_check_soc_prj_port(int devid, u32 prj_id, u32 port)
{
	u32 ready = 0;

	if (mango_mgmt_meta_introspect_rpc(devid, prj_id, MANGO_MGMT_SCOPE_PORT,
					   port, &ready))
		return false;
	return ready != 0;
}
EXPORT_SYMBOL_GPL(mango_mgmt_check_soc_prj_port);
