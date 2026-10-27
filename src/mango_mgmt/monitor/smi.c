// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - SMI-based monitor
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include <linux/string.h>

#include "dev.h"
#include "monitor.h"

#include "smi.h"
#include "smi_glue.h"

static int mango_mgmt_smi_claim(struct mango_mgmt_dev *mgmt)
{
	/* Nothing to start: the SMI command plane comes up on its own, and
	 * this backend answers through whichever SMI serves the device. */
	return 0;
}

/* SMI vocabulary <-> the monitor API's neutral types. QSFP is not here:
 * that API carries the SMI struct itself. */
static enum smi_sensor_type
smi_sensor_type_of(enum mango_mgmt_monitor_sensor sensor)
{
	switch (sensor) {
	case MANGO_MGMT_MONITOR_SENSOR_TOTAL_POWER:
		return SMI_SENSOR_POWER;
	case MANGO_MGMT_MONITOR_SENSOR_TEMP_BOARD:
		return SMI_SENSOR_TEMP_BOARD;
	case MANGO_MGMT_MONITOR_SENSOR_TEMP_FPGA:
		return SMI_SENSOR_TEMP_FPGA;
	case MANGO_MGMT_MONITOR_SENSOR_TEMP_ETHERNET:
		return SMI_SENSOR_TEMP_ETHERNET;
	default:
		return SMI_SENSOR_MAX;
	}
}

static void smi_reading_to_monitor(const struct smi_sensor_reading *reading,
				   struct mango_mgmt_sensor_reading *out)
{
	static const enum smi_sensor_threshold to_smi_threshold[] = {
		[MANGO_MGMT_SENSOR_THRESHOLD_LOWER_FATAL] =
			SMI_SENSOR_THRESHOLD_LOWER_FATAL,
		[MANGO_MGMT_SENSOR_THRESHOLD_LOWER_CRIT] =
			SMI_SENSOR_THRESHOLD_LOWER_CRIT,
		[MANGO_MGMT_SENSOR_THRESHOLD_LOWER_WARN] =
			SMI_SENSOR_THRESHOLD_LOWER_WARN,
		[MANGO_MGMT_SENSOR_THRESHOLD_UPPER_WARN] =
			SMI_SENSOR_THRESHOLD_UPPER_WARN,
		[MANGO_MGMT_SENSOR_THRESHOLD_UPPER_CRIT] =
			SMI_SENSOR_THRESHOLD_UPPER_CRIT,
		[MANGO_MGMT_SENSOR_THRESHOLD_UPPER_FATAL] =
			SMI_SENSOR_THRESHOLD_UPPER_FATAL,
	};
	int i;

	memset(out, 0, sizeof(*out));
	out->value = reading->value;
	out->min = reading->min;
	out->max = reading->max;
	out->avg = reading->avg;
	out->status = reading->status;

	if (reading->fields_valid & SMI_SENSOR_HAS_VALUE)
		out->fields_valid |= MANGO_MGMT_SENSOR_HAS_VALUE;
	if (reading->fields_valid & SMI_SENSOR_HAS_MIN)
		out->fields_valid |= MANGO_MGMT_SENSOR_HAS_MIN;
	if (reading->fields_valid & SMI_SENSOR_HAS_MAX)
		out->fields_valid |= MANGO_MGMT_SENSOR_HAS_MAX;
	if (reading->fields_valid & SMI_SENSOR_HAS_AVG)
		out->fields_valid |= MANGO_MGMT_SENSOR_HAS_AVG;

	for (i = 0; i < MANGO_MGMT_SENSOR_THRESHOLD_MAX; i++) {
		if (!(reading->thresholds_valid & BIT(to_smi_threshold[i])))
			continue;
		out->thresholds[i] = reading->thresholds[to_smi_threshold[i]];
		out->thresholds_valid |= BIT(i);
	}
}

