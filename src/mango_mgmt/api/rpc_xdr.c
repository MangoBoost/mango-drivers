// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - RPC XDR and Procedure Tables
 *
 * Two SunRPC procs total:
 *   MANGO_MGMT_RPC_NULL  — ping
 *   MANGO_MGMT_RPC_CALL  — generic project call; body is
 *                          (u32 prj_id, u32 proj_op, <project-defined>).
 *
 * Every prj_id (including MANGO_MGMT_PRJ_META, which is mgmt's own
 * project) routes through mango_mgmt_projects[] and the op's typed
 * codec — no special cases in the dispatcher.
 *
 * The dispatcher does the full body decode inside pc_decode, not
 * pc_func.  Reason: on legacy kernels (< 5.5) pc_func has no
 * xdr_stream / __be32 *p — the wire is only reachable from pc_decode
 * and pc_encode.  Doing the body decode early keeps one code path for
 * both kernel generations.  pc_decode pins the project's owner module
 * (try_module_get) before it stashes handler / res_encode pointers
 * into rq_argp, and pc_release module_put's; this makes the stashed
 * pointers safe to call from pc_func / pc_encode without holding
 * rcu_read_lock across the callback boundaries.
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include <linux/slab.h>
#include <linux/module.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/xdr.h>
#include <linux/sunrpc/clnt.h>
#include <linux/rcupdate.h>

#include <mango_compat/sunrpc.h>

#include "rpc.h"
#include "internal.h"
#include "meta.h"
#include "../mango_mgmt_rpc_macros.h"

/* ============================================================
 * Per-request argp / resp buffers (server side).
 *
 * Only the routing header + stashed desc pointers + body pointer live
 * in rq_argp/rq_resp.  The typed project body is kzalloc'd in
 * pc_decode once (prj_id, proj_op) are known (sized by
 * desc->arg_size / desc->res_size), and freed in pc_release.  This
 * keeps project authors free of a hardcoded per-op struct-size cap —
 * the only cap left is the wire-level MANGO_MGMT_RPC_MAX_BODY_WORDS,
 * enforced at compile time by the DECLARE_OP macros.
 *
 * `owner` is module_get'd under rcu_read_lock in pc_decode and
 * module_put'd in pc_release; it pins the aux module so the stashed
 * handler / res_encode pointers remain valid after we drop rcu.
 *
 * resp carries a fallback s32 status for error paths where body
 * couldn't be allocated or an early rejection happened before the
 * handler ran — encode emits just that status word in that case.
 * ============================================================ */

struct mango_mgmt_svc_argp {
	u32 prj_id;
	u32 proj_op;
	void *body; /* kzalloc(desc->arg_size) */
	int (*handler)(void *priv, const void *args, void *res);
	int (*res_encode)(struct xdr_stream *xdr, const void *in);
	enum mango_mgmt_rpc_scope scope;
	struct module *owner; /* pinned via try_module_get in pc_decode */
	__be32 decode_status; /* rpc_* code to return from pc_func when
			       * set to anything other than rpc_success */
};

struct mango_mgmt_svc_resp {
	s32 status; /* fallback, used by encode when body == NULL */
	void *body; /* kzalloc(desc->res_size) */
};

/* ============================================================
 * Client-side codecs
 * ============================================================ */

static void mgmt_xdr_enc_null(struct rpc_rqst *req, struct xdr_stream *xdr,
			      const void *data)
{
}

static int mgmt_xdr_dec_null(struct rpc_rqst *req, struct xdr_stream *xdr,
			     void *data)
{
	return 0;
}

static void mgmt_xdr_enc_rpc_call(struct rpc_rqst *req, struct xdr_stream *xdr,
				  const void *data)
{
	const struct mango_mgmt_rpc_call_argp *a = data;

	if (mango_mgmt_xdr_encode_u32(xdr, a->prj_id))
		return;
	if (mango_mgmt_xdr_encode_u32(xdr, a->proj_op))
		return;
	if (a->desc && a->desc->arg_encode && a->args_typed)
		a->desc->arg_encode(xdr, a->args_typed);
}

