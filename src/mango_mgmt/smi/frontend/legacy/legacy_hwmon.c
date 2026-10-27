// SPDX-License-Identifier: GPL-2.0-only
/*
 * legacy_hwmon.c - The legacy hwmon surface, frozen.
 *
 * The chip name, the attribute set and each attribute's meaning are what
 * released tools parse, so they stay as they shipped -- the ladder runs
 * downward, _lcrit is the critical step, and vendor _status and tempN_average
 * have no standard spelling.
 *
 * Copyright (c) 2023 - 2025 Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include <linux/device.h> /* struct device */
#include <linux/hwmon-sysfs.h>
#include <linux/hwmon.h>
#include <linux/sysfs.h> /* sysfs_emit */
#include <linux/types.h> /* umode_t */

#include "contract/smi/smi_abi.h" /* SmiRepo_* */
#include "core/smi_sensor.h"

#include "legacy.h"
#include "legacy_hwmon.h"
#include "smi_frontend.h"
#include "smi_hwmon.h"
#include "smi_top.h"

/* What lm-sensors and every monitoring script call this chip. Frozen. */
#define SMI_HWMON_NAME "mango"

/*
 * The channels are built at bind from the sensor model's records, and
 * mango_hwmon_is_visible answers per channel what its record carries. A card
 * numbers sensors within a repository, so sensor id N is channel N - 1, which
 * hwmon names <type>N.
 */
/* "power255_status" and a terminator is the longest a generated name gets. */
#define SMI_HWMON_ATTR_NAME_MAX (24)

#define READ_ONLY (0444)
#define READ_WRITE (0644)

/**
 * enum smi_hwmon_repo - Which repository an hwmon type reads.
 * @SMI_HWMON_REPO_INVALID: no repository serves this type.
 * @SMI_HWMON_REPO_TEMP: temperatures.
 * @SMI_HWMON_REPO_CURRENT: currents.
 * @SMI_HWMON_REPO_VOLTAGE: voltages.
 * @SMI_HWMON_REPO_POWER: the aggregate power record.
 */
enum smi_hwmon_repo {
	SMI_HWMON_REPO_INVALID = -1,
	SMI_HWMON_REPO_TEMP = SmiRepo_Temp,
	SMI_HWMON_REPO_CURRENT = SmiRepo_Current,
	SMI_HWMON_REPO_VOLTAGE = SmiRepo_Voltage,
	/* hwmon's power channels serve the aggregate repository. */
	SMI_HWMON_REPO_POWER = SmiRepo_TotalPower,
};

const char *smi_legacy_status_name(int status)
{
	switch (status) {
	case SmiSensorStatus_NotPresent:
		return "Sensor Not Present";
	case SmiSensorStatus_Valid:
		return "Sensor Present and Valid";
	case SmiSensorStatus_NoData:
		return "Data Not Available";
	case SMI_SENSOR_STATUS_LEGACY_NA:
		return "Not Applicable or Default Value";
	default:
		return "";
	}
}

/**
 * smi_hwmon_sensor_type() - The repository an hwmon type reads.
 * @type: The hwmon type.
 *
 * Return: The repository, or SMI_HWMON_REPO_INVALID.
 */
static enum smi_hwmon_repo smi_hwmon_sensor_type(enum hwmon_sensor_types type)
{
	enum smi_hwmon_repo ret = SMI_HWMON_REPO_INVALID;

	switch (type) {
	case hwmon_temp:
		ret = SMI_HWMON_REPO_TEMP;
		break;
	case hwmon_in:
		ret = SMI_HWMON_REPO_VOLTAGE;
		break;
	case hwmon_curr:
		ret = SMI_HWMON_REPO_CURRENT;
		break;
	case hwmon_power:
		ret = SMI_HWMON_REPO_POWER;
		break;
	default:
		break;
	}

	return ret;
}

/**
 * mango_sensor_visible() - Check if a sensor attribute exists for a given PCI device.
 * @pf_dev: Pointer to PCI device data.
 * @sid: Sensor ID (index/channel).
 * @type: Sensor type.
 * @attr: Sensor attribute.
 *
 * Return: The mode of the hwmon file if the attribute exists or 0.
 */
static umode_t mango_sensor_visible(struct pf_dev_struct *pf_dev, int sid,
				    enum smi_hwmon_repo type,
				    enum smi_sensor_attribute attr);

