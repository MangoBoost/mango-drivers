/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MangoBoost Management Module - RPC Schema Macros
 *
 * Generates typed arg/result structs + XDR codec functions from
 * per-op X-macro schemas.  Used by projects (aux modules) to declare
 * their RPC ops without writing XDR encoders/decoders by hand.
 *
 * Example — a per-port op with two arg fields and a status-only reply:
 *
 *   #define FOO_OP_WINDOW_SET_ARG(X)                                      \
 *       X(u32, qp_num)                                                    \
 *       X(u32, window_val)
 *   #define FOO_OP_WINDOW_SET_RES(X)    / empty -- status-only /
 *
 *   MANGO_MGMT_RPC_DECLARE_OP_PORT(foo_window_set,
 *                                  FOO_OP_WINDOW_SET_ARG,
 *                                  FOO_OP_WINDOW_SET_RES);
 *
 *   MANGO_MGMT_RPC_DEFINE(foo_window_set, MANGO_MGMT_SCOPE_PORT)
 *   {
 *       struct foo_port *pp = priv;
 *       return do_set_window(pp, args->qp_num, args->window_val);
 *   }
 *
 *   static const struct mango_mgmt_proc_desc foo_ops[] = {
 *       MANGO_MGMT_OP_ENTRY(foo_window_set),
 *       / ... /
 *   };
 *
 * Expansion of DECLARE_OP_PORT(NAME, ARG_X, RES_X) gives:
 *   struct NAME_arg { u32 port; <ARG_X fields>; }
 *   struct NAME_res { s32 status; <RES_X fields>; }
 *   static inline int NAME_arg_{en,de}code(struct xdr_stream *, ...)
 *   static inline int NAME_res_{en,de}code(struct xdr_stream *, ...)
 *
 * DEFINE(NAME, SCOPE) emits:
 *   static const enum mango_mgmt_rpc_scope NAME_scope = SCOPE;
 *   static int NAME_handler(void *, const void *, void *);    / trampoline /
 *   static int NAME_impl(void *priv,
 *                        const struct NAME_arg *args,
 *                        struct NAME_res *res)                / body follows /
 *
 * OP_ENTRY(NAME) fills one row of struct mango_mgmt_proc_desc, pulling
 * the handler, codecs, scope, and sizes by symbol name.
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_RPC_MACROS_H_
#define _MANGO_MGMT_RPC_MACROS_H_

#include <linux/types.h>
#include <linux/build_bug.h>
#include <linux/sunrpc/xdr.h>

#include "mango_mgmt_api.h"

/* ============================================================
 * Primitive XDR codecs — one wire word per u32/s32, two for u64/s64.
 * Wrap the xdr_reserve_space / xdr_inline_decode idiom used directly
 * in api/rpc_xdr.c so projects don't touch __be32 pointers.
 * Returns 0 on success, -EIO on buffer exhaustion.
 * ============================================================ */

static inline int mango_mgmt_xdr_encode_u32(struct xdr_stream *xdr, u32 v)
{
	__be32 *p = xdr_reserve_space(xdr, 4);

	if (!p)
		return -EIO;
	*p = cpu_to_be32(v);
	return 0;
}

static inline int mango_mgmt_xdr_decode_u32(struct xdr_stream *xdr, u32 *out)
{
	__be32 *p = xdr_inline_decode(xdr, 4);

	if (!p)
		return -EIO;
	*out = be32_to_cpup(p);
	return 0;
}

static inline int mango_mgmt_xdr_encode_s32(struct xdr_stream *xdr, s32 v)
{
	return mango_mgmt_xdr_encode_u32(xdr, (u32)v);
}

static inline int mango_mgmt_xdr_decode_s32(struct xdr_stream *xdr, s32 *out)
{
	u32 v;
	int ret = mango_mgmt_xdr_decode_u32(xdr, &v);

	if (ret)
		return ret;
	*out = (s32)v;
	return 0;
}

static inline int mango_mgmt_xdr_encode_u64(struct xdr_stream *xdr, u64 v)
{
	__be32 *p = xdr_reserve_space(xdr, 8);

	if (!p)
		return -EIO;
	*p++ = cpu_to_be32((u32)(v >> 32));
	*p = cpu_to_be32((u32)v);
	return 0;
}

static inline int mango_mgmt_xdr_decode_u64(struct xdr_stream *xdr, u64 *out)
{
	__be32 *p = xdr_inline_decode(xdr, 8);

	if (!p)
		return -EIO;
	*out = ((u64)be32_to_cpup(p) << 32) | be32_to_cpup(p + 1);
	return 0;
}