static int mgmt_xdr_dec_rpc_call(struct rpc_rqst *req, struct xdr_stream *xdr,
				 void *data)
{
	const struct mango_mgmt_rpc_call_resp *r = data;

	if (!r->desc || !r->desc->res_decode || !r->res_typed)
		return -EIO;
	return r->desc->res_decode(xdr, r->res_typed);
}

/* ============================================================
 * Client-side procedure table
 * ============================================================ */

const struct rpc_procinfo mango_mgmt_fwd_procs[] = {
	[MANGO_MGMT_RPC_NULL] = {
		.p_proc    = MANGO_MGMT_RPC_NULL,
		.p_encode  = mgmt_xdr_enc_null,
		.p_decode  = mgmt_xdr_dec_null,
		.p_arglen  = MGMT_NULL_ARG_SZ,
		.p_replen  = MGMT_NULL_RES_SZ,
		.p_statidx = MANGO_MGMT_RPC_NULL,
		.p_name    = "NULL",
	},
	[MANGO_MGMT_RPC_CALL] = {
		.p_proc    = MANGO_MGMT_RPC_CALL,
		.p_encode  = mgmt_xdr_enc_rpc_call,
		.p_decode  = mgmt_xdr_dec_rpc_call,
		.p_arglen  = 2 + MANGO_MGMT_RPC_MAX_BODY_WORDS,
		.p_replen  = MANGO_MGMT_RPC_MAX_BODY_WORDS,
		.p_statidx = MANGO_MGMT_RPC_CALL,
		.p_name    = "RPC_CALL",
	},
};

const struct rpc_procinfo *mango_mgmt_get_rpc_call_procinfo(void)
{
	return &mango_mgmt_fwd_procs[MANGO_MGMT_RPC_CALL];
}

/* ============================================================
 * Server-side codecs
 *
 * Two SunRPC callback shapes across kernels:
 *
 *   >= 5.5: bool fn(struct svc_rqst *, struct xdr_stream *xdr)
 *   <  5.5: int  fn(struct svc_rqst *, __be32 *p)
 *
 * The body of the decoder / encoder is written once against an
 * xdr_stream.  On legacy the public wrappers synthesize an
 * xdr_stream from __be32 *p via xdr_init_{decode,encode} and use
 * xdr_{arg,res}size_check on exit so SunRPC sees the correct buffer
 * lengths.
 * ============================================================ */

MANGO_COMPAT_SVC_DECODE_FUNC(null)
{
	MANGO_COMPAT_SVC_DECODE_START(0);
	MANGO_COMPAT_SVC_DECODE_RETURN;
}

MANGO_COMPAT_SVC_ENCODE_FUNC(null)
{
	MANGO_COMPAT_SVC_ENCODE_START(0);
	MANGO_COMPAT_SVC_ENCODE_RETURN;
}

/* Full decode + desc lookup + body alloc + module pin, all here.
 * pc_func runs handler-only, pc_encode runs stashed res_encode.
 *
 * Returns false only on wire-level garbage (cannot read routing
 * header).  Logical failures — unknown project, bad op, alloc fail,
 * body decode fail — are reported via arg->decode_status so that
 * SunRPC still drives us through pc_encode + pc_release and the
 * allocations / module ref are cleaned up uniformly. */
static bool mgmt_svc_dec_rpc_call_impl(struct svc_rqst *rqstp,
				       struct xdr_stream *xdr)
{
	struct mango_mgmt_svc_argp *arg = rqstp->rq_argp;
	struct mango_mgmt_svc_resp *res = rqstp->rq_resp;
	struct mango_mgmt_project *proj;
	const struct mango_mgmt_proc_desc *desc;
	size_t arg_size, res_size;