/* hwmon callbacks */
static umode_t mango_hwmon_is_visible(const void *data,
				      enum hwmon_sensor_types type, u32 attr,
				      int channel);
static int mango_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int channel, long *val);
static int mango_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long val);

static int mango_hwmon_read_string(struct device *dev,
				   enum hwmon_sensor_types type, u32 attr,
				   int channel, const char **str);

/* The readers for files hwmon has no attribute for. Named by the type table
 * below, defined with the other sysfs callbacks. */
static ssize_t sensor_status_show(struct device *dev,
				  struct device_attribute *da, char *buf);
static ssize_t temp_average_show(struct device *dev,
				 struct device_attribute *da, char *buf);

static const struct hwmon_ops mango_hwmon_ops = {
	.is_visible = mango_hwmon_is_visible,
	.read = mango_hwmon_read,
	.read_string = mango_hwmon_read_string,
	.write = mango_hwmon_write,
};

/* The chip's own channel, which no sensor backs. */
static const u32 mango_chip_config[] = { HWMON_C_UPDATE_INTERVAL, 0 };

static const struct hwmon_channel_info mango_chip = {
	.type = hwmon_chip,
	.config = mango_chip_config,
};

/**
 * struct v1_attr_row - One file this surface spells for a type.
 * @attr: the hwmon attribute the core asks by. Its config bit is BIT(@attr),
 *	  so the row cannot claim a file it then fails to serve.
 * @smi: the neutral meaning behind it; INVALID for a label, which
 *	 read_string() answers from the name copied at bind.
 */
struct v1_attr_row {
	u32 attr;
	enum smi_sensor_attribute smi;
};

/**
 * struct v1_gen_attr - One file hwmon has no attribute for.
 * @fmt: sysfs name, taking the channel number. NULL where the type has none.
 * @show: the reader that answers it.
 */
struct v1_gen_attr {
	const char *fmt;
	ssize_t (*show)(struct device *, struct device_attribute *, char *);
};

/* The frozen spelling: the ladder reads downward -- max is the warning step,
 * lcrit the critical one, crit the fatal one -- and an alarm rides beside each
 * limit. Released tools parse these, so the rows do not change. */
static const struct v1_attr_row v1_rows_temp[] = {
	{ hwmon_temp_input, SENSOR_ATTR_INSTANT },
	{ hwmon_temp_label, SENSOR_ATTR_INVALID },
	{ hwmon_temp_highest, SENSOR_ATTR_MAX },
	{ hwmon_temp_max, SENSOR_ATTR_WARN },
	{ hwmon_temp_max_alarm, SENSOR_ATTR_WARN_A },
	{ hwmon_temp_lcrit, SENSOR_ATTR_CRIT },
	{ hwmon_temp_lcrit_alarm, SENSOR_ATTR_CRIT_A },
	{ hwmon_temp_crit, SENSOR_ATTR_FATAL },
	{ hwmon_temp_crit_alarm, SENSOR_ATTR_FATAL_A },
};

static const struct v1_attr_row v1_rows_in[] = {
	{ hwmon_in_input, SENSOR_ATTR_INSTANT },
	{ hwmon_in_label, SENSOR_ATTR_INVALID },
	{ hwmon_in_highest, SENSOR_ATTR_MAX },
	{ hwmon_in_average, SENSOR_ATTR_AVERAGE },
	{ hwmon_in_max, SENSOR_ATTR_WARN },
	{ hwmon_in_max_alarm, SENSOR_ATTR_WARN_A },
	{ hwmon_in_lcrit, SENSOR_ATTR_CRIT },
	{ hwmon_in_lcrit_alarm, SENSOR_ATTR_CRIT_A },
	{ hwmon_in_crit, SENSOR_ATTR_FATAL },
	{ hwmon_in_crit_alarm, SENSOR_ATTR_FATAL_A },
};

