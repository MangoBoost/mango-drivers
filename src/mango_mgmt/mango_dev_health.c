// SPDX-License-Identifier: GPL-2.0
/*
 * mango_dev health -- devlink health reporter for a mango hardware unit.
 *
 * The only remedy the datapath responds to is a PCIe re-enumeration of the
 * whole unit. The fatal reporter's .recover performs it.
 *
 * The instance is anchored on a device of this layer's own bus, parented on
 * the unit's class node. A data function would be evicted by the recovery it
 * reports, and the class node has no bus while devlink_nl_put_handle()
 * dereferences devlink->dev->bus->name.
 *
 * One reporter only. Any reporter with a .recover gets auto_recover,
 * and an observability-only companion would arm a bus reset or report nothing.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include "mango_dev_internal.h"
#include "smi/smi_api.h"

#if IS_ENABLED(CONFIG_NET_DEVLINK)

#include <net/devlink.h>

#include <mango_compat/devlink.h>

#define MANGO_DEV_REASON_LEN 64
#define MANGO_DEV_LABEL_LEN 16
#define MANGO_DEV_BDF_LEN 16

/**
 * struct mango_dev_health_record - the fatal reporter's last report
 * @reason: summary text, copied because the reporting module may unload.
 * @source: decoded label for what raised it, empty when none was given.
 * @src_aux: decoded label for the aux that raised it, empty otherwise.
 * @code: source-specific status word, such as a PCIe AER status.
 * @has_code: @code carries a value. A zero status is meaningful.
 */
struct mango_dev_health_record {
	char reason[MANGO_DEV_REASON_LEN];
	char source[MANGO_DEV_LABEL_LEN];
	char src_aux[MANGO_DEV_LABEL_LEN];
	u64 code;
	bool has_code;
};

/**
 * struct mango_dev_health - the devlink health state of one hardware unit
 * @hw: the unit this belongs to.
 * @devlink: devlink instance anchored on @anchor.
 * @fatal: the destructive reporter, NULL when it could not be created.
 * @anchor: device on the mango_dev bus the instance is registered on. Not a
 *          PCI function, and therefore untouched by the eviction.
 * @recover_work: deferred body of .recover, owned by the unit for the same
 *                reason; queued again while a reset fence holds the unit.
 * @recover_deferred: the hold has been said once (@recover_work alone).
 * @recover_attempts: recoveries taken in the current window (@hw->reset_lock).
 * @recover_window: jiffies at the start of the window (@hw->reset_lock).
 * @recover_abandoned: budget spent until an operator reset (@hw->reset_lock).
 * @fault_lock: guards @fault_pending and @fault_seq. Taken outside
 *              @hw->reset_lock, a recovery in flight must not block a report.
 * @fault_pending: a fatal was reported and no recovery has cleared it yet.
 * @fault_seq: bumped on every fatal report, tells a fault raised mid-recovery
 *             from the one the recovery ran for.
 * @record: the last report.
 */
struct mango_dev_health {
	struct mango_dev *hw;
	struct devlink *devlink;
	struct devlink_health_reporter *fatal;
	struct device *anchor;
	struct delayed_work recover_work;
	bool recover_deferred;

	unsigned int recover_attempts;
	unsigned long recover_window;
	bool recover_abandoned;

	spinlock_t fault_lock;
	bool fault_pending;
	u32 fault_seq;

	struct mango_dev_health_record record;
};

/*
 * Must stay zero. devlink marks the reporter ERROR before it tests the grace
 * period. A report inside the period is dropped without a recovery, and every
 * later one is dropped on the previous-error-not-recovered test.
 * MANGO_DEV_RECOVER_BUDGET is the rate limit instead.
 */
#define MANGO_DEV_HEALTH_GRACE_MS 0

/*
 * A broken bitfile faults again on every successful re-enumeration. Success
 * therefore never clears the counter, only a quiet window or an operator does.
 */
#define MANGO_DEV_RECOVER_BUDGET 3
#define MANGO_DEV_RECOVER_WINDOW_MS (10 * 60 * 1000)
/* How long a recovery a reset fence holds back waits to ask again. */
#define MANGO_DEV_RECOVER_RETRY_MS (5000)

static struct mango_dev_health *
mango_dev_health_from_reporter(struct devlink_health_reporter *reporter)
{
	return devlink_health_reporter_priv(reporter);
}

