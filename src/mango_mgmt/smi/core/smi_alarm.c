// SPDX-License-Identifier: GPL-2.0
/*
 * smi_alarm.c - Which readings raise an alarm, and when.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/notifier.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "backend/smi_smc_control.h"

#include "smi_alarm.h"
#include "smi_log.h"
#include "smi_sensor.h"
#include "smi_top.h"

#define HYSTERESIS_MARGIN_C (3) /* degrees Celsius */
#define ALARM_RATE_LIMIT_INTERVAL (60000) /* 60 seconds */
/* Consecutive sensor failures that suspend the sweep. */
#define ALARM_FAIL_SUSPEND (3)

/* Who is told, module-wide: a consumer registers for alarms, not for a card. */
static BLOCKING_NOTIFIER_HEAD(smi_alarm_chain);

/**
 * struct smi_alarm - One card's monitors and the state of the sweep over them.
 * @monitors: what each alarm watches, and whether it is currently raised.
 * @fail_count: consecutive sweeps the store could not answer.
 * @last_try: when the last sweep ran, for the suspended rate limit.
 *
 * Written once at init and then only by the sweep, which one poll tick runs at
 * a time.
 */
struct smi_alarm {
	struct smi_alarm_info monitors[SMI_ALARM_MAX];
	int fail_count;
	unsigned long last_try;
};

/* Which part's temperature raises which alarm. Host policy against what the
 * card says a sensor IS -- not where it happens to sit in the repository. */
static const uint8_t alarm_sensor_kind[SMI_ALARM_MAX] = {
	[SMI_ALARM_FPGA_OVER_TEMP] = SmiSensorKind_Fpga,
	[SMI_ALARM_BOARD_OVER_TEMP] = SmiSensorKind_Board,
	[SMI_ALARM_ETH_OVER_TEMP] = SmiSensorKind_Ethernet,
};

static void register_monitor(struct smi_alarm *a, struct pf_dev_struct *pf_dev,
			     enum smi_alarm_type type)
{
	static const struct {
		int sev;
		enum smi_thr slot;
	} map[] = {
		{ SMI_ALARM_SEVERITY_WARN, SmiThr_UpperWarn },
		{ SMI_ALARM_SEVERITY_CRIT, SmiThr_UpperCrit },
		{ SMI_ALARM_SEVERITY_FATAL, SmiThr_UpperFatal },
	};
	struct smi_sensor rec = { 0 };
	bool present = false;
	int i;
	bool any_supported = false;

	if (type >= SMI_ALARM_MAX || !pf_dev)
		return;

	/* Resolved once, here: what the alarm watches is a kind, and the id it
	 * then polls by is whichever record the card gave that kind. */
	present = !read_sensor_kind(pf_dev, SmiRepo_Temp,
				    alarm_sensor_kind[type], &rec);

	/* Set per-severity state (WARN, CRIT, FATAL) */
	for (i = 0; i < ARRAY_SIZE(map); i++) {
		int sev = map[i].sev;
		a->monitors[type].supported[sev] = false;
		a->monitors[type].is_raised[sev] = false;

		if (present && (rec.mask & BIT(map[i].slot))) {
			a->monitors[type].thresholds[sev] =
				rec.thr[map[i].slot];
			a->monitors[type].default_thresholds[sev] =
				a->monitors[type].thresholds[sev];
			a->monitors[type].supported[sev] = true;
			any_supported = true;
		}
	}

	if (any_supported) {
		a->monitors[type].type = type;
		a->monitors[type].sensor_id = rec.id - 1;
		a->monitors[type].unit_exp = rec.unit_exp;
		a->monitors[type].enabled = true;
	} else {
		a->monitors[type].enabled = false;
	}
}

/* The record's own unit, for a margin the API states in degrees. */
static s64 hysteresis_raw(const struct smi_alarm_info *alarm)
{
	s64 raw = HYSTERESIS_MARGIN_C;

	if (smi_scale_pow10(HYSTERESIS_MARGIN_C, 0, alarm->unit_exp, &raw))
		return HYSTERESIS_MARGIN_C;

	return raw;
}

/* One event, whichever way the threshold was crossed. The readings leave in
 * degrees: the record's exponent is the driver's business, not a consumer's. */
static void alarm_emit(struct pf_dev_struct *pf_dev,
		       const struct smi_alarm_info *alarm, int sev,
		       enum smi_alarm_state action, s64 val)
{
	struct smi_alarm_event e = {
		.type = alarm->type,
		.action = action,
		.severity = sev,
	};
	s64 deg = 0;

	/* A consumer acts on the address, so it names the bus this card is
	 * really on: SMI binds platform devices too. */
	if (dev_is_pci(pf_dev->dev)) {
		e.addr.bus_type = SMI_BUS_TYPE_PCI;
		e.addr.pci_dev = to_pci_dev(pf_dev->dev);
	} else {
		e.addr.bus_type = SMI_BUS_TYPE_PLT;
		e.addr.plt_dev = to_platform_device(pf_dev->dev);
	}