static const struct v1_attr_row v1_rows_curr[] = {
	{ hwmon_curr_input, SENSOR_ATTR_INSTANT },
	{ hwmon_curr_label, SENSOR_ATTR_INVALID },
	{ hwmon_curr_highest, SENSOR_ATTR_MAX },
	{ hwmon_curr_average, SENSOR_ATTR_AVERAGE },
	{ hwmon_curr_max, SENSOR_ATTR_WARN },
	{ hwmon_curr_max_alarm, SENSOR_ATTR_WARN_A },
	{ hwmon_curr_lcrit, SENSOR_ATTR_CRIT },
	{ hwmon_curr_lcrit_alarm, SENSOR_ATTR_CRIT_A },
	{ hwmon_curr_crit, SENSOR_ATTR_FATAL },
	{ hwmon_curr_crit_alarm, SENSOR_ATTR_FATAL_A },
};

static const struct v1_attr_row v1_rows_power[] = {
	{ hwmon_power_input, SENSOR_ATTR_INSTANT },
	{ hwmon_power_label, SENSOR_ATTR_INVALID },
	{ hwmon_power_input_highest, SENSOR_ATTR_MAX },
	{ hwmon_power_average, SENSOR_ATTR_AVERAGE },
	{ hwmon_power_max, SENSOR_ATTR_WARN },
	{ hwmon_power_max_alarm, SENSOR_ATTR_WARN_A },
	{ hwmon_power_lcrit, SENSOR_ATTR_CRIT },
	{ hwmon_power_lcrit_alarm, SENSOR_ATTR_CRIT_A },
	{ hwmon_power_crit, SENSOR_ATTR_FATAL },
	{ hwmon_power_crit_alarm, SENSOR_ATTR_FATAL_A },
};

/**
 * struct mango_hwmon_type - One sensor type, as hwmon sees it.
 * @smi: The repository this type's channels are served from; the rid.
 * @hwmon: The hwmon type.
 * @rows: The files a channel of this type carries, and what each means.
 * @nrow: How many.
 * @base: the number hwmon gives this type's first channel -- 0 for voltages,
 *	  1 for the rest. A generated name must count the same way, or it
 *	  would describe a different channel than the one beside it.
 * @gen: the files hwmon cannot spell, under the names this type gives them.
 *	 Its size is how many a channel can generate; both the emitter and the
 *	 caller that sizes its array read that from here.
 */
struct mango_hwmon_type {
	enum smi_hwmon_repo smi;
	enum hwmon_sensor_types hwmon;
	const struct v1_attr_row *rows;
	size_t nrow;
	u8 base;
	struct v1_gen_attr gen[2];
};

static const struct mango_hwmon_type mango_hwmon_types[] = {
	{ SMI_HWMON_REPO_TEMP,
	  hwmon_temp,
	  v1_rows_temp,
	  ARRAY_SIZE(v1_rows_temp),
	  1,
	  { { "temp%u_status", sensor_status_show },
	    { "temp%u_average", temp_average_show } } },
	{ SMI_HWMON_REPO_VOLTAGE,
	  hwmon_in,
	  v1_rows_in,
	  ARRAY_SIZE(v1_rows_in),
	  0,
	  { { "in%u_status", sensor_status_show } } },
	{ SMI_HWMON_REPO_CURRENT,
	  hwmon_curr,
	  v1_rows_curr,
	  ARRAY_SIZE(v1_rows_curr),
	  1,
	  { { "curr%u_status", sensor_status_show } } },
	{ SMI_HWMON_REPO_POWER,
	  hwmon_power,
	  v1_rows_power,
	  ARRAY_SIZE(v1_rows_power),
	  1,
	  { { "power%u_status", sensor_status_show } } },
};

/* What outlives one call: hwmon keeps the label pointer it is handed, so the
 * names are copied at bind beside the census that fixed the channel count. */
struct mango_hwmon_priv {
	/* The device this surface speaks for, so a callback needs no
	 * binding record -- see the registration below. */
	struct pf_dev_struct *pf_dev;
	const char **label[ARRAY_SIZE(mango_hwmon_types)];
	u8 max_id[ARRAY_SIZE(mango_hwmon_types)];
};

/* What hwmon hands a callback is the device it registered, and its drvdata is
 * this surface's own record -- the SMI device is one hop further on. */
static struct pf_dev_struct *hwmon_pf_dev(struct device *dev)
{
	const struct mango_hwmon_priv *priv = dev ? dev_get_drvdata(dev) : NULL;

	return priv ? priv->pf_dev : NULL;
}