/**
 * mango_dev_budget_take() - claim one autonomous recovery from the budget.
 * @health: the unit's health state. Caller holds @health->hw->reset_lock.
 *
 * Return: true when a recovery may proceed. Abandoned is sticky until reset.
 */
static bool mango_dev_budget_take(struct mango_dev_health *health)
{
	unsigned long now = jiffies;

	if (health->recover_abandoned)
		return false;

	if (!health->recover_attempts ||
	    time_after(now,
		       health->recover_window +
			       msecs_to_jiffies(MANGO_DEV_RECOVER_WINDOW_MS))) {
		health->recover_window = now;
		health->recover_attempts = 0;
	}

	if (health->recover_attempts >= MANGO_DEV_RECOVER_BUDGET) {
		health->recover_abandoned = true;
		dev_err(health->hw->dev,
			"%u autonomous recoveries within %u s -- abandoning; device left down (clear with a manual reset)\n",
			health->recover_attempts,
			MANGO_DEV_RECOVER_WINDOW_MS / 1000);
		return false;
	}

	health->recover_attempts++;
	return true;
}

/**
 * mango_dev_alive_locked() - whether this unit is working right now
 * @health: the unit's health state. Caller holds mango_dev_lock.
 *
 * Return: true when no member is absent and no fault is pending.
 */
static bool mango_dev_alive_locked(struct mango_dev_health *health)
{
	struct mango_dev_func *f;

	if (list_empty(&health->hw->funcs))
		return false;
	list_for_each_entry (f, &health->hw->funcs, node)
		if (!READ_ONCE(f->present))
			return false;
	return !READ_ONCE(health->fault_pending);
}

/**
 * mango_dev_health_sync() - settle the reporter state on the unit
 * @hw: the hardware unit. Caller holds mango_dev_lock.
 *
 * Suppressed while reset_in_flight is set, a reset tears the roster up by
 * design. The state is settled once at its end.
 */
void mango_dev_health_sync(struct mango_dev *hw)
{
	struct mango_dev_health *health = hw->health;

	lockdep_assert_held(&mango_dev_lock);

	if (!health || !health->fatal || READ_ONCE(hw->reset_in_flight))
		return;

	devlink_health_reporter_state_update(
		health->fatal, mango_dev_alive_locked(health)
				       ? DEVLINK_HEALTH_REPORTER_STATE_HEALTHY
				       : DEVLINK_HEALTH_REPORTER_STATE_ERROR);
}

/**
 * mango_dev_fault_arm() - record that a fatal was just reported
 * @health: the unit's health state.
 */
static void mango_dev_fault_arm(struct mango_dev_health *health)
{
	spin_lock(&health->fault_lock);
	health->fault_seq++;
	WRITE_ONCE(health->fault_pending, true);
	spin_unlock(&health->fault_lock);
}

/**
 * mango_dev_fault_seq() - the sequence number of the last fatal report
 * @health: the unit's health state.
 *
 * Return: the current sequence number.
 */
static u32 mango_dev_fault_seq(struct mango_dev_health *health)
{
	u32 seq;

	spin_lock(&health->fault_lock);
	seq = health->fault_seq;
	spin_unlock(&health->fault_lock);

	return seq;
}

/**
 * mango_dev_fault_settle() - lower the pending flag if no newer fault arrived
 * @health: the unit's health state.
 * @seq: the sequence number the caller snapshotted before its reset.
 *
 * Return: true when the flag was lowered, false when a newer fault stands.
 *         Compare and clear in one step, or a report landing between them
 *         would have its own fault erased.
 */
static bool mango_dev_fault_settle(struct mango_dev_health *health, u32 seq)
{
	bool settled;

	spin_lock(&health->fault_lock);
	settled = health->fault_seq == seq;
	if (settled)
		WRITE_ONCE(health->fault_pending, false);
	spin_unlock(&health->fault_lock);

	return settled;
}

void mango_dev_budget_clear(struct mango_dev *hw)
{
	struct mango_dev_health *health = hw->health;

	if (!health)
		return;

	health->recover_attempts = 0;
	health->recover_window = jiffies;
	health->recover_abandoned = false;

	spin_lock(&health->fault_lock);
	WRITE_ONCE(health->fault_pending, false);
	spin_unlock(&health->fault_lock);
}

