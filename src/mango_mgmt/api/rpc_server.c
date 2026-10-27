// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - RPC Server
 *
 * Creates the forward-channel RPC service (SoC side): listens on
 * port 1023 and dispatches incoming calls from Host.
 *
 * Uses svc_create + svc_set_num_threads so the SunRPC framework
 * manages the service thread (and calls svc_process internally).
 *
 * Kernel compat (svc_destroy, svc_recv, svc_create, svc_xprt_create,
 * svc_xprt_destroy_all, svc_set_num_threads) is handled by
 * <mango_compat/sunrpc.h>.
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include <linux/kthread.h>
#include <linux/sunrpc/stats.h>

#include <mango_compat/sunrpc.h>

#include "rpc.h"
#include "internal.h"
#include "../dev.h"

/* ===== SoC-side handler context =====
 *
 * Set during forward-channel server init on SoC.  The RPC procedure
 * handlers in rpc_xdr.c read this to dispatch to registered rpc_ops.
 * Safe because SoC has exactly one mgmt device.
 */
struct mango_mgmt_device_entry *mango_mgmt_rpc_server_ctx;

void mango_mgmt_rpc_set_server_entry(struct mango_mgmt_device_entry *entry)
{
	WRITE_ONCE(mango_mgmt_rpc_server_ctx, entry);
}

/* ===== Service thread function ===== */

static int mango_mgmt_svc_threadfn(void *data)
{
	struct svc_rqst *rqstp = data;

	/* Unblocks svc_start_kthreads() on 6.8+; no-op on older kernels. */
	mango_compat_svc_thread_init_ok(rqstp);

	while (!mango_compat_svc_thread_should_stop(rqstp))
		mango_compat_svc_recv_one(rqstp);

	svc_exit_thread(rqstp);
	return 0;
}

/* ===== Service program descriptor =====
 *
 * pg_authenticate must be non-NULL — Xilinx 6.6 (and likely others)
 * call it unconditionally once svc_authenticate returns SVC_OK,
 * without a NULL check on the callback itself.  We use RPC_AUTH_NULL
 * at the flavor level so there's nothing program-specific to check;
 * just return SVC_OK. */

#ifdef MANGO_COMPAT_PG_AUTH_RETURNS_ENUM
static enum svc_auth_status mango_mgmt_svc_authenticate(struct svc_rqst *rqstp)
{
	return SVC_OK;
}
#else
static int mango_mgmt_svc_authenticate(struct svc_rqst *rqstp)
{
	return SVC_OK;
}
#endif

static const struct svc_version
	*mango_mgmt_svc_versions[MANGO_MGMT_RPC_VERSION + 1];

/* pg_stats: on kernels that keep it in svc_program (older mainline,
 * Xilinx 6.6), svc_create copies this into serv->sv_stats and
 * svc_process_common bumps serv->sv_stats->rpccnt++ without a NULL
 * check.  We don't expose these counters anywhere, but they still need
 * to point at writable memory.
 *
 * Mainline ~6.8 moved stats out of svc_program (passed to svc_create
 * directly, or plumbed via sv_stats some other way); on those kernels
 * we just omit the initializer. */
#ifdef MANGO_COMPAT_SVC_PROGRAM_HAS_PG_STATS
static struct svc_stat mango_mgmt_svc_stats;
#endif

static struct svc_program mango_mgmt_svc_program = {
	.pg_prog = MANGO_MGMT_RPC_PROGRAM,
	.pg_nvers = ARRAY_SIZE(mango_mgmt_svc_versions),
	.pg_vers = mango_mgmt_svc_versions,
	.pg_name = "mango_mgmt",
	.pg_class = "mango",
#ifdef MANGO_COMPAT_SVC_PROGRAM_HAS_PG_STATS
	.pg_stats = &mango_mgmt_svc_stats,
#endif
	.pg_authenticate = mango_mgmt_svc_authenticate,
	.pg_init_request = svc_generic_init_request,
	.pg_rpcbind_set = svc_generic_rpcbind_set,
};

/* ===== Server init/exit ===== */

int mango_mgmt_rpc_server_init(struct mango_mgmt_dev *dev,
			       struct mango_mgmt_rpc *rpc,
			       unsigned short listen_port,
			       const struct svc_version *version)
{
	struct svc_serv *serv;
	int ret;

	/* Install version (single-threaded init, safe) */
	mango_mgmt_svc_versions[MANGO_MGMT_RPC_VERSION] = version;

	serv = MANGO_COMPAT_SVC_CREATE(&mango_mgmt_svc_program,
				       RPCSVC_MAXPAYLOAD,
				       mango_mgmt_svc_threadfn);
	if (!serv) {
		dev_err(dev->dev, "RPC: svc_create failed\n");
		return -ENOMEM;
	}

	/* Create TCP listener */
	ret = svc_xprt_create(serv, "tcp", &init_net, PF_INET, listen_port, 0,
			      NULL);
	if (ret < 0) {
		dev_err(dev->dev, "RPC: TCP listener on port %u failed (%d)\n",
			listen_port, ret);
		mango_compat_svc_serv_free(&serv);
		return ret;
	}

	/* Start one service thread (managed by SunRPC) */
	ret = mango_compat_svc_set_num_threads(serv, 1);
	if (ret < 0) {
		dev_err(dev->dev, "RPC: svc_set_num_threads failed (%d)\n",
			ret);
		mango_compat_svc_xprt_destroy_all(serv, &init_net);
		mango_compat_svc_serv_free(&serv);
		return ret;
	}

	rpc->serv = serv;

	dev_info(dev->dev, "RPC server: listening on port %u\n", listen_port);
	return 0;
}

void mango_mgmt_rpc_server_exit(struct mango_mgmt_rpc *rpc)
{
	if (!rpc->serv)
		return;

	/* Stop service thread */
	mango_compat_svc_set_num_threads(rpc->serv, 0);

	/* Tear down transport + service */
	mango_compat_svc_xprt_destroy_all(rpc->serv, &init_net);
	mango_compat_svc_serv_free(&rpc->serv);
	rpc->serv = NULL;
}
