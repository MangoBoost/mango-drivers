/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MangoBoost Management Module - RPC Internal Definitions
 *
 * SunRPC-specific wiring: program/version descriptors and procedure
 * table externs.  Only included by the rpc_*.c files that need the
 * full SunRPC API.  api.c uses internal.h (which forward-declares
 * SunRPC types) to avoid header conflicts with libsmi.
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_RPC_H_
#define _MANGO_MGMT_RPC_H_

#include <linux/sunrpc/clnt.h>
#include <linux/sunrpc/svc.h>

#include "../mango_mgmt_api.h"

/* RPC program number (experimental range: 0x20000000-0x3FFFFFFF) */
#define MANGO_MGMT_RPC_PROGRAM 0x20004D42u /* "MB" */
#define MANGO_MGMT_RPC_VERSION 1

/* SunRPC procs; the client and server tables have exactly these entries:
 *   NULL       — ping
 *   RPC_CALL   — project ops, demultiplexed by (prj_id, proj_op) */
#define MANGO_MGMT_RPC_NULL 0
#define MANGO_MGMT_RPC_CALL 1
#define MANGO_MGMT_RPC_NPROC 2

/* XDR sizes for NULL (RPC_CALL sizes are inlined in rpc_xdr.c). */
#define MGMT_NULL_ARG_SZ 0
#define MGMT_NULL_RES_SZ 0

/* Forward-channel procedure table (defined in rpc_xdr.c) */
extern const struct rpc_procinfo mango_mgmt_fwd_procs[];

#endif /* _MANGO_MGMT_RPC_H_ */