	arg->body = NULL;
	arg->handler = NULL;
	arg->res_encode = NULL;
	arg->owner = NULL;
	arg->decode_status = rpc_success;
	res->body = NULL;
	res->status = 0;

	if (mango_mgmt_xdr_decode_u32(xdr, &arg->prj_id))
		return false;
	if (mango_mgmt_xdr_decode_u32(xdr, &arg->proj_op))
		return false;

	if (arg->prj_id >= MANGO_MGMT_PRJ_MAX) {
		arg->decode_status = rpc_proc_unavail;
		return true;
	}

	rcu_read_lock();
	proj = rcu_dereference(mango_mgmt_projects[arg->prj_id]);
	if (!proj || arg->proj_op >= proj->n_ops) {
		rcu_read_unlock();
		arg->decode_status = rpc_proc_unavail;
		return true;
	}
	desc = &proj->ops[arg->proj_op];
	/* Reserved/unimplemented op ids leave a zeroed table slot. Reject
	 * them instead of dereferencing a NULL arg_decode/handler below. */
	if (!desc->handler || !desc->arg_decode) {
		rcu_read_unlock();
		arg->decode_status = rpc_proc_unavail;
		return true;
	}
	/* Stash under rcu, then pin the owner so it's safe to use
	 * after rcu_read_unlock. */
	if (!try_module_get(proj->owner)) {
		rcu_read_unlock();
		arg->decode_status = rpc_proc_unavail;
		return true;
	}
	arg->owner = proj->owner;
	arg->handler = desc->handler;
	arg->res_encode = desc->res_encode;
	arg->scope = desc->scope;
	arg_size = desc->arg_size;
	res_size = desc->res_size;
	rcu_read_unlock();

	arg->body = kzalloc(arg_size, GFP_KERNEL);
	res->body = kzalloc(res_size, GFP_KERNEL);
	if (!arg->body || !res->body) {
		arg->decode_status = rpc_system_err;
		return true;
	}

	if (desc->arg_decode(xdr, arg->body)) {
		arg->decode_status = rpc_garbage_args;
		return true;
	}

	return true;
}

static bool mgmt_svc_enc_rpc_call_impl(struct svc_rqst *rqstp,
				       struct xdr_stream *xdr)
{
	struct mango_mgmt_svc_argp *arg = rqstp->rq_argp;
	struct mango_mgmt_svc_resp *res = rqstp->rq_resp;

	/* res_encode is only set once pc_decode successfully stashed it
	 * and the body was allocated; that's also the only case where
	 * res->body is non-NULL.  Anything else means we took an error
	 * path — emit just the s32 status. */
	if (arg->res_encode && res->body)
		return arg->res_encode(xdr, res->body) == 0;

	return mango_mgmt_xdr_encode_s32(xdr, res->status) == 0;
}

static bool mgmt_svc_dec_rpc_call(struct svc_rqst *rqstp,
				  struct xdr_stream *xdr)
{
	return mgmt_svc_dec_rpc_call_impl(rqstp, xdr);
}

static bool mgmt_svc_enc_rpc_call(struct svc_rqst *rqstp,
				  struct xdr_stream *xdr)
{
	return mgmt_svc_enc_rpc_call_impl(rqstp, xdr);
}

static __be32 mgmt_svc_proc_null(struct svc_rqst *rqstp)
{
	return rpc_success;
}

