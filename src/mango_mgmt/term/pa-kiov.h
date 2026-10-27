// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost VirtIO - PA-addressed kiov helpers
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_PA_KIOV_H_
#define _MANGO_PA_KIOV_H_

#include <linux/types.h>

#include "vring_host.h"

/* Sum remaining bytes in a chain starting from kiov->i */
size_t mango_mgmt_kiov_total_len(const struct mango_mgmt_kiov *kiov);

/* Copy `len` bytes from PA-addressed read kiov into flat kernel buffer.
 * Returns bytes copied (< len if kiov is exhausted or page invalid). */
ssize_t mango_mgmt_pa_kiov_to_buf(void *buf, struct mango_mgmt_kiov *kiov,
				  size_t len);

/* Copy `len` bytes from flat kernel buffer into PA-addressed write kiov.
 * Returns bytes copied (< len if kiov is exhausted or page invalid). */
ssize_t mango_mgmt_pa_kiov_from_buf(struct mango_mgmt_kiov *kiov,
				    const void *buf, size_t len);

#endif /* _MANGO_PA_KIOV_H_ */