/**
 * mango_dev_recover_work_fn() - the deferred body of the fatal .recover.
 * @w: @health->recover_work.
 *
 * .recover runs synchronously inside devlink_health_report(), which mango_core
 * calls from its own fatal work. Re-enumerating there deadlocks, the evicted
 * data functions' .remove waits on that very work. So .recover only queues,
 * and this runs on work the unit owns. The budget is claimed here because it
 * lives under @reset_lock, which .recover must not take behind a recovery in
 * flight. A fault raised during the re-enumeration is recovered again from
 * here, devlink drops its report while the reporter is still at ERROR.
 */
static void mango_dev_recover_work_fn(struct work_struct *w)
{
	struct mango_dev_health *health = container_of(
		to_delayed_work(w), struct mango_dev_health, recover_work);
	const struct mango_dev_health_record *fault = &health->record;
	struct mango_dev *hw = health->hw;
	bool again = false;
	u32 seq;
	int ret;

	do {
		mutex_lock(&hw->reset_lock);
		if (mango_dev_reset_claim(hw)) {
			mutex_unlock(&hw->reset_lock);
			if (!health->recover_deferred)
				dev_warn(
					hw->dev,
					"a reset fence holds the unit -- recovering once it clears\n");
			health->recover_deferred = true;
			queue_delayed_work(
				system_unbound_wq, &health->recover_work,
				msecs_to_jiffies(MANGO_DEV_RECOVER_RETRY_MS));
			return;
		}
		health->recover_deferred = false;
		/* An operator reset meanwhile cleared the fault: nothing is
		 * left to recover. */
		if (!READ_ONCE(health->fault_pending)) {
			mango_dev_reset_unclaim(hw);
			mutex_unlock(&hw->reset_lock);
			return;
		}
		if (!mango_dev_budget_take(health)) {
			mango_dev_reset_unclaim(hw);
			mutex_unlock(&hw->reset_lock);
			mutex_lock(&mango_dev_lock);
			mango_dev_health_sync(hw);
			mutex_unlock(&mango_dev_lock);
			return;
		}
		seq = mango_dev_fault_seq(health);
		dev_warn(hw->dev, "autonomous recovery %u/%u: %s\n",
			 health->recover_attempts, MANGO_DEV_RECOVER_BUDGET,
			 fault->reason[0] ? fault->reason : "hw fault");
		ret = mango_dev_reset_remove(hw, true);
		mango_dev_reset_unclaim(hw);
		again = !ret && !mango_dev_fault_settle(health, seq);
		mutex_unlock(&hw->reset_lock);

		if (!ret && health->fatal)
			devlink_health_reporter_recovery_done(health->fatal);
		mutex_lock(&mango_dev_lock);
		mango_dev_health_sync(hw);
		mutex_unlock(&mango_dev_lock);

		if (ret)
			dev_err(hw->dev,
				"autonomous recovery failed (%d); device left down\n",
				ret);
		else if (again)
			dev_warn(
				hw->dev,
				"a fault landed during re-enumeration -- recovering again\n");
		else
			dev_info(hw->dev, "autonomous recovery complete\n");
	} while (again);
}

/*
 * -EINPROGRESS leaves the outcome to the work: a 0 would count a recovery not
 * yet tried. An abandoned unit is refused; a
 * reset fence is the work's to judge.
 */
static int mango_dev_health_recover(struct devlink_health_reporter *reporter,
				    void *priv_ctx)
{
	struct mango_dev_health *health =
		mango_dev_health_from_reporter(reporter);

	if (READ_ONCE(health->recover_abandoned))
		return -ENODEV;

	queue_delayed_work(system_unbound_wq, &health->recover_work, 0);
	return -EINPROGRESS;
}

/*
 * The dump is the detection-time record alone. MMIO is gated dead by the time
 * a fatal is reported, and a register snapshot at detection time would not
 * tell a wedged unit from a healthy one. devlink runs this inline under devl_lock ahead of .recover, and a non-posted
 * read into a stuck endpoint would block the recovery it describes.
 *
 * fmsg puts return void from v6.8 and are not __must_check before that.
 */

