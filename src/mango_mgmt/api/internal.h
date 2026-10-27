/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MangoBoost Management Module - API Layer Internal Declarations
 *
 * Shared structs and helpers used across api.c, rpc_xdr.c, rpc_client.c,
 * and rpc_server.c.  Not exported -- only included within the api/ directory.
 *
 * This header intentionally does NOT include the SunRPC headers so that
 * api.c can co-exist with the libsmi headers (which conflict on dev_open).
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_API_INTERNAL_H_
#define _MANGO_MGMT_API_INTERNAL_H_

#include <linux/types.h>
#include <linux/xarray.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/rcupdate.h>

#include "../mango_mgmt_api.h"
#include "../dev.h"

/* Forward-declare SunRPC types so we can embed pointers without
 * pulling in the full sunrpc headers. */
struct rpc_clnt;
struct svc_serv;
struct svc_version;

/* ===== Global xarray (defined in api.c) ===== */

extern struct xarray mango_mgmt_device_xa;

/* ===== Per-device RPC state ===== */

struct mango_mgmt_rpc {
	struct rpc_clnt *clnt; /* client: connects to remote */
	struct svc_serv *serv; /* server: listens on local port */
};

/* ===== Per-device API state ===== */

struct mango_mgmt_device_entry {
	struct mango_mgmt_dev *mgmt_dev; /* back-pointer */
	int devid; /* xarray key */

	/* RPC channels */
	struct mango_mgmt_rpc fwd_rpc; /* forward: Host->SoC */
	struct task_struct
		*rpc_startup_thread; /* waits for mpipe then starts RPC */

	/* Per-(devid,port) instance pointers, RCU-protected.  Writers
	 * serialize on mango_mgmt_projects_lock; readers take
	 * rcu_read_lock() around dereference + use. */
	void __rcu *inst_devid[MANGO_MGMT_PRJ_MAX];
	void __rcu *inst_port[MANGO_MGMT_PRJ_MAX][MANGO_MGMT_MAX_PORTS];
};

/* ===== Generic RPC_CALL carriers (shared between rpc_xdr.c and
 * rpc_client.c) ===== */

struct mango_mgmt_rpc_call_argp {
	u32 prj_id;
	u32 proj_op;
	const struct mango_mgmt_proc_desc *desc;
	const void *args_typed;
};

struct mango_mgmt_rpc_call_resp {
	const struct mango_mgmt_proc_desc *desc;
	void *res_typed;
};

/* Defined in rpc_xdr.c */
const struct rpc_procinfo *mango_mgmt_get_rpc_call_procinfo(void);

/* Defined in meta.c — mgmt's own project.  Register/unregister run
 * once per module lifetime from api.h; attach/detach run per device
 * from api.c. */
int mango_mgmt_meta_module_init(void);
void mango_mgmt_meta_module_exit(void);
int mango_mgmt_meta_attach_dev(struct mango_mgmt_device_entry *entry);
void mango_mgmt_meta_detach_dev(struct mango_mgmt_device_entry *entry);

/* ===== Project registry (defined in api.c) =====
 *
 * mango_mgmt_projects[prj] points at a kmalloc'd mango_mgmt_project
 * iff the project is currently registered.  Writer-side access is
 * serialized by mango_mgmt_projects_lock and publishes with
 * rcu_assign_pointer; dispatcher reads under rcu_read_lock().
 * The same lock serializes per-device attach/detach against
 * project unregister, so unregister can drain all remaining
 * instances safely. */

struct mango_mgmt_project {
	const struct mango_mgmt_proc_desc *ops;
	unsigned int n_ops;
	struct module *owner; /* pinned per-dispatch via try_module_get() so
			       * stashed op-desc pointers stay valid across
			       * the pc_decode → pc_func → pc_encode →
			       * pc_release callback chain without holding
			       * rcu_read_lock for the duration. */
};

extern struct mango_mgmt_project __rcu *mango_mgmt_projects[MANGO_MGMT_PRJ_MAX];
extern struct mutex mango_mgmt_projects_lock;

/* ===== RPC lifecycle (defined in rpc_client.c / rpc_server.c) ===== */

int mango_mgmt_rpc_client_init(struct mango_mgmt_dev *dev,
			       struct mango_mgmt_rpc *rpc, __be32 remote_ip,
			       unsigned short remote_port);
void mango_mgmt_rpc_client_exit(struct mango_mgmt_rpc *rpc);

int mango_mgmt_rpc_server_init(struct mango_mgmt_dev *dev,
			       struct mango_mgmt_rpc *rpc,
			       unsigned short listen_port,
			       const struct svc_version *version);
void mango_mgmt_rpc_server_exit(struct mango_mgmt_rpc *rpc);

/* Version structs (defined in rpc_xdr.c) */
extern const struct svc_version mango_mgmt_fwd_svc_version;

/* SoC-side handler context (defined in rpc_server.c) */
extern struct mango_mgmt_device_entry *mango_mgmt_rpc_server_ctx;
void mango_mgmt_rpc_set_server_entry(struct mango_mgmt_device_entry *entry);

#endif /* _MANGO_MGMT_API_INTERNAL_H_ */
