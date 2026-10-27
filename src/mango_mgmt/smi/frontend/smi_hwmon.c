// SPDX-License-Identifier: GPL-2.0-only
/*
 * smi_hwmon.c - What hwmon's vocabulary means against the sensor model.
 *
 * A surface decides which attributes a channel has and what they are called;
 * this decides what a record answers for one, and in what unit. Both surfaces
 * read it, and so does the ioctl that speaks hwmon's words without being it.
 *
 * Copyright (c) 2023 - 2025 Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include <linux/hwmon.h>
#include <linux/types.h>

#include "contract/smi/smi_abi.h" /* SmiThr_* -- the model's threshold slots */

#include "core/smi_sensor.h"

#include "smi_hwmon.h"
#include "smi_top.h"

/* One exhaustive switch, so a meaning added to the enum inherits neither
 * answer silently. An alarm shares its limit's bit and reads a published
 * value; the running pair's bits sit past the threshold array. */
struct smi_attr_info smi_hwmon_attr_info(enum smi_sensor_attribute attr)
{
	switch (attr) {
	case SENSOR_ATTR_INSTANT:
	case SENSOR_ATTR_STATUS:
		return (struct smi_attr_info){ .bit = -1, .live = true };

	case SENSOR_ATTR_AVERAGE:
		return (struct smi_attr_info){ .bit = SmiThr_Avg,
					       .live = true };

	case SENSOR_ATTR_MAX:
		return (struct smi_attr_info){ .bit = SmiThr_Max,
					       .live = true };

	case SENSOR_ATTR_WARN:
		return (struct smi_attr_info){ .bit = SmiThr_UpperWarn };

	case SENSOR_ATTR_CRIT:
		return (struct smi_attr_info){ .bit = SmiThr_UpperCrit };

	case SENSOR_ATTR_FATAL:
		return (struct smi_attr_info){ .bit = SmiThr_UpperFatal };

	case SENSOR_ATTR_LWARN:
		return (struct smi_attr_info){ .bit = SmiThr_LowerWarn };

	case SENSOR_ATTR_LCRIT:
		return (struct smi_attr_info){ .bit = SmiThr_LowerCrit };

	/* A comparison against a limit, so the limit's bit and the reading's
	 * freshness both. */
	case SENSOR_ATTR_WARN_A:
		return (struct smi_attr_info){ .bit = SmiThr_UpperWarn,
					       .live = true };

	case SENSOR_ATTR_CRIT_A:
		return (struct smi_attr_info){ .bit = SmiThr_UpperCrit,
					       .live = true };

	case SENSOR_ATTR_FATAL_A:
		return (struct smi_attr_info){ .bit = SmiThr_UpperFatal,
					       .live = true };

	case SENSOR_ATTR_INVALID:
		break;
	}

	return (struct smi_attr_info){ .bit = -1 };
}

/**
 * smi_hwmon_thr_slot() - The model slot an alarm or limit attribute reads.
 * @attr: Attribute to resolve.
 * @slot: Set to the threshold slot when there is one.
 *
 * The limit-only view of smi_hwmon_attr_info(): a threshold is a bit that also
 * indexes thr[]. The running pair's bits sit past that array, which is what
 * keeps the frozen surface's visibility rule calling them not-limits.
 *
 * Return: 0 when @attr is a threshold, -EINVAL when it is not.
 */
int smi_hwmon_thr_slot(enum smi_sensor_attribute attr, enum smi_thr *slot)
{
	const int bit = smi_hwmon_attr_info(attr).bit;

	if ((bit < 0) || (bit >= (int)SMI_THR_CNT))
		return -EINVAL;

	*slot = (enum smi_thr)bit;
	return 0;
}

/**
 * smi_hwmon_attr_value() - Read one attribute out of a sensor record.
 * @rec: The record, already copied out of the store.
 * @attr: Attribute to read.
 * @value: Output.
 *
 * A pure read of the caller's copy, so every attribute of one hwmon read comes
 * from the same observation -- the copy is taken once, above.
 *
 * Return: 0 on success or negative error code.
 */
int smi_hwmon_attr_value(const struct smi_sensor *rec,
			 enum smi_sensor_attribute attr, s64 *value)
{
	enum smi_thr slot;

	if (!rec || !value)
		return -EINVAL;

	switch (attr) {
	case SENSOR_ATTR_INSTANT:
		*value = rec->value;
		return 0;

	/* A card that keeps no running pair says so in the record's mask, and
	 * what it parked in the cell is not a reading to report. */
	case SENSOR_ATTR_AVERAGE:
		if (!(rec->mask & BIT(SmiThr_Avg)))
			return -ENODATA;
		*value = rec->avg;
		return 0;

	case SENSOR_ATTR_MAX:
		if (!(rec->mask & BIT(SmiThr_Max)))
			return -ENODATA;
		*value = rec->max;
		return 0;

	case SENSOR_ATTR_STATUS:
		*value = (s64)rec->status;
		return 0;

	default:
		break;
	}

	/* An alarm returns its limit; comparing it is the caller's. */
	if (smi_hwmon_thr_slot(attr, &slot))
		return -EINVAL;

	if (!(rec->mask & BIT(slot)))
		return -EINVAL;

	*value = rec->thr[slot];
	return 0;
}

/**
 * smi_hwmon_scale() - Put a reading in the unit hwmon documents for its type.
 * @type: Hwmon sensor type.
 * @unit_exp: The record's own exponent.
 * @val: The raw reading.
 * @out: The scaled reading.
 *
 * Millivolt, millidegree Celsius and milliampere; microwatt for power.
 *
 * The one place a reading narrows to the word hwmon reads it in.
 *
 * Return: 0, -EINVAL for a type with no hwmon unit, -E2BIG where the scaling
 *         itself overflows, or -ERANGE where the scaled reading does not fit.
 */
int smi_hwmon_scale(enum hwmon_sensor_types type, int8_t unit_exp, s64 val,
		    long *out)
{
	s64 scaled = 0;
	int to, ret;

	/* hwmon's own unit for the type, as its documentation fixes it. */
	switch (type) {
	case hwmon_temp:
	case hwmon_curr:
	case hwmon_in:
		to = -3;
		break;

	case hwmon_power:
		to = -6;
		break;

	default:
		return -EINVAL;
	}

	if (!out)
		return -EINVAL;

	/* The reading's exponent IS the power of ten, so the same arithmetic
	 * the sensor API uses serves here: an exponent no table happens to name
	 * still scales, and one that would not fit is an error, not a value. */
	ret = smi_scale_pow10(val, unit_exp, to, &scaled);
	if (ret)
		return ret;

	if ((scaled > LONG_MAX) || (scaled < LONG_MIN))
		return -ERANGE;

	*out = (long)scaled;
	return 0;
}

int smi_hwmon_set_interval(struct pf_dev_struct *pf_dev, long ms)
{
	if (!pf_dev)
		return -EINVAL;
	if ((ms < 0) || (ms > U16_MAX))
		return -EINVAL;

	WRITE_ONCE(pf_dev->sensor_refresh, (uint16_t)ms);
	return 0;
}
