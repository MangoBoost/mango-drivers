// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/errno.h>

#include "dev.h"
#include "regmap.h"
#include "monitor.h"
#include "monitor_api.h"
#include "smi.h"
#include "smi/smi_api.h"

static struct mango_mgmt_monitor *mgmt_monitors[MGMT_MONITOR_TYPE_NUM];

int mango_mgmt_monitor_register()
{
	mgmt_monitors[MGMT_MONITOR_TYPE_SMI] = &mango_mgmt_smi;

	return 0;
}

void mango_mgmt_monitor_unregister()
{
	int i;

	for (i = 0; i < MGMT_MONITOR_TYPE_NUM; i++)
		mgmt_monitors[i] = NULL;
}

struct mango_mgmt_monitor *mango_mgmt_monitor_get(struct mango_mgmt_dev *mgmt)
{
	struct mango_mgmt_monitor *monitor = NULL;

	if (mgmt->desc->smi != MGMT_SMI_NONE)
		monitor = mgmt_monitors[MGMT_MONITOR_TYPE_SMI];

	if (monitor && monitor->claim(mgmt) == 0)
		return monitor;

	return NULL;
}

/* ===== Implementation-agnostic monitor API =====
 *
 * Resolve a caller-supplied HW address to its device and dispatch to the
 * per-device monitor backend, selected at probe time.
 */

/* No management device answers at @hw_addr -- none registered, or its MGMT
 * component is down -- so the SMI backend resolves the address itself. */
int mango_mgmt_monitor_get_info(const char *hw_addr,
				enum mango_mgmt_monitor_card_info type, u8 *buf,
				size_t len)
{
	struct mango_mgmt_dev *mgmt;
	enum mango_mgmt_monitor_info info;
	ssize_t ret;

	if (!buf || !len)
		return -EINVAL;

	switch (type) {
	case MANGO_MGMT_MONITOR_CARD_INFO_SERIAL:
		info = MGMT_MONITOR_INFO_SERIAL;
		break;
	case MANGO_MGMT_MONITOR_CARD_INFO_MACADDR:
		info = MGMT_MONITOR_INFO_MACADDR;
		break;
	default:
		return -EINVAL;
	}

	mgmt = mango_mgmt_dev_by_addr(hw_addr);
	if (!mgmt || !mgmt->monitor || !mgmt->monitor->get_info)
		ret = mango_mgmt_smi_get_info_by_addr(hw_addr, info, buf, len);
	else
		ret = mgmt->monitor->get_info(mgmt, info, buf, len);

	/* This API reports success as 0; the length stays inside the vtable. */
	return ret < 0 ? ret : 0;
}
EXPORT_SYMBOL_GPL(mango_mgmt_monitor_get_info);

int mango_mgmt_monitor_get_sensor(const char *hw_addr,
				  enum mango_mgmt_monitor_sensor sensor,
				  struct mango_mgmt_sensor_reading *out)
{
	struct mango_mgmt_dev *mgmt;

	if (!out)
		return -EINVAL;

	mgmt = mango_mgmt_dev_by_addr(hw_addr);
	if (!mgmt || !mgmt->monitor)
		return mango_mgmt_smi_get_sensor_by_addr(hw_addr, sensor, out);

	if (!mgmt->monitor->get_sensor)
		return -EOPNOTSUPP;

	return mgmt->monitor->get_sensor(mgmt, sensor, out);
}
EXPORT_SYMBOL_GPL(mango_mgmt_monitor_get_sensor);

int mango_mgmt_monitor_get_qsfp_status(const char *hw_addr, u8 link,
				       struct smi_qsfp_status *status,
				       u8 *num_links)
{
	struct mango_mgmt_dev *mgmt;

	if (!status)
		return -EINVAL;

	/* This API carries SMI's own record, so the fallback is SMI's read. */
	mgmt = mango_mgmt_dev_by_addr(hw_addr);
	if (!mgmt || !mgmt->monitor)
		return mango_mgmt_smi_get_qsfp_status(hw_addr, link, status,
						      num_links);

	if (!mgmt->monitor->get_qsfp_status)
		return -EOPNOTSUPP;

	return mgmt->monitor->get_qsfp_status(mgmt, link, status, num_links);
}
EXPORT_SYMBOL_GPL(mango_mgmt_monitor_get_qsfp_status);

int mango_mgmt_monitor_register_alarm_notifier(const char *hw_addr,
					       struct notifier_block *nb)
{
	struct mango_mgmt_dev *mgmt = mango_mgmt_dev_by_addr(hw_addr);

	/* The SMI notifier is driver-wide, so it answers without a device. */
	if (!mgmt || !mgmt->monitor)
		return mango_mgmt_smi_register_alarm_notifier(nb);

	if (!mgmt->monitor->register_alarm_notifier)
		return -EOPNOTSUPP;

	return mgmt->monitor->register_alarm_notifier(mgmt, nb);
}
EXPORT_SYMBOL_GPL(mango_mgmt_monitor_register_alarm_notifier);

int mango_mgmt_monitor_unregister_alarm_notifier(const char *hw_addr,
						 struct notifier_block *nb)
{
	struct mango_mgmt_dev *mgmt = mango_mgmt_dev_by_addr(hw_addr);

	if (!mgmt || !mgmt->monitor)
		return mango_mgmt_smi_unregister_alarm_notifier(nb);

	if (!mgmt->monitor->unregister_alarm_notifier)
		return -EOPNOTSUPP;

	return mgmt->monitor->unregister_alarm_notifier(mgmt, nb);
}
EXPORT_SYMBOL_GPL(mango_mgmt_monitor_unregister_alarm_notifier);