static __be32 mgmt_svc_proc_rpc_call(struct svc_rqst *rqstp)
{
	struct mango_mgmt_svc_argp *arg = rqstp->rq_argp;
	struct mango_mgmt_svc_resp *res = rqstp->rq_resp;
	struct mango_mgmt_device_entry *entry;
	void *priv;
	u32 port;
	int status;

	if (arg->decode_status != rpc_success)
		return arg->decode_status;

	entry = READ_ONCE(mango_mgmt_rpc_server_ctx);
	if (!entry)
		return rpc_proc_unavail;

	rcu_read_lock();

	switch (arg->scope) {
	case MANGO_MGMT_SCOPE_DEVID:
		priv = rcu_dereference(entry->inst_devid[arg->prj_id]);
		break;
	case MANGO_MGMT_SCOPE_PORT:
		/* SCOPE_PORT arg structs begin with u32 port (enforced by
		 * MANGO_MGMT_RPC_DECLARE_OP_PORT). */
		port = *(u32 *)arg->body;
		if (port >= MANGO_MGMT_MAX_PORTS) {
			priv = NULL;
			break;
		}
		priv = rcu_dereference(entry->inst_port[arg->prj_id][port]);
		break;
	default:
		priv = NULL;
		break;
	}
	if (!priv) {
		rcu_read_unlock();
		return rpc_proc_unavail;
	}

	status = arg->handler(priv, arg->body, res->body);
	/* Mirror into the fallback so encode can emit a status-only
	 * reply even if it decides not to trust res->body. */
	res->status = status;

	rcu_read_unlock();
	return rpc_success;
}

static void mgmt_svc_release_rpc_call(struct svc_rqst *rqstp)
{
	struct mango_mgmt_svc_argp *arg = rqstp->rq_argp;
	struct mango_mgmt_svc_resp *res = rqstp->rq_resp;

	kfree(arg->body);
	kfree(res->body);
	arg->body = NULL;
	res->body = NULL;

	if (arg->owner) {
		module_put(arg->owner);
		arg->owner = NULL;
	}
	arg->handler = NULL;
	arg->res_encode = NULL;
}

/* Explicit dispatcher.
 *
 * Some kernels (notably Xilinx 6.6) call vs_dispatch unconditionally
 * rather than falling back to svc_generic_dispatch when it's NULL —
 * the NULL call was the source of the `pc : 0x0` oops.  Provide our
 * own to stay compatible both with those kernels and with old ones
 * where svc_generic_dispatch would have been used.
 *
 * Signature varies:
 *   old (≤ 6.6 mainline): int fn(struct svc_rqst *, __be32 *statp)
 *   new (≥ 6.7, Xilinx 6.6 backport):
 *                        int fn(struct svc_rqst *)
 *     — status is written via rqstp->rq_accept_statp instead.
 *
 * Contract (both variants): call pc_func to produce the status, then
 * call pc_encode if the status is rpc_success.  Return 1 on successful
 * dispatch, 0 to signal that the reply should be dropped. */

/* Invoke pc_encode with the right signature for this kernel. */
static inline bool mgmt_svc_call_encode(struct svc_rqst *rqstp)
{
	const struct svc_procedure *procp = rqstp->rq_procinfo;

	if (!procp->pc_encode)
		return true;
	return procp->pc_encode(rqstp, &rqstp->rq_res_stream);
}

#ifdef MANGO_COMPAT_SVC_DISPATCH_TAKES_STATP
/* Old kernels: svc_process_common calls pc_decode before the
 * dispatcher and writes the result status via *statp.  Our dispatcher
 * only needs to run pc_func + pc_encode. */
static int mgmt_svc_fwd_dispatch(struct svc_rqst *rqstp, __be32 *statp)
{
	const struct svc_procedure *procp = rqstp->rq_procinfo;

	*statp = procp->pc_func(rqstp);

	if (*statp == rpc_drop_reply)
		return 0;

	if (*statp == rpc_success && !mgmt_svc_call_encode(rqstp))
		*statp = rpc_system_err;
	return 1;
}
#else
/* Modern kernels (≥ 6.7, Xilinx 6.6 backport): svc_process_common
 * does NOT call pc_decode — the dispatcher must.  Status goes via
 * rqstp->rq_accept_statp (a fallback local is used if the kernel
 * hasn't reserved the XDR slot yet). */