/* Which row of the type table an hwmon type is, or -1. */
static int hwmon_type_index(enum hwmon_sensor_types type)
{
	size_t t;

	for (t = 0; t < ARRAY_SIZE(mango_hwmon_types); t++)
		if (mango_hwmon_types[t].hwmon == type)
			return (int)t;

	return -1;
}

/* Every attribute a channel of this type carries: one config bit per row,
 * and a bit is BIT of the attribute the row names. */
static u32 v1_config_of(const struct mango_hwmon_type *row)
{
	u32 cfg = 0;
	size_t i;

	for (i = 0; i < row->nrow; i++)
		cfg |= BIT(row->rows[i].attr);

	return cfg;
}

/* What the core is asking of the record, or INVALID for a file this type does
 * not spell. The same rows decide the file exists, so the two cannot part. */
static enum smi_sensor_attribute v1_attr_of(enum hwmon_sensor_types type,
					    u32 attr)
{
	const int t = hwmon_type_index(type);
	size_t i;

	if (t < 0)
		return SENSOR_ATTR_INVALID;

	for (i = 0; i < mango_hwmon_types[t].nrow; i++)
		if (mango_hwmon_types[t].rows[i].attr == attr)
			return mango_hwmon_types[t].rows[i].smi;

	return SENSOR_ATTR_INVALID;
}

static umode_t mango_sensor_visible(struct pf_dev_struct *pf_dev, int sid,
				    enum smi_hwmon_repo type,
				    enum smi_sensor_attribute attr)
{
	struct smi_sensor rec = { 0 };
	enum smi_thr slot;

	/* Sensor doesn't exist. */
	if (read_sensor_record(pf_dev, (uint8_t)type, sid, &rec))
		return 0;

	/* Only the thresholds can be absent; everything else is always there. */
	if (smi_hwmon_thr_slot(attr, &slot))
		return READ_ONLY;

	return (rec.mask & BIT(slot)) ? READ_ONLY : 0;
}

/* @data is what registration handed hwmon: this surface's own record. */
static umode_t mango_hwmon_is_visible(const void *data,
				      enum hwmon_sensor_types type, u32 attr,
				      int channel)
{
	const struct mango_hwmon_priv *priv = data;

	if (!priv)
		return 0;

	/* Chip config is special */
	if (type == hwmon_chip) {
		switch (attr) {
		case hwmon_chip_update_interval:
			return READ_WRITE;

		default:
			return 0;
		}
	}

	return mango_sensor_visible(priv->pf_dev, channel,
				    smi_hwmon_sensor_type(type),
				    v1_attr_of(type, attr));
}

static int mango_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long val)
{
	int ret = 0;
	struct pf_dev_struct *pf_dev = NULL;

	if (!dev)
		return -EINVAL;

	pf_dev = get_pf_dev_entry(dev, PF_DEV_CACHE_DEV);

	if (!pf_dev)
		return -ENODEV;

	if (type == hwmon_chip) {
		switch (attr) {
		case hwmon_chip_update_interval:
			/* Negative has meant zero on this surface since it
			 * shipped, so it is clamped here rather than refused. */
			ret = smi_hwmon_set_interval(pf_dev,
						     (val < 0) ? 0 : val);
			break;

		default:
			ret = -EINVAL;
			break;
		}
	}

	put_pf_dev_entry(pf_dev);
	return ret;
}

/**
 * smi_hwmon_read_val() - Read one attribute of one record, in hwmon's units.
 * @pf_dev: PCI device data structure.
 * @type: Hwmon sensor type -- what unit to report in, and what to refresh.
 * @rid: The repository the record lives in.
 * @sid: Its ordinal there (record id - 1).
 * @smi_attr: What is wanted of the record -- the meaning, not a file name.
 * @val: Variable to store sensor value.
 * @status: The record's status word (optional).
 * @fresh: Variable to store cache status for this value (optional).
 *
 * Return: 0 or negative error code.
 */
