// SPDX-License-Identifier: GPL-2.0
/*
 * smi_log.h - Device-based logging macros for libsmi.
 *
 * The SMI_DEV_* macros log on a bare struct device * so the command engine,
 * its rows and the sensor helpers can log without depending on the S3
 * management context. smi_smc_control.h keeps the SMI_*
 * spellings as thin wrappers (SMI_ERR(ctxt, ...) -> SMI_DEV_ERR((ctxt)->dev,
 * ...)) so the S3/S4 call sites stay unchanged.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef _SMI_LOG_H_
#define _SMI_LOG_H_

#include <linux/device.h>

#define SMI_DEV_ERR(dev, fmt, arg...)                                          \
	dev_err((dev), "ERROR           : " fmt "\n", ##arg)
#define SMI_DEV_ERR_ONCE(dev, fmt, arg...)                                     \
	dev_err_once((dev), "ERROR           : " fmt "\n", ##arg)
/* For errors a polled path can repeat every scrape. */
#define SMI_DEV_ERR_RATELIMITED(dev, fmt, arg...)                              \
	dev_err_ratelimited((dev), "ERROR           : " fmt "\n", ##arg)
#define SMI_DEV_WARN_ONCE(dev, fmt, arg...)                                    \
	dev_warn_once((dev), "WARNING         : " fmt "\n", ##arg)
#define SMI_DEV_INFO(dev, fmt, arg...)                                         \
	dev_info((dev), "INFO            : " fmt "\n", ##arg)
/* For an audit line one request type can repeat thousands of times. */
#define SMI_DEV_INFO_RATELIMITED(dev, fmt, arg...)                             \
	dev_info_ratelimited((dev), "INFO            : " fmt "\n", ##arg)
#define SMI_DEV_WARN(dev, fmt, arg...)                                         \
	dev_warn((dev), "WARNING         : " fmt "\n", ##arg)
#define SMI_DEV_DBG(dev, fmt, arg...)                                          \
	dev_dbg((dev), "DEBUG           : " fmt "\n", ##arg)
#define SMI_DEV_VDBG(dev, fmt, arg...)                                         \
	dev_vdbg((dev), "VERBOSE DEBUG           : " fmt "\n", ##arg)
#define SMI_DEV_SMC_LOG(dev, fmt, arg...) dev_info((dev), fmt "\n", ##arg)

#endif /* _SMI_LOG_H_ */
