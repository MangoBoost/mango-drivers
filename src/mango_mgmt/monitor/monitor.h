// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_MONITOR_H_
#define _MANGO_MGMT_MONITOR_H_

#include "dev.h"

#include <monitor_api.h>

/* Defined in mango_mgmt/smi/smi_api.h; passed by pointer only. */
struct smi_qsfp_status;

enum mango_mgmt_monitor_type {
	MGMT_MONITOR_TYPE_SMI = 0,
	MGMT_MONITOR_TYPE_NUM,
};

enum mango_mgmt_monitor_power {
	MGMT_MONITOR_POWER_MAX = 0,
	MGMT_MONITOR_POWER_AVG,
	MGMT_MOINTOR_POWER_NUM,
};

enum mango_mgmt_monitor_temp {
	MGMT_MONITOR_TEMP_MAX = 0,
	MGMT_MONITOR_TEMP_AVG,
	MGMT_MOINTOR_TEMP_NUM,
};

enum mango_mgmt_monitor_temp_src {
	MGMT_MONITOR_TEMP_SRC_BOARD = 0,
	MGMT_MONITOR_TEMP_SRC_FPGA,
	MGMT_MONITOR_TEMP_SRC_ETHERNET,
	MGMT_MONITOR_TEMP_SRC_NUM,
};

enum mango_mgmt_monitor_info {
	MGMT_MONITOR_INFO_SERIAL = 0,
	MGMT_MONITOR_INFO_MACADDR,
	MGMT_MOINTOR_INFO_NUM,
};

/* Defined in smi/smi_api.h; backends only pass the pointer through. */
struct smi_qsfp_status;

struct mango_mgmt_monitor {
	enum mango_mgmt_monitor_type type;

	/* Whether this backend can be the device's monitor: selectable, not
	 * ready to answer -- SMI claims before its own attach. */
	int (*claim)(struct mango_mgmt_dev *);
	/* One read per sensor, reporting whatever the backend has. A backend
	 * without a current value or thresholds simply leaves them out. The
	 * types are the neutral ones, so a caller never sees which backend
	 * answered. */
	int (*get_sensor)(struct mango_mgmt_dev *,
			  enum mango_mgmt_monitor_sensor sensor,
			  struct mango_mgmt_sensor_reading *out);
	/* Reads one info item into the caller's buffer. Returns the bytes
	 * filled, or a negative error; a buffer shorter than the item's
	 * published length is -EINVAL. */
	ssize_t (*get_info)(struct mango_mgmt_dev *,
			    enum mango_mgmt_monitor_info, u8 *buf, size_t len);
	int (*get_qsfp_status)(struct mango_mgmt_dev *, u8 link,
			       struct smi_qsfp_status *status, u8 *num_links);
	int (*register_alarm_notifier)(struct mango_mgmt_dev *,
				       struct notifier_block *);
	int (*unregister_alarm_notifier)(struct mango_mgmt_dev *,
					 struct notifier_block *);
};

struct mango_mgmt_monitor *mango_mgmt_monitor_get(struct mango_mgmt_dev *);

int mango_mgmt_monitor_register(void);
void mango_mgmt_monitor_unregister(void);

#endif