static int smi_hwmon_read_val(struct pf_dev_struct *pf_dev,
			      enum hwmon_sensor_types type, uint8_t rid,
			      int sid, enum smi_sensor_attribute smi_attr,
			      long *val, const char **status, bool *fresh)
{
	int ret = 0;
	s64 value = 0;
	long mapped_value = 0;
	struct smi_sensor rec = { 0 };

	if (!pf_dev || !val)
		return -EINVAL;

	/* The host's own word, which no record backs and no state gates. */
	if (type == hwmon_chip) {
		*val = READ_ONCE(pf_dev->sensor_refresh);
		return 0;
	}

	/* Everything below refreshes over the SMC mailbox, so it takes the same
	 * state policy the legacy sensor ioctl applies. */
	if (!smi_pf_dev_ready(pf_dev))
		return -ENODEV;

	/* A reading is refreshed and copied under one lock, so every attribute
	 * below comes from one observation. A limit is the description the card
	 * stated once -- no refresh moves it -- so it is copied. The repository
	 * is the record's own, not the one this type usually serves. */
	ret = smi_hwmon_attr_info(smi_attr).live
		      ? read_sensor_fresh(pf_dev, rid, sid, &rec, fresh)
		      : read_sensor_record(pf_dev, rid, sid, &rec);
	if (ret)
		return ret;

	switch (smi_attr) {
	/* An alarm is the comparison the frozen surface publishes, so it needs
	 * the limit and the reading out of the one record copied above. */
	case SENSOR_ATTR_WARN_A:
	case SENSOR_ATTR_CRIT_A:
	case SENSOR_ATTR_FATAL_A: {
		s64 limit = 0;

		ret = smi_hwmon_attr_value(&rec, smi_attr, &limit);
		if (ret)
			return ret;

		ret = smi_hwmon_attr_value(&rec, SENSOR_ATTR_INSTANT, &value);
		if (ret)
			return ret;

		*val = (value >= limit);
	} break;

	default: {
		ret = smi_hwmon_attr_value(&rec, smi_attr, &value);
		if (ret)
			return ret;

		/* A status is the card's own byte, not a measurement: scaling it
		 * would turn "present and valid" into a thousand of them. */
		if (smi_attr == SENSOR_ATTR_STATUS) {
			*val = value;
		} else {
			ret = smi_hwmon_scale(type, rec.unit_exp, value,
					      &mapped_value);
			if (!ret)
				*val = mapped_value;
		}

		/* Status, when the caller asked for it: the word, which the
		 * caller spells. */
		if (status) {
			s64 s = 0;

			smi_hwmon_attr_value(&rec, SENSOR_ATTR_STATUS, &s);
			*status = smi_legacy_status_name((int)s);
		}
	} break;
	}

	return ret;
}

int read_sensor_val(struct pf_dev_struct *pf_dev, enum hwmon_sensor_types type,
		    enum smi_sensor_attribute smi_attr, int channel, long *val,
		    const char **status, bool *fresh)
{
	uint8_t rid = 0, id = 0;
	int ret;

	if (!pf_dev)
		return -EINVAL;

	/* The chip's own channel is backed by no record, so it names none. */
	if (type == hwmon_chip)
		return smi_hwmon_read_val(pf_dev, type, rid, channel, smi_attr,
					  val, status, fresh);

	/* The same gate the read below applies, ahead of the numbering: a card
	 * that cannot answer at all says so itself. */
	if (!smi_pf_dev_ready(pf_dev))
		return -ENODEV;

	/* A channel is the bound surface's word: the ioctl exists to pair a
	 * value with its status, and it must name the record hwmon names. */
	ret = smi_frontend_chan_resolve(pf_dev, type, channel, &rid, &id);
	if (ret)
		return ret;

	return smi_hwmon_read_val(pf_dev, type, rid, id - 1, smi_attr, val,
				  status, fresh);
}

/**
 * mango_hwmon_read() - Hwmon read callback.
 * @dev: The character device data.
 * @type: The requested sensor type.
 * @attr: The requested sensor attribute.
 * @channel: The requested sensor ID.
 * @val: Void pointer to the output variable.
 *
 * Return: 0 on success or negative error code.
 */
static int mango_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int channel, long *val)
{
	int ret = 0;
	struct pf_dev_struct *pf_dev = NULL;

	if (!dev || !val)
		return -EINVAL;

	/*
	 * If this callback is triggered, we know that the device supports
	 * the sensor as this is already verified by the `mango_hwmon_is_visible`
	 * function.
	 */
	pf_dev = get_pf_dev_entry(dev, PF_DEV_CACHE_DEV);

	if (!pf_dev)
		return -ENODEV;

	ret = read_sensor_val(pf_dev, type, v1_attr_of(type, attr), channel,
			      val, NULL, NULL);

	put_pf_dev_entry(pf_dev);
	return ret;
}

