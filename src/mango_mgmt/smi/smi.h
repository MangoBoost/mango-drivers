// SPDX-License-Identifier: GPL-2.0-only
/*
 * smi.h - This file contains generic SMI driver definitions.
 *
 * Copyright (c) 2023, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_DRIVER_H
#define SMI_DRIVER_H

#include <linux/printk.h>

/* Meta Information */
#define MDL_VERSION "1.0.0"
#define MDL_DESCRIPTION "MangoBoost SMI driver for management and monitoring"
#define MDL_AUTHOR "MangoBoost, Inc."
#define MDL_RELDATE "2025-06-01"
#define MDL_LICENSE "GPL"

#include "smi_name.h"

/* Enables debug messages in dmesg */
extern bool smi_debug_enabled;

#undef pr_fmt
#define pr_fmt(fmt) SMI_NAME ": " fmt

#define PR_ERR(fmt, arg...) pr_err("ERROR           : " fmt "\n", ##arg)
#define PR_INFO(fmt, arg...) pr_info("INFO            : " fmt "\n", ##arg)
#define PR_WARN(fmt, arg...) pr_warn("WARNING         : " fmt "\n", ##arg)
#define PR_CRIT_WARN(fmt, arg...) pr_warn("CRITICAL WARNING: " fmt "\n", ##arg)
#define PR_DBG(fmt, arg...)                                                    \
	do {                                                                   \
		if (smi_debug_enabled)                                         \
			pr_debug("DEBUG           : " fmt "\n", ##arg);        \
	} while (0)

#define SUCCESS 0
#define FAILURE -1

#define BDF_STR_LEN 16

#endif /* SMI_DRIVER_H */
