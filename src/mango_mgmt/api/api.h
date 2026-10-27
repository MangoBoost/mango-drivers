/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MangoBoost Management Module - API Layer Lifecycle
 *
 * Internal header used by frontend.c and mod.c for init/exit calls.
 * The public API (rpc_call, etc.) is in mango_mgmt_api.h.
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_API_LIFECYCLE_H_
#define _MANGO_MGMT_API_LIFECYCLE_H_

struct mango_mgmt_dev;

/* Called by mod.c at module init -- registers mgmt's own meta project.
 * Returns 0 on success, -errno on failure. */
int mango_mgmt_api_module_init(void);

/* Called by frontend during device init -- registers device in global xarray,
 * starts RX dispatch kthread for MGMT_CH_API session, grants initial credits.
 * Returns 0 on success, -errno on failure. */
int mango_mgmt_api_init(struct mango_mgmt_dev *mgmt_dev);

/* Called by frontend during device cleanup -- stops RX dispatch kthread,
 * releases all signal entries and removes device from global xarray. */
void mango_mgmt_api_exit(struct mango_mgmt_dev *mgmt_dev);

/* Called by mod.c at module exit -- destroys global callback and device xarrays.
 * Warns about any leaked (unregistered) callbacks. */
void mango_mgmt_api_module_exit(void);

#endif /* _MANGO_MGMT_API_LIFECYCLE_H_ */