/* Non-standard attributes - every sensor has an additional label and status */

/* sysfs callback - same as `mango_hwmon_is_visible` but for nonstandard attributes. */
static umode_t is_visible_extra(struct kobject *kobj, struct attribute *attr,
				int n)
{
	struct device *dev = NULL; /* This is the hwmon device */
	struct pf_dev_struct *pf_dev = NULL;
	struct device_attribute *da = NULL;
	struct sensor_device_attribute_2 *sensor_da = NULL;
	enum smi_hwmon_repo sensor_type = SMI_HWMON_REPO_INVALID;

	if (!kobj || !attr)
		return 0;

	dev = container_of(kobj, struct device, kobj);
	pf_dev = hwmon_pf_dev(dev);
	da = container_of(attr, struct device_attribute, attr);
	sensor_da = to_sensor_dev_attr_2(da);
	sensor_type = (enum smi_hwmon_repo)sensor_da->nr;

	/*
	 * Setting attribute to invalid; assume that all extra attributes
	 * are supported by all sensors.
	 */
	return mango_sensor_visible(pf_dev, sensor_da->index, sensor_type,
				    SENSOR_ATTR_INVALID);
}

static ssize_t temp_average_show(struct device *dev,
				 struct device_attribute *da, char *buf)
{
	int ret = 0;
	struct sensor_device_attribute_2 *sensor_da = NULL;
	struct smi_sensor rec = { 0 };
	s64 value = 0;
	long mapped_value = 0;

	if (!dev || !da || !buf)
		return 0;

	sensor_da = to_sensor_dev_attr_2(da);

	if (read_sensor_record(hwmon_pf_dev(dev), (uint8_t)sensor_da->nr,
			       sensor_da->index, &rec))
		return 0;

	ret = smi_hwmon_attr_value(&rec, SENSOR_ATTR_AVERAGE, &value);
	if (ret)
		return 0;

	/* This generation reports a temperature in millidegrees. */
	if (!smi_hwmon_scale(hwmon_temp, rec.unit_exp, value, &mapped_value))
		return sysfs_emit(buf, "%ld\n", mapped_value);

	return 0;
}

static ssize_t sensor_status_show(struct device *dev,
				  struct device_attribute *da, char *buf)
{
	struct sensor_device_attribute_2 *sensor_da = NULL;
	struct smi_sensor rec = { 0 };
	s64 status = 0;

	if (!dev || !da || !buf)
		return 0;

	sensor_da = to_sensor_dev_attr_2(da);

	if (!read_sensor_record(hwmon_pf_dev(dev), (uint8_t)sensor_da->nr,
				sensor_da->index, &rec))
		smi_hwmon_attr_value(&rec, SENSOR_ATTR_STATUS, &status);

	return sysfs_emit(buf, "%s\n", smi_legacy_status_name((int)status));
}

/*
 * Initialize hwmon.
 */
/* A channel's label: the record's name, as it read at bind. */
static int mango_hwmon_read_string(struct device *dev,
				   enum hwmon_sensor_types type, u32 attr,
				   int channel, const char **str)
{
	const struct mango_hwmon_priv *priv = dev ? dev_get_drvdata(dev) : NULL;
	int t = hwmon_type_index(type);

	if (!priv || (t < 0) || (channel < 0) || (channel >= priv->max_id[t]))
		return -EOPNOTSUPP;
	if (!priv->label[t][channel])
		return -ENODATA;

	*str = priv->label[t][channel];
	return 0;
}

/**
 * DOC: hwmon construction
 *
 * How many sensors of a type a card has is the card's to say, so the channel
 * tables are built at bind from the records the model already holds -- one
 * channel per ordinal up to the highest the repository carries. hwmon's core
 * stops at the first zero config word, so every slot below the highest carries
 * the full mask and is_visible answers, per channel, what the record behind it
 * actually has. A channel keeps its ordinal: tempN is still sensor N.
 *
 * Two attributes have no hwmon spelling -- a temperature average, and a
 * status of any type -- so those are generated here under the same names.
 */

/* The highest ordinal each named repository carries; a repository this card
 * lacks leaves its slot 0. How many channels of a type a card has is the
 * card's to say, so the tables below size themselves from this. */