static inline int mango_mgmt_xdr_encode_s64(struct xdr_stream *xdr, s64 v)
{
	return mango_mgmt_xdr_encode_u64(xdr, (u64)v);
}

static inline int mango_mgmt_xdr_decode_s64(struct xdr_stream *xdr, s64 *out)
{
	u64 v;
	int ret = mango_mgmt_xdr_decode_u64(xdr, &v);

	if (ret)
		return ret;
	*out = (s64)v;
	return 0;
}

/* ============================================================
 * X-macro rows — internal helpers expanded by DECLARE_OP.
 * Each takes (TYPE, NAME) and emits one piece of code.
 * ============================================================ */

#define __MANGO_MGMT_ROW_DECL(T, N) T N;

#define __MANGO_MGMT_ROW_ENC(T, N)                                             \
	do {                                                                   \
		int __ret = mango_mgmt_xdr_encode_##T(xdr, in->N);             \
		if (__ret)                                                     \
			return __ret;                                          \
	} while (0);

#define __MANGO_MGMT_ROW_DEC(T, N)                                             \
	do {                                                                   \
		int __ret = mango_mgmt_xdr_decode_##T(xdr, &out->N);           \
		if (__ret)                                                     \
			return __ret;                                          \
	} while (0);

/* Leading-comma rows: append an extra parameter / init entry onto
 * an existing list without tripping over empty ARG_X / RES_X. */
#define __MANGO_MGMT_ROW_PARAM(T, N) , T N
#define __MANGO_MGMT_ROW_OUT_PARAM(T, N) , T *N##_out
#define __MANGO_MGMT_ROW_INIT(T, N) .N = N,
#define __MANGO_MGMT_ROW_EXTRACT(T, N)                                         \
	if (N##_out)                                                           \
		*N##_out = __r.N;

/* ============================================================
 * DECLARE_OP — struct + codecs, no auto port field.
 * Use for introspection / device-scope ops where port is not implied.
 * ============================================================ */

#define MANGO_MGMT_RPC_DECLARE_OP(NAME, ARG_X, RES_X)                          \
	struct NAME##_arg {                                                    \
		ARG_X(__MANGO_MGMT_ROW_DECL)                                   \
	};                                                                     \
	struct NAME##_res {                                                    \
		s32 status;                                                    \
		RES_X(__MANGO_MGMT_ROW_DECL)                                   \
	};                                                                     \
	static_assert(sizeof(struct NAME##_arg) <=                             \
			      MANGO_MGMT_RPC_MAX_BODY_WORDS * 4,               \
		      #NAME " arg exceeds wire cap; bump "                     \
			    "MANGO_MGMT_RPC_MAX_BODY_WORDS");                  \
	static_assert(sizeof(struct NAME##_res) <=                             \
			      MANGO_MGMT_RPC_MAX_BODY_WORDS * 4,               \
		      #NAME " res exceeds wire cap; bump "                     \
			    "MANGO_MGMT_RPC_MAX_BODY_WORDS");                  \
	static inline int NAME##_arg_encode(struct xdr_stream *xdr,            \
					    const void *in_)                   \
	{                                                                      \
		const struct NAME##_arg *in = in_;                             \
		(void)in;                                                      \
		ARG_X(__MANGO_MGMT_ROW_ENC)                                    \
		return 0;                                                      \
	}                                                                      \
	static inline int NAME##_arg_decode(struct xdr_stream *xdr,            \
					    void *out_)                        \
	{                                                                      \
		struct NAME##_arg *out = out_;                                 \
		(void)out;                                                     \
		ARG_X(__MANGO_MGMT_ROW_DEC)                                    \
		return 0;                                                      \
	}                                                                      \
	static inline int NAME##_res_encode(struct xdr_stream *xdr,            \
					    const void *in_)                   \
	{                                                                      \
		const struct NAME##_res *in = in_;                             \
		int __ret = mango_mgmt_xdr_encode_s32(xdr, in->status);        \
		if (__ret)                                                     \
			return __ret;                                          \
		RES_X(__MANGO_MGMT_ROW_ENC)                                    \
		return 0;                                                      \
	}                                                                      \
	static inline int NAME##_res_decode(struct xdr_stream *xdr,            \
					    void *out_)                        \
	{                                                                      \
		struct NAME##_res *out = out_;                                 \
		int __ret = mango_mgmt_xdr_decode_s32(xdr, &out->status);      \
		if (__ret)                                                     \
			return __ret;                                          \
		RES_X(__MANGO_MGMT_ROW_DEC)                                    \
		return 0;                                                      \
	}