/**
 * mango_dev_primary_bdf() - format the unit's primary function as a BDF.
 * @hw: the unit. hw_id carries PCI_DEVID(bus, devfn) in its low 16 bits.
 * @buf: receives "DOMAIN:BUS:SLOT.FUNC".
 * @len: length of @buf.
 */
static void mango_dev_primary_bdf(const struct mango_dev *hw, char *buf,
				  size_t len)
{
	u8 devfn = hw->hw_id & 0xff;

	snprintf(buf, len, "%04x:%02x:%02x.%x", hw->domain,
		 PCI_BUS_NUM(hw->hw_id), PCI_SLOT(devfn), PCI_FUNC(devfn));
}

static void mango_dev_primary_bdf_put(struct devlink_fmsg *fmsg,
				      const struct mango_dev *hw)
{
	char bdf[MANGO_DEV_BDF_LEN];

	mango_dev_primary_bdf(hw, bdf, sizeof(bdf));
	devlink_fmsg_string_pair_put(fmsg, "primary_bdf", bdf);
}

static void mango_dev_health_dump_fill(struct mango_dev_health *health,
				       struct devlink_fmsg *fmsg)
{
	const struct mango_dev_health_record *rec = &health->record;

	mango_dev_primary_bdf_put(fmsg, health->hw);
	devlink_fmsg_string_pair_put(fmsg, "reason",
				     rec->reason[0] ? rec->reason : "none");
	if (rec->source[0])
		devlink_fmsg_string_pair_put(fmsg, "source", rec->source);
	if (rec->src_aux[0])
		devlink_fmsg_string_pair_put(fmsg, "source_aux", rec->src_aux);
	if (rec->has_code)
		devlink_fmsg_u64_pair_put(fmsg, "source_code", rec->code);
}

/**
 * enum mango_dev_verdict - how bad this unit's condition is
 * @MANGO_DEV_VERDICT_NORMAL: whole roster on the bus, nothing outstanding.
 * @MANGO_DEV_VERDICT_RECOVERABLE: a fault is latched and the budget has room.
 * @MANGO_DEV_VERDICT_FATAL: a member is missing, or the budget is spent.
 */
enum mango_dev_verdict {
	MANGO_DEV_VERDICT_NORMAL,
	MANGO_DEV_VERDICT_RECOVERABLE,
	MANGO_DEV_VERDICT_FATAL,
};

/**
 * mango_dev_roster_incomplete() - whether a member function is missing
 * @present: member functions currently on the bus.
 * @total: member functions registered.
 *
 * Return: true when the whole roster is not on the bus.
 */
static bool mango_dev_roster_incomplete(unsigned int present,
					unsigned int total)
{
	return !total || present < total;
}

/**
 * mango_dev_verdict_of() - the ladder the severity and remedy words share
 * @health: the unit's health state.
 * @present: member functions currently on the bus.
 * @total: member functions registered.
 *
 * Return: the unit's verdict. Status is not on this ladder, a latched fault
 *         with budget left and one without share a status and not a remedy.
 */
static enum mango_dev_verdict
mango_dev_verdict_of(struct mango_dev_health *health, unsigned int present,
		     unsigned int total)
{
	if (mango_dev_roster_incomplete(present, total) ||
	    health->recover_abandoned)
		return MANGO_DEV_VERDICT_FATAL;
	if (READ_ONCE(health->fault_pending))
		return MANGO_DEV_VERDICT_RECOVERABLE;
	return MANGO_DEV_VERDICT_NORMAL;
}

/**
 * mango_dev_status() - what this unit is doing, on the devlink vocabulary
 * @health: the unit's health state.
 * @present: member functions currently on the bus.
 * @total: member functions registered.
 *
 * Return: "recovering", "error" or "healthy". See mango_dev_verdict_of().
 */
static const char *mango_dev_status(struct mango_dev_health *health,
				    unsigned int present, unsigned int total)
{
	if (READ_ONCE(health->hw->reset_in_flight))
		return "recovering";
	if (mango_dev_roster_incomplete(present, total) ||
	    READ_ONCE(health->fault_pending))
		return "error";
	return "healthy";
}

/**
 * mango_dev_severity() - how bad the error is
 * @verdict: the unit's verdict.
 *
 * Return: a stable label.
 */