static void v1_census(struct pf_dev_struct *pf_dev, const uint8_t *rids,
		      u8 *max_id, size_t n)
{
	struct smi_sensor rec;
	size_t t;
	int r;

	for (t = 0; t < n; t++) {
		r = 0;
		while (!read_sensor_record_at(pf_dev, rids[t], r++, &rec))
			max_id[t] = max(max_id[t], rec.id);
	}
}

/* One type's channel info, sized to its highest ordinal. */
static struct hwmon_channel_info *
hwmon_type_info(struct device *dev, const struct mango_hwmon_type *row,
		u8 max_id)
{
	struct hwmon_channel_info *info;
	u32 *config;
	u8 i;

	config = devm_kcalloc(dev, (size_t)max_id + 1, sizeof(*config),
			      GFP_KERNEL);
	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!config || !info)
		return NULL;

	for (i = 0; i < max_id; i++)
		config[i] = v1_config_of(row);

	info->type = row->hwmon;
	info->config = config;
	return info;
}

/* One type's channel labels, copied out of the records at bind. */
static const char **hwmon_type_labels(struct device *dev,
				      struct pf_dev_struct *pf_dev,
				      const struct mango_hwmon_type *row,
				      u8 max_id)
{
	const char **out = devm_kcalloc(dev, max_id, sizeof(*out), GFP_KERNEL);
	char *name;
	u8 i;

	if (!out)
		return NULL;

	for (i = 0; i < max_id; i++) {
		struct smi_sensor rec = { 0 };

		if (read_sensor_record(pf_dev, (uint8_t)row->smi, i, &rec))
			continue; /* no record at this ordinal */
		name = devm_kzalloc(dev, sizeof(rec.name), GFP_KERNEL);
		if (!name)
			return NULL;
		strscpy(name, rec.name, sizeof(rec.name));
		out[i] = name;
	}

	return out;
}

/* Every generated attribute of one type: one per ordinal, per name it has. The
 * name and the reader that answers it are one row, not two indexes. */
static int hwmon_type_extras(struct device *dev,
			     const struct mango_hwmon_type *row, u8 max_id,
			     struct attribute **ptrs, unsigned int *at)
{
	struct sensor_device_attribute_2 *attrs;
	unsigned int n = 0, k;
	u8 ch;

	attrs = devm_kcalloc(dev, (size_t)max_id * ARRAY_SIZE(row->gen),
			     sizeof(*attrs), GFP_KERNEL);
	if (!attrs)
		return -ENOMEM;

	for (k = 0; k < ARRAY_SIZE(row->gen); k++) {
		if (!row->gen[k].fmt)
			continue; /* hwmon has an attribute for this one */

		for (ch = 0; ch < max_id; ch++) {
			struct sensor_device_attribute_2 *a = &attrs[n++];
			char *name = devm_kzalloc(dev, SMI_HWMON_ATTR_NAME_MAX,
						  GFP_KERNEL);

			if (!name)
				return -ENOMEM;
			snprintf(name, SMI_HWMON_ATTR_NAME_MAX, row->gen[k].fmt,
				 ch + row->base);

			sysfs_attr_init(&a->dev_attr.attr);
			a->dev_attr.attr.name = name;
			a->dev_attr.attr.mode = READ_ONLY;
			a->dev_attr.show = row->gen[k].show;
			a->nr = (u8)row->smi;
			/* The channel, which is what a show reads a record by. */
			a->index = (u16)ch;
			ptrs[(*at)++] = &a->dev_attr.attr;
		}
	}

	return 0;
}

/**
 * hwmon_build() - The chip info and the extra group this card needs.
 * @dev: Parent device, which owns every allocation here.
 * @pf_dev: Pointer to top level PCI data struct.
 * @chip: Set to the chip info to register with.
 * @groups: Set to the extra attribute groups, NULL terminated.
 * @priv_out: Set to what the callbacks read; published only once bound.
 *
 * Return: 0 or -ENOMEM.
 */
