/* SPDX-License-Identifier: GPL-2.0 */
/*
 * smi_alarm.h - Which readings raise an alarm, and when.
 *
 * Host policy over the sensor store: which kind of temperature each alarm
 * watches, the thresholds the card stated for it, the hysteresis that keeps a
 * reading on the boundary from flapping, and the rate limit that keeps a
 * failing card from filling the log. The card is asked nothing here.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_ALARM_H
#define SMI_ALARM_H

#include <linux/types.h>

#include "smi_api.h"

struct device;
struct pf_dev_struct;
struct smi_alarm;

struct smi_alarm_info {
	enum smi_alarm_type type;
	int sensor_id;
	/* The exponent the watched record states. Thresholds and readings are
	 * compared in it; the event publishes in degrees. */
	int8_t unit_exp;
	s64 thresholds[SMI_ALARM_SEVERITY_MAX];
	s64 default_thresholds[SMI_ALARM_SEVERITY_MAX];
	bool supported[SMI_ALARM_SEVERITY_MAX];
	bool is_raised[SMI_ALARM_SEVERITY_MAX];
	unsigned long last_alarm_time[SMI_ALARM_SEVERITY_MAX];

	bool enabled;
};

/**
 * smi_alarm_init() - Read the card's thresholds and arm what it can.
 *
 * Runs once discovery has filled the store: what each alarm watches is a
 * sensor kind, and the record carrying that kind is what it then polls. A
 * kind the card does not carry leaves that monitor disabled, which is not
 * an error.
 *
 * Return: 0, -EINVAL or -ENOMEM.
 */
int smi_alarm_init(struct pf_dev_struct *pf_dev);

/**
 * smi_alarm_evaluate() - One sweep: refresh the thermals and judge them.
 *
 * Called on each poll tick. Repeated sensor failures suspend the sweep until
 * a rate-limit interval has passed, so a silent card costs one try a minute
 * rather than one a tick.
 */
void smi_alarm_evaluate(struct pf_dev_struct *pf_dev);

#endif /* SMI_ALARM_H */