static const char *mango_dev_severity(enum mango_dev_verdict verdict)
{
	static const char *const names[] = {
		[MANGO_DEV_VERDICT_NORMAL] = "normal",
		[MANGO_DEV_VERDICT_RECOVERABLE] = "recoverable",
		[MANGO_DEV_VERDICT_FATAL] = "fatal",
	};

	return names[verdict];
}

/**
 * mango_dev_remedy() - what makes this unit work again
 * @verdict: the unit's verdict.
 *
 * Return: a stable label, the field an operator acts on.
 */
static const char *mango_dev_remedy(enum mango_dev_verdict verdict)
{
	static const char *const names[] = {
		[MANGO_DEV_VERDICT_NORMAL] = "none",
		[MANGO_DEV_VERDICT_RECOVERABLE] = "devlink_recover",
		[MANGO_DEV_VERDICT_FATAL] = "operator_reset",
	};

	return names[verdict];
}

/*
 * The live view. Each input behind status/severity/remedy is emitted beside
 * it. last_fault_reason outlives the fault, fault_pending says it is current.
 */
static void mango_dev_health_diagnose_fill(struct mango_dev_health *health,
					   struct devlink_fmsg *fmsg)
{
	const struct mango_dev_health_record *rec = &health->record;
	unsigned int present = 0, total = 0;
	enum mango_dev_verdict verdict;
	struct mango_dev_func *f;

	mutex_lock(&mango_dev_lock);
	list_for_each_entry (f, &health->hw->funcs, node) {
		total++;
		if (READ_ONCE(f->present))
			present++;
	}
	verdict = mango_dev_verdict_of(health, present, total);

	mango_dev_primary_bdf_put(fmsg, health->hw);
	devlink_fmsg_string_pair_put(fmsg, "status",
				     mango_dev_status(health, present, total));
	devlink_fmsg_string_pair_put(fmsg, "severity",
				     mango_dev_severity(verdict));
	devlink_fmsg_string_pair_put(fmsg, "remedy", mango_dev_remedy(verdict));
	devlink_fmsg_bool_pair_put(fmsg, "reset_in_flight",
				   READ_ONCE(health->hw->reset_in_flight));
	devlink_fmsg_u32_pair_put(fmsg, "reset_fences",
				  READ_ONCE(health->hw->reset_fences));
	devlink_fmsg_u32_pair_put(fmsg, "members_present", present);
	devlink_fmsg_u32_pair_put(fmsg, "members_total", total);
	devlink_fmsg_bool_pair_put(fmsg, "fault_pending",
				   READ_ONCE(health->fault_pending));
	devlink_fmsg_u32_pair_put(fmsg, "recover_attempts",
				  health->recover_attempts);
	devlink_fmsg_u32_pair_put(fmsg, "recover_budget",
				  MANGO_DEV_RECOVER_BUDGET);
	devlink_fmsg_bool_pair_put(fmsg, "recover_abandoned",
				   health->recover_abandoned);
	devlink_fmsg_string_pair_put(fmsg, "last_fault_reason",
				     rec->reason[0] ? rec->reason : "none");

	devlink_fmsg_arr_pair_nest_start(fmsg, "members");
	list_for_each_entry (f, &health->hw->funcs, node) {
		devlink_fmsg_obj_nest_start(fmsg);
		devlink_fmsg_string_pair_put(fmsg, "role", f->role);
		devlink_fmsg_string_pair_put(fmsg, "id", f->id);
		devlink_fmsg_bool_pair_put(fmsg, "present",
					   READ_ONCE(f->present));
		devlink_fmsg_obj_nest_end(fmsg);
	}
	mutex_unlock(&mango_dev_lock);
	devlink_fmsg_arr_pair_nest_end(fmsg);
}

static int mango_dev_health_dump(struct devlink_health_reporter *reporter,
				 struct devlink_fmsg *fmsg, void *priv_ctx)
{
	mango_dev_health_dump_fill(mango_dev_health_from_reporter(reporter),
				   fmsg);
	return 0;
}

static int mango_dev_health_diagnose(struct devlink_health_reporter *reporter,
				     struct devlink_fmsg *fmsg)
{
	mango_dev_health_diagnose_fill(mango_dev_health_from_reporter(reporter),
				       fmsg);
	return 0;
}

static int mango_dev_health_recover_op(struct devlink_health_reporter *reporter,
				       void *priv_ctx,
				       struct netlink_ext_ack *extack)
{
	return mango_dev_health_recover(reporter, priv_ctx);
}