/* ============================================================
 * DECLARE_OP_PORT — struct + codecs with u32 port prepended.
 * Use for per-port ops; the dispatcher reads port from the wire
 * to resolve the per-port priv before calling the handler.
 * ============================================================ */

#define MANGO_MGMT_RPC_DECLARE_OP_PORT(NAME, ARG_X, RES_X)                     \
	struct NAME##_arg {                                                    \
		u32 port;                                                      \
		ARG_X(__MANGO_MGMT_ROW_DECL)                                   \
	};                                                                     \
	struct NAME##_res {                                                    \
		s32 status;                                                    \
		RES_X(__MANGO_MGMT_ROW_DECL)                                   \
	};                                                                     \
	static_assert(sizeof(struct NAME##_arg) <=                             \
			      MANGO_MGMT_RPC_MAX_BODY_WORDS * 4,               \
		      #NAME " arg exceeds wire cap; bump "                     \
			    "MANGO_MGMT_RPC_MAX_BODY_WORDS");                  \
	static_assert(sizeof(struct NAME##_res) <=                             \
			      MANGO_MGMT_RPC_MAX_BODY_WORDS * 4,               \
		      #NAME " res exceeds wire cap; bump "                     \
			    "MANGO_MGMT_RPC_MAX_BODY_WORDS");                  \
	static inline int NAME##_arg_encode(struct xdr_stream *xdr,            \
					    const void *in_)                   \
	{                                                                      \
		const struct NAME##_arg *in = in_;                             \
		int __ret = mango_mgmt_xdr_encode_u32(xdr, in->port);          \
		if (__ret)                                                     \
			return __ret;                                          \
		ARG_X(__MANGO_MGMT_ROW_ENC)                                    \
		return 0;                                                      \
	}                                                                      \
	static inline int NAME##_arg_decode(struct xdr_stream *xdr,            \
					    void *out_)                        \
	{                                                                      \
		struct NAME##_arg *out = out_;                                 \
		int __ret = mango_mgmt_xdr_decode_u32(xdr, &out->port);        \
		if (__ret)                                                     \
			return __ret;                                          \
		ARG_X(__MANGO_MGMT_ROW_DEC)                                    \
		return 0;                                                      \
	}                                                                      \
	static inline int NAME##_res_encode(struct xdr_stream *xdr,            \
					    const void *in_)                   \
	{                                                                      \
		const struct NAME##_res *in = in_;                             \
		int __ret = mango_mgmt_xdr_encode_s32(xdr, in->status);        \
		if (__ret)                                                     \
			return __ret;                                          \
		RES_X(__MANGO_MGMT_ROW_ENC)                                    \
		return 0;                                                      \
	}                                                                      \
	static inline int NAME##_res_decode(struct xdr_stream *xdr,            \
					    void *out_)                        \
	{                                                                      \
		struct NAME##_res *out = out_;                                 \
		int __ret = mango_mgmt_xdr_decode_s32(xdr, &out->status);      \
		if (__ret)                                                     \
			return __ret;                                          \
		RES_X(__MANGO_MGMT_ROW_DEC)                                    \
		return 0;                                                      \
	}

/* ============================================================
 * DEFINE — handler body wrapper.
 * Emits the void*-typed trampoline the dispatcher calls, the scope
 * constant used by OP_ENTRY, and the typed impl declaration.  The
 * function body follows the macro invocation.
 * ============================================================ */

#define MANGO_MGMT_RPC_DEFINE(NAME, SCOPE)                                     \
	static int NAME##_impl(void *priv, const struct NAME##_arg *args,      \
			       struct NAME##_res *res);                        \
	static int NAME##_handler(void *priv, const void *a, void *r)          \
	{                                                                      \
		return NAME##_impl(priv, a, r);                                \
	}                                                                      \
	static const enum mango_mgmt_rpc_scope NAME##_scope = SCOPE;           \
	static int NAME##_impl(void *priv, const struct NAME##_arg *args,      \
			       struct NAME##_res *res)

/* ============================================================
 * OP_ENTRY — fills one mango_mgmt_proc_desc row for a project's
 * ops table.
 * ============================================================ */

