// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - Terminal Server (SoC only)
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */
#ifndef _MANGO_MGMT_TERM_SERVER_H_
#define _MANGO_MGMT_TERM_SERVER_H_

struct mango_mgmt_dev;

int mango_mgmt_term_server_init(struct mango_mgmt_dev *mgmt_dev);
void mango_mgmt_term_server_exit(struct mango_mgmt_dev *mgmt_dev);

#endif