static int mango_dev_health_dump_op(struct devlink_health_reporter *reporter,
				    struct devlink_fmsg *fmsg, void *priv_ctx,
				    struct netlink_ext_ack *extack)
{
	return mango_dev_health_dump(reporter, fmsg, priv_ctx);
}

static int
mango_dev_health_diagnose_op(struct devlink_health_reporter *reporter,
			     struct devlink_fmsg *fmsg,
			     struct netlink_ext_ack *extack)
{
	return mango_dev_health_diagnose(reporter, fmsg);
}

static const struct devlink_health_reporter_ops mango_dev_fatal_ops = {
	.name = "fatal",
	.recover = mango_dev_health_recover_op,
	.dump = mango_dev_health_dump_op,
	.diagnose = mango_dev_health_diagnose_op,
#ifdef MANGO_COMPAT_DEVLINK_HEALTH_GRACE_IN_OPS
	.default_graceful_period = MANGO_DEV_HEALTH_GRACE_MS,
#endif
};

/*
 * fw.mgmt is what firmware stated at probe, read from the SMI function without
 * a frame. No serial_number, there is no PCI DSN and the slot is the handle.
 */
static int mango_dev_devlink_info_get(struct devlink *devlink,
				      struct devlink_info_req *req,
				      struct netlink_ext_ack *extack)
{
	struct mango_dev *hw = *(struct mango_dev **)devlink_priv(devlink);
	char bdf[MANGO_DEV_BDF_LEN];
	char addr[MANGO_DEV_BDF_LEN + 5];
	char version[16];
	int err;

	err = mango_compat_devlink_info_driver_name_put(req, "mango");
	if (err)
		return err;

	mango_dev_primary_bdf(hw, bdf, sizeof(bdf));
	snprintf(addr, sizeof(addr), "pcie:%s", bdf);
	if (mango_mgmt_smi_get_fw_version(addr, version, sizeof(version)))
		return 0;

	return devlink_info_version_running_put(
		req, DEVLINK_INFO_VERSION_GENERIC_FW_MGMT, version);
}

static const struct devlink_ops mango_dev_devlink_ops = {
	.info_get = mango_dev_devlink_info_get,
};

static struct bus_type mango_dev_bus_type = {
	.name = "mango_dev",
};

/**
 * mango_dev_anchor_create() - build the device to anchor devlink on.
 * @health: the unit's health state. @health->hw->dev must exist.
 *
 * Return: 0 on success, with @health->anchor set.
 */
static int mango_dev_anchor_create(struct mango_dev_health *health)
{
	struct platform_device *pdev;
	struct device *dev;
	int ret;

	if (!health->hw->dev)
		return -ENODEV;

	pdev = platform_device_alloc("mango_dev_anchor", PLATFORM_DEVID_NONE);
	if (!pdev)
		return -ENOMEM;

	dev = &pdev->dev;
	dev->bus = &mango_dev_bus_type;
	dev->parent = health->hw->dev;

	ret = dev_set_name(dev, "%s", dev_name(health->hw->dev));
	if (!ret)
		ret = device_add(dev);
	if (ret) {
		platform_device_put(pdev);
		return ret;
	}

	health->anchor = dev;
	return 0;
}

static void mango_dev_anchor_destroy(struct mango_dev_health *health)
{
	if (!health->anchor)
		return;

	device_unregister(health->anchor);
	health->anchor = NULL;
}

/**
 * mango_dev_health_init() - register the bus every devlink anchor sits on.
 *
 * Return: 0 on success, or the bus_register() error.
 */
int mango_dev_health_init(void)
{
	return bus_register(&mango_dev_bus_type);
}

/**
 * mango_dev_health_exit() - unregister the anchor bus, after the last unit.
 */
void mango_dev_health_exit(void)
{
	bus_unregister(&mango_dev_bus_type);
}