static int hwmon_build(struct device *dev, struct pf_dev_struct *pf_dev,
		       const struct hwmon_chip_info **chip,
		       const struct attribute_group ***groups,
		       struct mango_hwmon_priv **priv_out)
{
	const struct hwmon_channel_info **info;
	struct attribute **ptrs = NULL;
	struct attribute_group *group;
	const struct attribute_group **list;
	struct mango_hwmon_priv *priv;
	struct hwmon_chip_info *ci;
	u8 *max_id;
	unsigned int total = 0, at = 0;
	size_t t, n = 1;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->pf_dev = pf_dev;
	max_id = priv->max_id;
	{
		uint8_t rids[ARRAY_SIZE(mango_hwmon_types)];
		size_t i;

		for (i = 0; i < ARRAY_SIZE(rids); i++)
			rids[i] = (uint8_t)mango_hwmon_types[i].smi;
		v1_census(pf_dev, rids, max_id, ARRAY_SIZE(rids));
	}

	for (t = 0; t < ARRAY_SIZE(mango_hwmon_types); t++)
		if (max_id[t])
			n++;

	/* The chip channel, one per populated type, and the terminator. */
	info = devm_kcalloc(dev, n + 1, sizeof(*info), GFP_KERNEL);
	ci = devm_kzalloc(dev, sizeof(*ci), GFP_KERNEL);
	group = devm_kzalloc(dev, sizeof(*group), GFP_KERNEL);
	list = devm_kcalloc(dev, 2, sizeof(*list), GFP_KERNEL);
	if (!info || !ci || !group || !list)
		return -ENOMEM;

	n = 0;
	info[n++] = &mango_chip;
	for (t = 0; t < ARRAY_SIZE(mango_hwmon_types); t++) {
		if (!max_id[t])
			continue;
		info[n] =
			hwmon_type_info(dev, &mango_hwmon_types[t], max_id[t]);
		priv->label[t] = hwmon_type_labels(
			dev, pf_dev, &mango_hwmon_types[t], max_id[t]);
		if (!info[n] || !priv->label[t])
			return -ENOMEM;
		n++;
	}

	/* The most every type can generate, from the same list the emitter
	 * walks: a type that names fewer leaves the tail NULL, and the array
	 * ends at @at. */
	for (t = 0; t < ARRAY_SIZE(mango_hwmon_types); t++)
		total += (unsigned int)max_id[t] *
			 ARRAY_SIZE(mango_hwmon_types[t].gen);

	ptrs = devm_kcalloc(dev, (size_t)total + 1, sizeof(*ptrs), GFP_KERNEL);
	if (!ptrs)
		return -ENOMEM;

	for (t = 0; t < ARRAY_SIZE(mango_hwmon_types); t++)
		if (max_id[t] && hwmon_type_extras(dev, &mango_hwmon_types[t],
						   max_id[t], ptrs, &at))
			return -ENOMEM;

	group->attrs = ptrs;
	group->is_visible = is_visible_extra;
	list[0] = group;

	ci->ops = &mango_hwmon_ops;
	ci->info = info;
	*priv_out = priv;
	*chip = ci;
	*groups = list;
	return 0;
}

int smi_hwmon_v1_chan(struct pf_dev_struct *pf_dev,
		      enum hwmon_sensor_types type, int channel, uint8_t *rid,
		      uint8_t *id)
{
	const enum smi_hwmon_repo repo = smi_hwmon_sensor_type(type);

	/* This surface serves one repository per type and keeps a record's own
	 * ordinal as its channel, which is what its tools parse. */
	if ((repo == SMI_HWMON_REPO_INVALID) || (channel < 0) ||
	    (channel >= U8_MAX))
		return -ENOENT;

	*rid = (uint8_t)repo;
	*id = (uint8_t)(channel + 1);
	return 0;
}

int smi_hwmon_v1_register(struct device *dev, struct pf_dev_struct *pf_dev)
{
	const struct hwmon_chip_info *chip = NULL;
	const struct attribute_group **groups = NULL;
	struct mango_hwmon_priv *priv = NULL;
	struct device *hwmon_dev = NULL;
	int ret;

	if (!dev || !pf_dev)
		return -EINVAL;

	ret = hwmon_build(dev, pf_dev, &chip, &groups, &priv);
	if (ret)
		return ret;

	/* The record, not the SMI device: these files are visible from here,
	 * and the binding below them is written after. */
	hwmon_dev = devm_hwmon_device_register_with_info(dev, SMI_HWMON_NAME,
							 priv, chip, groups);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	return smi_frontend_bound(pf_dev, &smi_legacy_frontend_ops, hwmon_dev,
				  priv);
}
