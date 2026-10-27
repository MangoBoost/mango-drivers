// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_MONITOR_SMI_H_
#define _MANGO_MGMT_MONITOR_SMI_H_

extern struct mango_mgmt_monitor mango_mgmt_smi;

/* Address-keyed reads, for a design that registers no management device to
 * carry the monitor. Both translate the SMI vocabulary, so a caller above this
 * backend stays in the neutral types.
 */
int mango_mgmt_smi_get_sensor_by_addr(const char *hw_addr,
				      enum mango_mgmt_monitor_sensor sensor,
				      struct mango_mgmt_sensor_reading *out);
ssize_t mango_mgmt_smi_get_info_by_addr(const char *hw_addr,
					enum mango_mgmt_monitor_info info,
					u8 *buf, size_t len);

#endif