/* Buffer length for a board info item; 0 when the monitor has no such item. */
static size_t smi_board_info_of(enum mango_mgmt_monitor_info info,
				enum smi_board_info *type)
{
	switch (info) {
	case MGMT_MONITOR_INFO_SERIAL:
		*type = SMI_BOARD_INFO_SERIAL_NUMBER;
		return MANGO_MGMT_MONITOR_SERIAL_LEN;
	case MGMT_MONITOR_INFO_MACADDR:
		*type = SMI_BOARD_INFO_MAC_ADDR;
		return MANGO_MGMT_MONITOR_MACADDR_LEN;
	default:
		return 0;
	}
}

/* Address-keyed entry points: a design without a management IP registers no
 * management device yet answers monitoring from its SMI. */
int mango_mgmt_smi_get_sensor_by_addr(const char *hw_addr,
				      enum mango_mgmt_monitor_sensor sensor,
				      struct mango_mgmt_sensor_reading *out)
{
	enum smi_sensor_type type = smi_sensor_type_of(sensor);
	struct smi_sensor_reading reading;
	int ret;

	if (!hw_addr)
		return -ENODEV;
	if (type == SMI_SENSOR_MAX)
		return -EINVAL;

	ret = mango_mgmt_smi_get_sensor_info(hw_addr, type, &reading);
	if (ret)
		return ret;

	smi_reading_to_monitor(&reading, out);
	return 0;
}

ssize_t mango_mgmt_smi_get_info_by_addr(const char *hw_addr,
					enum mango_mgmt_monitor_info info,
					u8 *buf, size_t len)
{
	enum smi_board_info type;
	size_t need = smi_board_info_of(info, &type);

	if (!hw_addr || !need || len < need)
		return -EINVAL;

	/* libsmi writes only the bytes the card reported, and says how many. */
	memset(buf, 0, need);
	return mango_mgmt_smi_get_board_info(hw_addr, type, buf, need);
}

/* Per-device entry points: the SMI serving the device, own or sibling. */
static int mango_mgmt_smi_get_sensor(struct mango_mgmt_dev *mgmt,
				     enum mango_mgmt_monitor_sensor sensor,
				     struct mango_mgmt_sensor_reading *out)
{
	enum smi_sensor_type type = smi_sensor_type_of(sensor);
	struct smi_sensor_reading reading;
	int ret;

	if (type == SMI_SENSOR_MAX)
		return -EINVAL;

	ret = mango_mgmt_smi_dev_sensor(mgmt, type, &reading);
	if (ret)
		return ret;

	smi_reading_to_monitor(&reading, out);
	return 0;
}

static ssize_t mango_mgmt_smi_get_info(struct mango_mgmt_dev *mgmt,
				       enum mango_mgmt_monitor_info info,
				       u8 *buf, size_t len)
{
	enum smi_board_info type;
	size_t need = smi_board_info_of(info, &type);

	if (!need || len < need)
		return -EINVAL;

	memset(buf, 0, need);
	return mango_mgmt_smi_dev_board_info(mgmt, type, buf, need);
}

static int mango_mgmt_smi_register_alarm(struct mango_mgmt_dev *mgmt,
					 struct notifier_block *nb)
{
	return mango_mgmt_smi_register_alarm_notifier(nb);
}

static int mango_mgmt_smi_unregister_alarm(struct mango_mgmt_dev *mgmt,
					   struct notifier_block *nb)
{
	return mango_mgmt_smi_unregister_alarm_notifier(nb);
}

struct mango_mgmt_monitor mango_mgmt_smi = {
	.type = MGMT_MONITOR_TYPE_SMI,
	.claim = mango_mgmt_smi_claim,
	.get_sensor = mango_mgmt_smi_get_sensor,
	.get_info = mango_mgmt_smi_get_info,
	.get_qsfp_status = mango_mgmt_smi_dev_qsfp,
	.register_alarm_notifier = mango_mgmt_smi_register_alarm,
	.unregister_alarm_notifier = mango_mgmt_smi_unregister_alarm,
};
