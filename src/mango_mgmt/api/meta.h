/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MangoBoost Management Module - Meta project op schemas
 *
 * mgmt's own project (MANGO_MGMT_PRJ_META).  Implemented like any
 * aux project — see api/meta.c for the handler and attach/detach.
 * This module is intentionally the smallest working reference for
 * how to add a new project to the RPC framework.
 *
 * The typed arg/res structs generated from the X-macro schemas
 * below are consumed by both sides: the SoC handler in meta.c and
 * the Host helpers in rpc_client.c.  api.c stays libsmi-compatible
 * by not including this header.
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_API_META_H_
#define _MANGO_MGMT_API_META_H_

#include "../mango_mgmt_api.h"
#include "../mango_mgmt_rpc_macros.h"

enum mango_mgmt_meta_op {
	MANGO_MGMT_META_OP_INTROSPECT = 0,
	MANGO_MGMT_META_OP_MAX,
};

#define MANGO_MGMT_META_INTROSPECT_ARG(X)                                      \
	X(u32, target_prj_id)                                                  \
	X(u32, scope)                                                          \
	X(u32, target_port)

#define MANGO_MGMT_META_INTROSPECT_RES(X) X(u32, ready)

MANGO_MGMT_RPC_DECLARE_CALL(mango_mgmt_meta_introspect, MANGO_MGMT_PRJ_META,
			    MANGO_MGMT_META_OP_INTROSPECT,
			    MANGO_MGMT_META_INTROSPECT_ARG,
			    MANGO_MGMT_META_INTROSPECT_RES)

#endif /* _MANGO_MGMT_API_META_H_ */
