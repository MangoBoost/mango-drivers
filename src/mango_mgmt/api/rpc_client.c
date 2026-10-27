// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - RPC Client
 *
 * Creates the forward-channel RPC client: Host connects to SoC:1023.
 *
 * Also provides mango_mgmt_rpc_call() — the Host-side synchronous RPC
 * entry point that every project's typed *_rpc.h wrapper routes through.
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/sunrpc/clnt.h>
#include <linux/rcupdate.h>

#include "rpc.h"
#include "internal.h"
#include "../dev.h"

/* ===== RPC program/version descriptors (client-side) ===== */

static struct rpc_stat mango_mgmt_rpc_clnt_stats;

static const struct rpc_version *mango_mgmt_rpc_versions[] = {
	[MANGO_MGMT_RPC_VERSION] = NULL, /* filled per-instance */
};

static const struct rpc_program mango_mgmt_rpc_program = {
	.name = "mango_mgmt",
	.number = MANGO_MGMT_RPC_PROGRAM,
	.nrvers = ARRAY_SIZE(mango_mgmt_rpc_versions),
	.version = mango_mgmt_rpc_versions,
	.stats = &mango_mgmt_rpc_clnt_stats,
};

/* Forward channel version (Host is client, calls SoC) */
static unsigned int fwd_clnt_counts[MANGO_MGMT_RPC_NPROC];

static const struct rpc_version mango_mgmt_fwd_clnt_version = {
	.number = MANGO_MGMT_RPC_VERSION,
	.nrprocs = MANGO_MGMT_RPC_NPROC,
	.procs = mango_mgmt_fwd_procs,
	.counts = fwd_clnt_counts,
};

/* ===== Client init/exit ===== */

int mango_mgmt_rpc_client_init(struct mango_mgmt_dev *dev,
			       struct mango_mgmt_rpc *rpc, __be32 remote_ip,
			       unsigned short remote_port)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = remote_ip,
		.sin_port = htons(remote_port),
	};
	struct rpc_create_args args = {
		.net = &init_net,
		.protocol = IPPROTO_TCP,
		.address = (struct sockaddr *)&addr,
		.addrsize = sizeof(addr),
		.servername = "mango_mgmt",
		.program = &mango_mgmt_rpc_program,
		.version = MANGO_MGMT_RPC_VERSION,
		.authflavor = RPC_AUTH_NULL,
		.flags = RPC_CLNT_CREATE_NOPING | RPC_CLNT_CREATE_NONPRIVPORT,
	};
	struct rpc_clnt *clnt;

	mango_mgmt_rpc_versions[MANGO_MGMT_RPC_VERSION] =
		&mango_mgmt_fwd_clnt_version;

	clnt = rpc_create(&args);
	if (IS_ERR(clnt)) {
		dev_err(dev->dev, "RPC: rpc_create failed (%ld)\n",
			PTR_ERR(clnt));
		return PTR_ERR(clnt);
	}

	rpc->clnt = clnt;

	dev_info(dev->dev, "RPC client: connected to port %u\n", remote_port);
	return 0;
}

void mango_mgmt_rpc_client_exit(struct mango_mgmt_rpc *rpc)
{
	if (rpc->clnt) {
		rpc_shutdown_client(rpc->clnt);
		rpc->clnt = NULL;
	}
}

/* ===== Host-side synchronous RPC call =====
 *
 * Multiplexes every project op through the single MANGO_MGMT_RPC_CALL
 * sunrpc proc.  The op descriptor is supplied by the caller — the
 * typed wrappers in each project's *_rpc.h hard-code it from compile-
 * time-known codecs — so no project lookup happens here.  This lets a
 * host-only module call into a SoC-only project's _rpc() wrapper
 * without that project being registered on the host.  The SoC dispatcher still does its own
 * project lookup using the prj_id / proj_op put on the wire.
 *
 * Returns: 0 on success; transport errors (negative) from
 *          rpc_call_sync; otherwise the handler-returned status copied
 *          from the first field of the typed reply.  -ENODEV if devid
 *          is unknown, -ENOLINK if the RPC client isn't up yet,
 *          -EINVAL on bad args. */
int mango_mgmt_rpc_call(int devid, u32 prj_id, u32 proj_op,
			const struct mango_mgmt_proc_desc *desc,
			const void *args, void *res)
{
	struct mango_mgmt_device_entry *entry;
	struct mango_mgmt_rpc_call_argp argp;
	struct mango_mgmt_rpc_call_resp resp;
	struct rpc_clnt *clnt;
	struct rpc_message msg = {};
	int ret;

	if (!desc || !res)
		return -EINVAL;
	if (prj_id >= MANGO_MGMT_PRJ_MAX)
		return -EINVAL;

	entry = xa_load(&mango_mgmt_device_xa, devid);
	if (!entry)
		return -ENODEV;
	clnt = entry->fwd_rpc.clnt;
	if (!clnt)
		return -ENOLINK;

	argp.prj_id = prj_id;
	argp.proj_op = proj_op;
	argp.desc = desc;
	argp.args_typed = args;

	resp.desc = desc;
	resp.res_typed = res;

	msg.rpc_proc = mango_mgmt_get_rpc_call_procinfo();
	msg.rpc_argp = &argp;
	msg.rpc_resp = &resp;

	ret = rpc_call_sync(clnt, &msg, 0);
	if (ret)
		return ret;

	/* Every typed reply struct begins with s32 status. */
	return ((struct mango_mgmt_rpc_res *)res)->status;
}
EXPORT_SYMBOL_GPL(mango_mgmt_rpc_call);