void mango_dev_health_create(struct mango_dev *hw)
{
	struct mango_dev_health *health;
	struct devlink *dl;
	int ret;

	if (hw->xport != MANGO_DEV_XPORT_PCI)
		return;

	health = kzalloc(sizeof(*health), GFP_KERNEL);
	if (!health) {
		dev_warn(
			hw->dev,
			"health alloc failed; devlink health and autonomous recovery unavailable\n");
		return;
	}
	health->hw = hw;
	spin_lock_init(&health->fault_lock);
	INIT_DELAYED_WORK(&health->recover_work, mango_dev_recover_work_fn);
	health->recover_window = jiffies;

	ret = mango_dev_anchor_create(health);
	if (ret) {
		dev_warn(
			hw->dev,
			"devlink anchor unavailable (%d); health unavailable\n",
			ret);
		kfree(health);
		return;
	}

	dl = devlink_alloc(&mango_dev_devlink_ops, sizeof(struct mango_dev *),
			   health->anchor);
	if (!dl) {
		dev_warn(hw->dev, "devlink alloc failed; health unavailable\n");
		mango_dev_anchor_destroy(health);
		kfree(health);
		return;
	}
	*(struct mango_dev **)devlink_priv(dl) = hw;

	health->fatal = mango_compat_devlink_health_reporter_create(
		dl, &mango_dev_fatal_ops, MANGO_DEV_HEALTH_GRACE_MS, health);
	if (IS_ERR_OR_NULL(health->fatal)) {
		dev_err(hw->dev,
			"fatal reporter create failed; autonomous recovery unavailable\n");
		health->fatal = NULL;
	}

	if (mango_compat_devlink_register(dl)) {
		dev_err(hw->dev,
			"devlink register failed; health unavailable\n");
		if (health->fatal)
			devlink_health_reporter_destroy(health->fatal);
		devlink_free(dl);
		mango_dev_anchor_destroy(health);
		kfree(health);
		return;
	}

	health->devlink = dl;
	hw->health = health;
	dev_info(hw->dev, "devlink health at mango_dev/%s\n",
		 dev_name(health->anchor));
}

void mango_dev_health_destroy(struct mango_dev *hw)
{
	struct mango_dev_health *health = hw->health;

	if (!health)
		return;

	/*
	 * Unregister before cancelling. A netlink recover can queue
	 * recover_work while the instance is still registered, and a cancel
	 * that ran first would leave that work to run against freed storage.
	 */
	devlink_unregister(health->devlink);
	cancel_delayed_work_sync(&health->recover_work);
	if (health->fatal)
		devlink_health_reporter_destroy(health->fatal);
	devlink_free(health->devlink);
	mango_dev_anchor_destroy(health);

	hw->health = NULL;
	kfree(health);
}

void mango_dev_report_health(u16 domain, u8 bus, const char *reason,
			     const struct mango_dev_health_detail *detail)
{
	struct mango_dev *hw = mango_dev_find_slot(domain, bus);
	struct mango_dev_health_record *rec;
	struct mango_dev_health *health;

	if (!hw || !hw->health || !hw->health->fatal)
		return;

	health = hw->health;
	rec = &health->record;
	memset(rec, 0, sizeof(*rec));
	strscpy(rec->reason, reason ? reason : "unspecified",
		sizeof(rec->reason));
	if (detail) {
		if (detail->source)
			strscpy(rec->source, detail->source,
				sizeof(rec->source));
		if (detail->src_aux)
			strscpy(rec->src_aux, detail->src_aux,
				sizeof(rec->src_aux));
		rec->code = detail->code;
		rec->has_code = detail->has_code;
	}
	mango_dev_fault_arm(health);
	devlink_health_report(health->fatal, rec->reason, NULL);
}
EXPORT_SYMBOL_GPL(mango_dev_report_health);

#else /* CONFIG_NET_DEVLINK */

int mango_dev_health_init(void)
{
	return 0;
}

void mango_dev_health_exit(void)
{
}

void mango_dev_health_create(struct mango_dev *hw)
{
	if (hw->xport == MANGO_DEV_XPORT_PCI)
		dev_warn(
			hw->dev,
			"kernel has no devlink; health and autonomous recovery unavailable, recover with the node's reset attribute\n");
}

void mango_dev_health_destroy(struct mango_dev *hw)
{
}

void mango_dev_budget_clear(struct mango_dev *hw)
{
}

void mango_dev_health_sync(struct mango_dev *hw)
{
}

void mango_dev_report_health(u16 domain, u8 bus, const char *reason,
			     const struct mango_dev_health_detail *detail)
{
}
EXPORT_SYMBOL_GPL(mango_dev_report_health);

#endif /* CONFIG_NET_DEVLINK */
