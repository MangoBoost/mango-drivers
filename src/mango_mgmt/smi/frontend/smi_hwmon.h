// SPDX-License-Identifier: GPL-2.0-only
/*
 * smi_hwmon.h - What hwmon's vocabulary means against the sensor model.
 *
 * A surface decides which attributes a channel has and what each is called;
 * this decides what a record answers for one. Shared by both surfaces and by
 * the ioctl that speaks hwmon's words without being hwmon.
 *
 * Copyright (c) 2023 Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_HWMON_H
#define SMI_HWMON_H

#include <linux/hwmon.h>

#include "contract/smi/smi_abi.h" /* SmiThr_* */

struct pf_dev_struct;
struct smi_sensor;

/**
 * enum smi_sensor_attribute - What a caller wants of a record.
 * @SENSOR_ATTR_INSTANT: The current value of the sensor.
 * @SENSOR_ATTR_MAX: The historical max value of the sensor.
 * @SENSOR_ATTR_AVERAGE: The average value of the sensor since boot.
 * @SENSOR_ATTR_STATUS: Sensor status.
 * @SENSOR_ATTR_WARN: Upper warning limit.
 * @SENSOR_ATTR_WARN_A: Upper warning alarm.
 * @SENSOR_ATTR_CRIT: Upper critical limit.
 * @SENSOR_ATTR_CRIT_A: Upper critical alarm.
 * @SENSOR_ATTR_FATAL: Upper fatal limit.
 * @SENSOR_ATTR_FATAL_A: Upper fatal alarm.
 * @SENSOR_ATTR_LWARN: Lower warning limit.
 * @SENSOR_ATTR_LCRIT: Lower critical limit.
 * @SENSOR_ATTR_INVALID: No such attribute.
 *
 * The meaning, not the spelling: which hwmon file carries which of these is a
 * surface's decision, and the two surfaces do not agree on it.
 */
enum smi_sensor_attribute {
	SENSOR_ATTR_INSTANT,
	SENSOR_ATTR_MAX,
	SENSOR_ATTR_AVERAGE,
	SENSOR_ATTR_STATUS,
	SENSOR_ATTR_WARN,
	SENSOR_ATTR_WARN_A,
	SENSOR_ATTR_CRIT,
	SENSOR_ATTR_CRIT_A,
	SENSOR_ATTR_FATAL,
	SENSOR_ATTR_FATAL_A,
	SENSOR_ATTR_LWARN,
	SENSOR_ATTR_LCRIT,
	SENSOR_ATTR_INVALID,
};

/**
 * struct smi_attr_info - What one neutral attribute is, to the model.
 * @bit: SmiSensMask_* bit position, -1 where no bit carries the attribute.
 *	 A bit below SMI_THR_CNT is also the record's thr[] index.
 * @live: is the value a published cell, which a refresh moves, rather than the
 *	  descriptor the card stated once?
 */
struct smi_attr_info {
	int8_t bit;
	bool live;
};

/**
 * smi_hwmon_attr_info() - The model's description of one neutral attribute.
 *
 * The card's mask says whether an attribute is there at all, and the wire
 * numbers its bits and the record's thr[] slots alike -- so one answer serves
 * the surface deciding which files exist, the read finding the value, and the
 * read deciding whether to refresh first.
 *
 * Return: the description; an attribute outside the model reads as one no bit
 *	   carries and no refresh moves.
 */
struct smi_attr_info smi_hwmon_attr_info(enum smi_sensor_attribute attr);

/* The limit-only view of smi_hwmon_attr_info(). Documented at each definition. */
int smi_hwmon_thr_slot(enum smi_sensor_attribute attr, enum smi_thr *slot);
int smi_hwmon_attr_value(const struct smi_sensor *rec,
			 enum smi_sensor_attribute attr, s64 *value);
int smi_hwmon_scale(enum hwmon_sensor_types type, int8_t unit_exp, s64 val,
		    long *out);

/**
 * smi_hwmon_set_interval() - Set the host's cache window, in milliseconds.
 * @pf_dev: PCI device data structure.
 * @ms: the window. 0 means refresh on every read.
 *
 * The one writer of it: the store holds a halfword, and folding a larger
 * number would set a window nobody asked for -- 0 among them.
 *
 * Return: 0, or -EINVAL for a value the store cannot hold.
 */
int smi_hwmon_set_interval(struct pf_dev_struct *pf_dev, long ms);

#endif /* SMI_HWMON_H */