#define MANGO_MGMT_OP_ENTRY(NAME)                                              \
	{                                                                      \
		.scope = NAME##_scope, .handler = NAME##_handler,              \
		.arg_decode = NAME##_arg_decode,                               \
		.arg_encode = NAME##_arg_encode,                               \
		.res_decode = NAME##_res_decode,                               \
		.res_encode = NAME##_res_encode,                               \
		.arg_size = sizeof(struct NAME##_arg),                         \
		.res_size = sizeof(struct NAME##_res), .name = #NAME,          \
	}

/* ============================================================
 * DEFINE_WRAP[_PORT] — Host-side typed _rpc() wrapper.
 *
 * Emits `static inline int NAME##_rpc(int devid[, u32 port], <args>,
 * <*res_out>)` that packs scalars into NAME##_arg, calls
 * mango_mgmt_rpc_call(devid, PRJ, OP, ...), and unpacks RES_X fields
 * into caller-supplied output pointers.  RES output pointers may be
 * NULL to ignore the field.
 *
 * Field names in ARG_X / RES_X must not collide with `devid`, `port`
 * (port variant only), `__a`, `__r`, or `__ret`.
 * ============================================================ */

/* Client-only proc_desc: just the two codecs the host actually uses
 * (arg_encode / res_decode), plus name for tracing.  The other fields
 * (handler / arg_decode / res_encode / scope / sizes) are server-side
 * and would force the SoC handler module to be loaded on the host. */
#define __MANGO_MGMT_RPC_CLIENT_DESC(NAME)                                     \
	{                                                                      \
		.arg_encode = NAME##_arg_encode,                               \
		.res_decode = NAME##_res_decode, .name = #NAME,                \
	}

#define MANGO_MGMT_RPC_DEFINE_WRAP(NAME, PRJ, OP, ARG_X, RES_X)                \
	static inline int NAME##_rpc(int devid ARG_X(                          \
		__MANGO_MGMT_ROW_PARAM) RES_X(__MANGO_MGMT_ROW_OUT_PARAM))     \
	{                                                                      \
		static const struct mango_mgmt_proc_desc __desc =              \
			__MANGO_MGMT_RPC_CLIENT_DESC(NAME);                    \
		struct NAME##_arg __a = { ARG_X(__MANGO_MGMT_ROW_INIT) };      \
		struct NAME##_res __r = {};                                    \
		int __ret = mango_mgmt_rpc_call(devid, (PRJ), (OP), &__desc,   \
						&__a, &__r);                   \
		if (__ret)                                                     \
			return __ret;                                          \
		RES_X(__MANGO_MGMT_ROW_EXTRACT)                                \
		return 0;                                                      \
	}

#define MANGO_MGMT_RPC_DEFINE_WRAP_PORT(NAME, PRJ, OP, ARG_X, RES_X)           \
	static inline int NAME##_rpc(                                          \
		int devid, u32 port ARG_X(__MANGO_MGMT_ROW_PARAM)              \
				   RES_X(__MANGO_MGMT_ROW_OUT_PARAM))          \
	{                                                                      \
		static const struct mango_mgmt_proc_desc __desc =              \
			__MANGO_MGMT_RPC_CLIENT_DESC(NAME);                    \
		struct NAME##_arg __a = { .port = port,                        \
					  ARG_X(__MANGO_MGMT_ROW_INIT) };      \
		struct NAME##_res __r = {};                                    \
		int __ret = mango_mgmt_rpc_call(devid, (PRJ), (OP), &__desc,   \
						&__a, &__r);                   \
		if (__ret)                                                     \
			return __ret;                                          \
		RES_X(__MANGO_MGMT_ROW_EXTRACT)                                \
		return 0;                                                      \
	}

/* ============================================================
 * DECLARE_CALL[_PORT] — one-shot: structs + codecs + _rpc wrapper.
 * The common case for a project header: one macro invocation per op
 * and all sides (struct, codec, Host-side call) are generated.
 * ============================================================ */

#define MANGO_MGMT_RPC_DECLARE_CALL(NAME, PRJ, OP, ARG_X, RES_X)               \
	MANGO_MGMT_RPC_DECLARE_OP(NAME, ARG_X, RES_X)                          \
	MANGO_MGMT_RPC_DEFINE_WRAP(NAME, PRJ, OP, ARG_X, RES_X)

#define MANGO_MGMT_RPC_DECLARE_CALL_PORT(NAME, PRJ, OP, ARG_X, RES_X)          \
	MANGO_MGMT_RPC_DECLARE_OP_PORT(NAME, ARG_X, RES_X)                     \
	MANGO_MGMT_RPC_DEFINE_WRAP_PORT(NAME, PRJ, OP, ARG_X, RES_X)

#endif /* _MANGO_MGMT_RPC_MACROS_H_ */
