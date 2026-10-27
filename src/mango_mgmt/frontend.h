// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - Frontend Lifecycle
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_FRONTEND_H_
#define _MANGO_MGMT_FRONTEND_H_

struct mango_mgmt_dev;

/* Bring up what the descriptor says the function carries (MGMT, SMI or
 * both); a component that fails is left out, a probe with none fails. */
int mango_mgmt_frontend_init_device(struct mango_mgmt_dev *mgmt);
void mango_mgmt_frontend_cleanup_device(struct mango_mgmt_dev *mgmt);

/* Around a bus reset or shutdown: stop both components without destroying
 * anything. resume returns MPIPE's error; SMI stays quiesced until the
 * function is re-attached. */
void mango_mgmt_frontend_quiesce(struct mango_mgmt_dev *mgmt);
int mango_mgmt_frontend_resume(struct mango_mgmt_dev *mgmt);

#endif