	if (!smi_scale_pow10(val, alarm->unit_exp, 0, &deg))
		e.current_value = (long)deg;
	if (!smi_scale_pow10(alarm->thresholds[sev], alarm->unit_exp, 0, &deg))
		e.threshold = (long)deg;

	blocking_notifier_call_chain(&smi_alarm_chain, e.type, &e);
}

static void check_and_trigger_alarm(struct pf_dev_struct *pf_dev,
				    struct smi_alarm_info *alarm)
{
	const s64 margin = hysteresis_raw(alarm);
	struct smi_sensor rec = { 0 };
	s64 val = 0;
	int sev;

	if (!alarm->enabled)
		return;

	if (read_sensor_record(pf_dev, SmiRepo_Temp, alarm->sensor_id, &rec))
		return;

	/* A reading the card will not vouch for is no evidence either way:
	 * judging on it raises an alarm from whatever the record last held,
	 * and the consumers of that alarm take a link down over it. */
	if (smi_status_denies_value(rec.status))
		return;

	val = rec.value;

	for (sev = SMI_ALARM_SEVERITY_MAX - 1; sev >= 0; sev--) {
		if (!alarm->supported[sev])
			continue;

		/* 1. Clear Check */
		if (val < (alarm->thresholds[sev] - margin)) {
			if (alarm->is_raised[sev]) {
				alarm_emit(pf_dev, alarm, sev,
					   SMI_ALARM_STATE_CLEARED, val);
				alarm->is_raised[sev] = false;
			}
			/* Continue to check lower severity alarms */
		}
		/* 2. Raise Check */
		else if (val > alarm->thresholds[sev]) {
			if (!alarm->is_raised[sev] ||
			    time_after(
				    jiffies,
				    alarm->last_alarm_time[sev] +
					    msecs_to_jiffies(
						    ALARM_RATE_LIMIT_INTERVAL))) {
				alarm_emit(pf_dev, alarm, sev,
					   SMI_ALARM_STATE_RAISED, val);
				alarm->is_raised[sev] = true;
				alarm->last_alarm_time[sev] = jiffies;
			}
			/* Stop checking lower severity alarms (Masking) */
			break;
		}
		/* 3. Holding (Deadband) Check */
		else if (alarm->is_raised[sev]) {
			/* Value is in the hysteresis band (Threshold -
			 * Margin <= Val <= Threshold): the upper alarm is
			 * still active, so mask lower alarms. */
			break;
		}
	}
}

static int alarm_sweep(struct pf_dev_struct *pf_dev, struct smi_alarm *a)
{
	int i = 0;
	int ret = 0;

	if (!pf_dev)
		return -EINVAL;

	/* Refresh thermal sensors */
	ret = read_thermal_sensors(pf_dev, NULL);
	if (ret)
		return ret;

	for (i = 0; i < SMI_ALARM_MAX; i++)
		check_and_trigger_alarm(pf_dev, &a->monitors[i]);

	return 0;
}

int smi_alarm_init(struct pf_dev_struct *pf_dev)
{
	struct smi_alarm *a;

	if (!pf_dev)
		return -EINVAL;

	a = devm_kzalloc(pf_dev->dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	a->last_try = jiffies;
	register_monitor(a, pf_dev, SMI_ALARM_FPGA_OVER_TEMP);
	register_monitor(a, pf_dev, SMI_ALARM_ETH_OVER_TEMP);

	/* Last: a sweep that sees it sees armed monitors behind it. */
	smp_store_release(&pf_dev->alarm, a);
	return 0;
}

void smi_alarm_evaluate(struct pf_dev_struct *pf_dev)
{
	struct smi_alarm *a = pf_dev ? smp_load_acquire(&pf_dev->alarm) : NULL;
	int ret;

	if (!a)
		return;

	/* Suspending keeps a dead card from filling the log, so it must not
	 * outlive the card's silence: a suspended sweep still tries once a
	 * rate-limit interval, and a try that lands resumes it. */
	if ((a->fail_count >= ALARM_FAIL_SUSPEND) &&
	    !time_after(jiffies,
			a->last_try +
				msecs_to_jiffies(ALARM_RATE_LIMIT_INTERVAL)))
		return;

	a->last_try = jiffies;
	ret = alarm_sweep(pf_dev, a);
	if (!ret) {
		if (a->fail_count >= ALARM_FAIL_SUSPEND)
			SMI_DEV_INFO(pf_dev->dev, "Alarm monitoring resumed");
		a->fail_count = 0;
		return;
	}

	/* Neither an abort nor silence a live request still has time for
	 * counts toward suspending. */
	if (smi_cmd_aborted(ret) || smi_smc_deadline_pending(pf_dev->smc_ctx))
		return;

	if (++a->fail_count == ALARM_FAIL_SUSPEND)
		SMI_DEV_ERR(
			pf_dev->dev,
			"Alarm monitoring suspended after repeated sensor failures (%d)",
			ret);
}

int mango_mgmt_smi_register_alarm_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&smi_alarm_chain, nb);
}

int mango_mgmt_smi_unregister_alarm_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&smi_alarm_chain, nb);
}