static int mgmt_svc_fwd_dispatch(struct svc_rqst *rqstp)
{
	const struct svc_procedure *procp = rqstp->rq_procinfo;
	__be32 local_statp;
	__be32 *statp =
		rqstp->rq_accept_statp ? rqstp->rq_accept_statp : &local_statp;

	if (procp->pc_decode &&
	    !procp->pc_decode(rqstp, &rqstp->rq_arg_stream)) {
		*statp = rpc_garbage_args;
		return 1;
	}

	*statp = procp->pc_func(rqstp);

	if (*statp == rpc_drop_reply)
		return 0;

	if (*statp == rpc_success && !mgmt_svc_call_encode(rqstp))
		*statp = rpc_system_err;
	return 1;
}
#endif

/* ============================================================
 * Server-side procedure table
 * ============================================================ */

const struct svc_procedure mango_mgmt_fwd_svc_procs[] = {
	[MANGO_MGMT_RPC_NULL] = { .pc_func = mgmt_svc_proc_null,
				  .pc_decode = mgmt_svc_dec_null,
				  .pc_encode = mgmt_svc_enc_null,
				  .pc_argsize = 0,
				  MANGO_COMPAT_SVC_ARGZERO(0).pc_ressize = 0,
				  .pc_xdrressize = 0,
				  MANGO_COMPAT_SVC_NAME("NULL") },
	[MANGO_MGMT_RPC_CALL] = { .pc_func = mgmt_svc_proc_rpc_call,
				  .pc_decode = mgmt_svc_dec_rpc_call,
				  .pc_encode = mgmt_svc_enc_rpc_call,
				  .pc_release = mgmt_svc_release_rpc_call,
				  .pc_argsize =
					  sizeof(struct mango_mgmt_svc_argp),
				  MANGO_COMPAT_SVC_ARGZERO(
					  sizeof(struct mango_mgmt_svc_argp))
					  .pc_ressize =
					  sizeof(struct mango_mgmt_svc_resp),
				  .pc_xdrressize =
					  MANGO_MGMT_RPC_MAX_BODY_WORDS,
				  MANGO_COMPAT_SVC_NAME("RPC_CALL") },
};

static_assert(ARRAY_SIZE(mango_mgmt_fwd_procs) == MANGO_MGMT_RPC_NPROC,
	      "one client entry per proc");
static_assert(ARRAY_SIZE(mango_mgmt_fwd_svc_procs) == MANGO_MGMT_RPC_NPROC,
	      "one server entry per proc");

/* ===== Version struct =====
 *
 * vs_count is a per-proc call counter that svc_generic_init_request
 * does this_cpu_inc() on without a NULL check — must be an initialized
 * per-CPU array of length vs_nproc.
 *
 * Element type varies: older kernels (5.15) declare it as
 * unsigned int *; modern kernels (and Xilinx 6.6) use unsigned long *. */

#ifdef MANGO_COMPAT_SVC_VS_COUNT_IS_LONG
static DEFINE_PER_CPU_ALIGNED(unsigned long,
			      mango_mgmt_fwd_svc_count[MANGO_MGMT_RPC_NPROC]);
#else
static DEFINE_PER_CPU_ALIGNED(unsigned int,
			      mango_mgmt_fwd_svc_count[MANGO_MGMT_RPC_NPROC]);
#endif

const struct svc_version mango_mgmt_fwd_svc_version = {
	.vs_vers = MANGO_MGMT_RPC_VERSION,
	.vs_nproc = MANGO_MGMT_RPC_NPROC,
	.vs_proc = mango_mgmt_fwd_svc_procs,
	.vs_dispatch = mgmt_svc_fwd_dispatch,
	.vs_count = mango_mgmt_fwd_svc_count,
	.vs_xdrsize = MANGO_MGMT_RPC_MAX_BODY_WORDS * 4,
	.vs_hidden = true, /* don't register with rpcbind */
};
