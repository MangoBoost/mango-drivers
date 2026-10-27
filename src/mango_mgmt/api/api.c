// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - API Device Registry + RPC Lifecycle
 *
 * Owns the global device xarray, init/exit lifecycle, RPC startup,
 * SoC-side handler registration, and utility functions (device lookup,
 * EEPROM, SoC IP).
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/rcupdate.h>

#include "internal.h"
#include "api.h"
#include "../transport/transport.h"
#include "../mpipe/nat.h"
#include "../smi_glue.h"

/* ===== Global xarray definition ===== */

DEFINE_XARRAY(mango_mgmt_device_xa);

/* ===== Project registry =====
 * Serialized by mango_mgmt_projects_lock.  Readers take rcu_read_lock
 * around rcu_dereference() of a slot pointer.
 */

struct mango_mgmt_project __rcu *mango_mgmt_projects[MANGO_MGMT_PRJ_MAX];
DEFINE_MUTEX(mango_mgmt_projects_lock);

/* ===== RPC IP-change callback (called from work item context) ===== */

static void mango_mgmt_rpc_on_ip_change(void *ctx, __be32 new_soc_ext)
{
	struct mango_mgmt_device_entry *entry = ctx;

	/* Tear down old RPC client */
	mango_mgmt_rpc_client_exit(&entry->fwd_rpc);

	if (new_soc_ext == MANGO_MGMT_IP_UNSET) {
		dev_info(entry->mgmt_dev->dev,
			 "RPC: IP removed, client shut down\n");
		return;
	}

	/* Recreate with new SoC external IP */
	if (mango_mgmt_rpc_client_init(entry->mgmt_dev, &entry->fwd_rpc,
				       new_soc_ext, mango_mgmt_rpc_port))
		dev_err(entry->mgmt_dev->dev,
			"RPC: reconnect to new SoC IP failed\n");
	else
		dev_info(entry->mgmt_dev->dev,
			 "RPC: reconnected to new SoC IP %pI4\n", &new_soc_ext);
}

/* ===== RPC startup kthread =====
 *
 * SoC: starts RPC server immediately (SoC has IP from systemd-networkd).
 * Host: waits for link_ready (user must assign IP first), then starts
 *       RPC client to the auto-derived SoC external IP.
 */

static int mango_mgmt_rpc_startup_kthread(void *data)
{
	struct mango_mgmt_device_entry *entry = data;
	struct mango_mgmt_dev *mgmt_dev = entry->mgmt_dev;
	bool is_soc = mango_mgmt_transport_is_soc_device(mgmt_dev);
	int ret;

	if (is_soc) {
		/* SoC: serve forward-channel RPC on :1023 */
		ret = mango_mgmt_rpc_server_init(mgmt_dev, &entry->fwd_rpc,
						 mango_mgmt_rpc_port,
						 &mango_mgmt_fwd_svc_version);
		if (ret)
			dev_err(mgmt_dev->dev,
				"RPC: forward server init failed (%d)\n", ret);
		else
			mango_mgmt_rpc_set_server_entry(entry);
	} else {
		/* Host: wait for user to assign IP (NAT ready gate) */
		struct mango_mgmt_mpipe_nat *nat =
			mango_mgmt_mpipe_get_nat(mgmt_dev);
		long ext;
		__be32 soc_ext;

		if (!nat)
			goto park;

		ret = wait_event_interruptible(
			nat->ready_wq,
			kthread_should_stop() || mango_mgmt_nat_ready(nat));
		if (kthread_should_stop() || ret)
			goto park;

		ext = atomic_long_read(&nat->external);
		soc_ext = nat_soc(ext);

		ret = mango_mgmt_rpc_client_init(mgmt_dev, &entry->fwd_rpc,
						 soc_ext, mango_mgmt_rpc_port);
		if (ret)
			dev_err(mgmt_dev->dev,
				"RPC: forward client init failed (%d)\n", ret);

		/* Register for IP-change notifications */
		nat->on_ip_change = mango_mgmt_rpc_on_ip_change;
		nat->on_ip_change_ctx = entry;
	}

	dev_info(mgmt_dev->dev, "API RPC: started (%s mode)\n",
		 is_soc ? "SoC" : "Host");

park:
	/* Park until kthread_stop() is called during device removal.
	 * The thread must not exit on its own — api_exit relies on
	 * kthread_stop() which is only safe on a live task_struct. */
	while (!kthread_should_stop())
		schedule_timeout_interruptible(MAX_SCHEDULE_TIMEOUT);
	return 0;
}

/* ===== Lifecycle: init / exit / module_exit ===== */

int mango_mgmt_api_init(struct mango_mgmt_dev *mgmt_dev)
{
	struct mango_mgmt_device_entry *entry;
	int ret;

	if (!mgmt_dev || mgmt_dev->mgmt.devid < 0)
		return -EINVAL;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->mgmt_dev = mgmt_dev;
	entry->devid = mgmt_dev->mgmt.devid;

	/* Start RPC startup kthread (waits for mpipe, then inits RPC).
	 * The thread parks after setup until kthread_stop() in api_exit. */
	entry->rpc_startup_thread =
		kthread_run(mango_mgmt_rpc_startup_kthread, entry,
			    "mango_mgmt_rpc/%d", mgmt_dev->mgmt.devid);
	if (IS_ERR(entry->rpc_startup_thread)) {
		ret = PTR_ERR(entry->rpc_startup_thread);
		dev_err(mgmt_dev->dev,
			"Failed to create RPC startup thread: %d", ret);
		kfree(entry);
		return ret;
	}

	ret = xa_insert(&mango_mgmt_device_xa, mgmt_dev->mgmt.devid, entry,
			GFP_KERNEL);
	if (ret) {
		if (ret == -EBUSY)
			ret = -EEXIST;
		kthread_stop(entry->rpc_startup_thread);
		kfree(entry);
		return ret;
	}

	ret = mango_mgmt_meta_attach_dev(entry);
	if (ret) {
		dev_warn(
			mgmt_dev->dev,
			"meta project attach failed (%d); introspection unavailable",
			ret);
		/* Non-fatal: an aux module's check_soc_prj_* will return
		 * false, forcing host-mode fallback. */
	}

	dev_info(mgmt_dev->dev, "API initialized (devid=%d)",
		 mgmt_dev->mgmt.devid);
	return 0;
}

void mango_mgmt_api_exit(struct mango_mgmt_dev *mgmt_dev)
{
	struct mango_mgmt_device_entry *entry;

	if (!mgmt_dev)
		return;

	entry = xa_erase(&mango_mgmt_device_xa, mgmt_dev->mgmt.devid);
	if (entry) {
		struct mango_mgmt_mpipe_nat *nat =
			mango_mgmt_mpipe_get_nat(mgmt_dev);
		u32 prj_id, port;
		bool had_instances = false;

		/* Detach meta first so its priv no longer points into this
		 * entry while other teardown runs.  Operates on the entry
		 * pointer directly — the entry is already out of the
		 * xarray at this point. */
		mango_mgmt_meta_detach_dev(entry);

		/* Unregister IP-change callback before tearing down RPC */
		if (nat) {
			nat->on_ip_change = NULL;
			nat->on_ip_change_ctx = NULL;
		}

		/* Stop the startup kthread (parked in schedule_timeout) */
		if (entry->rpc_startup_thread)
			kthread_stop(entry->rpc_startup_thread);

		/* Clear SoC-side server context */
		mango_mgmt_rpc_set_server_entry(NULL);

		/* Shut down RPC channels */
		mango_mgmt_rpc_client_exit(&entry->fwd_rpc);
		mango_mgmt_rpc_server_exit(&entry->fwd_rpc);

		/* Clear any project instances still bound to this entry.
		 * An aux module that hasn't detached is a bug — WARN.  The
		 * projects_lock also serializes against attach/detach from
		 * other threads so the kfree below is safe. */
		mutex_lock(&mango_mgmt_projects_lock);
		for (prj_id = 0; prj_id < MANGO_MGMT_PRJ_MAX; prj_id++) {
			if (rcu_dereference_protected(
				    entry->inst_devid[prj_id],
				    lockdep_is_held(
					    &mango_mgmt_projects_lock))) {
				WARN(1,
				     "mango_mgmt: devid %d prj %u still attached at api_exit\n",
				     mgmt_dev->mgmt.devid, prj_id);
				rcu_assign_pointer(entry->inst_devid[prj_id],
						   NULL);
				had_instances = true;
			}
			for (port = 0; port < MANGO_MGMT_MAX_PORTS; port++) {
				if (rcu_dereference_protected(
					    entry->inst_port[prj_id][port],
					    lockdep_is_held(
						    &mango_mgmt_projects_lock))) {
					WARN(1,
					     "mango_mgmt: devid %d prj %u port %u still attached at api_exit\n",
					     mgmt_dev->mgmt.devid, prj_id,
					     port);
					rcu_assign_pointer(
						entry->inst_port[prj_id][port],
						NULL);
					had_instances = true;
				}
			}
		}
		mutex_unlock(&mango_mgmt_projects_lock);
		if (had_instances)
			synchronize_rcu();

		kfree(entry);
	}

	dev_info(mgmt_dev->dev, "API uninitialized (devid=%d)",
		 mgmt_dev->mgmt.devid);
}

int mango_mgmt_api_module_init(void)
{
	return mango_mgmt_meta_module_init();
}

void mango_mgmt_api_module_exit(void)
{
	mango_mgmt_meta_module_exit();
	xa_destroy(&mango_mgmt_device_xa);
}

/* ===== SoC IP (from NAT) ===== */

int mango_mgmt_get_soc_ip(int devid, __be32 *ip_addr, __be32 *netmask)
{
	struct mango_mgmt_device_entry *entry;
	struct mango_mgmt_mpipe_nat *nat;
	long ext;

	entry = xa_load(&mango_mgmt_device_xa, devid);
	if (!entry || !entry->mgmt_dev)
		return -ENODEV;

	nat = mango_mgmt_mpipe_get_nat(entry->mgmt_dev);
	if (!nat)
		return -ENODEV;

	ext = atomic_long_read(&nat->external);
	if (ext == MANGO_MGMT_IP_PAIR_UNSET)
		return -ENOLINK;

	*ip_addr = nat_soc(ext);
	/* Derive netmask from the longest common prefix between
	 * host_ip and soc_ext — this recovers the user's prefix. */
	{
		__be32 h = nat_host(ext);
		__be32 s = nat_soc(ext);
		u32 diff = ntohl(h) ^ ntohl(s);
		int prefixlen = diff ? (32 - fls(diff)) : 32;

		*netmask = htonl(~0u << (32 - prefixlen));
	}
	return 0;
}
EXPORT_SYMBOL_GPL(mango_mgmt_get_soc_ip);

/* ===== Device lookup utilities ===== */

int mango_mgmt_get_device_count(void)
{
	struct mango_mgmt_device_entry *entry;
	unsigned long index;
	int count = 0;

	xa_for_each (&mango_mgmt_device_xa, index, entry)
		count++;

	return count;
}
EXPORT_SYMBOL_GPL(mango_mgmt_get_device_count);

bool mango_mgmt_device_is_ready(int devid)
{
	struct mango_mgmt_device_entry *entry;
	struct mango_mgmt_dev *mgmt_dev;

	entry = xa_load(&mango_mgmt_device_xa, devid);
	mgmt_dev = entry ? entry->mgmt_dev : NULL;
	if (!mgmt_dev) {
		pr_err("mango_mgmt: Device %d not found\n", devid);
		return false;
	}

	return atomic_read(&mgmt_dev->mgmt.ready) != 0;
}
EXPORT_SYMBOL_GPL(mango_mgmt_device_is_ready);

int mango_mgmt_get_devid(const char *mgmt_addr)
{
	struct mango_mgmt_device_entry *entry;
	unsigned long index;
	const char *dev_mgmt_addr;

	if (!mgmt_addr)
		return -EINVAL;

	xa_for_each (&mango_mgmt_device_xa, index, entry) {
		struct mango_mgmt_dev *mgmt_dev = entry->mgmt_dev;

		if (!mgmt_dev || !mgmt_dev->transport)
			continue;

		/* In SoC, there is only one MGMT device */
		if (mango_mgmt_transport_is_soc_device(mgmt_dev))
			return entry->devid;

		dev_mgmt_addr = mgmt_dev->transport->get_mgmt_addr(mgmt_dev);
		if (dev_mgmt_addr && strcmp(dev_mgmt_addr, mgmt_addr) == 0)
			return entry->devid;
	}

	return -ENODEV;
}
EXPORT_SYMBOL_GPL(mango_mgmt_get_devid);

struct mango_mgmt_dev *mango_mgmt_dev_by_addr(const char *hw_addr)
{
	struct mango_mgmt_device_entry *entry;
	int devid;

	devid = mango_mgmt_get_devid(hw_addr);
	if (devid < 0)
		return NULL;

	entry = xa_load(&mango_mgmt_device_xa, devid);
	return entry ? entry->mgmt_dev : NULL;
}

/* ===== EEPROM access via SMI ===== */

/* The Host-side device @devid names; EEPROM lives behind its SMI. */
static struct mango_mgmt_dev *mango_mgmt_eeprom_dev(int devid)
{
	struct mango_mgmt_device_entry *entry;

	entry = xa_load(&mango_mgmt_device_xa, devid);
	if (!entry || !entry->mgmt_dev)
		return ERR_PTR(-ENODEV);

	if (mango_mgmt_transport_is_soc_device(entry->mgmt_dev))
		return ERR_PTR(-EOPNOTSUPP);

	return entry->mgmt_dev;
}

int mango_mgmt_read_eeprom(int devid, u8 *buf, u16 len, u8 offset)
{
	struct mango_mgmt_dev *mgmt = mango_mgmt_eeprom_dev(devid);

	if (IS_ERR(mgmt))
		return PTR_ERR(mgmt);

	return mango_mgmt_smi_dev_eeprom_read(mgmt, buf, len, offset);
}
EXPORT_SYMBOL_GPL(mango_mgmt_read_eeprom);

int mango_mgmt_write_eeprom(int devid, u8 *buf, u16 len, u8 offset)
{
	struct mango_mgmt_dev *mgmt = mango_mgmt_eeprom_dev(devid);

	if (IS_ERR(mgmt))
		return PTR_ERR(mgmt);

	return mango_mgmt_smi_dev_eeprom_write(mgmt, buf, len, offset);
}
EXPORT_SYMBOL_GPL(mango_mgmt_write_eeprom);

int mango_mgmt_update_eeprom(int devid, u8 offset, u8 mask, u8 bits)
{
	struct mango_mgmt_dev *mgmt = mango_mgmt_eeprom_dev(devid);

	if (IS_ERR(mgmt))
		return PTR_ERR(mgmt);

	return mango_mgmt_smi_dev_eeprom_update(mgmt, offset, mask, bits);
}
EXPORT_SYMBOL_GPL(mango_mgmt_update_eeprom);

/* ===== Project / instance lifecycle =====
 *
 * Writer serialization: mango_mgmt_projects_lock.
 * Publication to dispatcher: rcu_assign_pointer + rcu_read_lock-side read.
 * Teardown waits via synchronize_rcu() before releasing resources. */

int __mango_mgmt_register_project(u32 prj_id,
				  const struct mango_mgmt_proc_desc *ops,
				  unsigned int n_ops, struct module *owner)
{
	struct mango_mgmt_project *p;
	int ret = 0;

	if (prj_id >= MANGO_MGMT_PRJ_MAX || !ops || !n_ops)
		return -EINVAL;

	p = kmalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->ops = ops;
	p->n_ops = n_ops;
	p->owner = owner;

	mutex_lock(&mango_mgmt_projects_lock);
	if (rcu_dereference_protected(
		    mango_mgmt_projects[prj_id],
		    lockdep_is_held(&mango_mgmt_projects_lock))) {
		ret = -EEXIST;
		goto out;
	}
	rcu_assign_pointer(mango_mgmt_projects[prj_id], p);
	pr_info("mango_mgmt: project %u registered (%u ops)\n", prj_id, n_ops);
out:
	mutex_unlock(&mango_mgmt_projects_lock);
	if (ret)
		kfree(p);
	return ret;
}
EXPORT_SYMBOL_GPL(__mango_mgmt_register_project);

void mango_mgmt_unregister_project(u32 prj_id)
{
	struct mango_mgmt_project *p;
	struct mango_mgmt_device_entry *entry;
	unsigned long index;

	if (prj_id >= MANGO_MGMT_PRJ_MAX)
		return;

	mutex_lock(&mango_mgmt_projects_lock);

	p = rcu_dereference_protected(
		mango_mgmt_projects[prj_id],
		lockdep_is_held(&mango_mgmt_projects_lock));
	if (!p) {
		mutex_unlock(&mango_mgmt_projects_lock);
		return;
	}

	/* Auto-detach any instances the aux module neglected to release.
	 * Each such leak is an aux-module bug -- WARN so it shows up. */
	xa_for_each (&mango_mgmt_device_xa, index, entry) {
		u32 port;

		if (rcu_dereference_protected(
			    entry->inst_devid[prj_id],
			    lockdep_is_held(&mango_mgmt_projects_lock))) {
			WARN(1,
			     "mango_mgmt: prj %u devid %d still attached at unregister\n",
			     prj_id, entry->devid);
			rcu_assign_pointer(entry->inst_devid[prj_id], NULL);
		}
		for (port = 0; port < MANGO_MGMT_MAX_PORTS; port++) {
			if (rcu_dereference_protected(
				    entry->inst_port[prj_id][port],
				    lockdep_is_held(
					    &mango_mgmt_projects_lock))) {
				WARN(1,
				     "mango_mgmt: prj %u devid %d port %u still attached at unregister\n",
				     prj_id, entry->devid, port);
				rcu_assign_pointer(
					entry->inst_port[prj_id][port], NULL);
			}
		}
	}

	rcu_assign_pointer(mango_mgmt_projects[prj_id], NULL);
	mutex_unlock(&mango_mgmt_projects_lock);

	/* Drain any dispatcher that read a non-NULL pointer before the
	 * writes above became visible. */
	synchronize_rcu();
	kfree(p);
	pr_info("mango_mgmt: project %u unregistered\n", prj_id);
}
EXPORT_SYMBOL_GPL(mango_mgmt_unregister_project);

int mango_mgmt_attach_devid(u32 prj_id, int devid, void *priv)
{
	struct mango_mgmt_device_entry *entry;
	int ret = 0;

	if (prj_id >= MANGO_MGMT_PRJ_MAX || !priv)
		return -EINVAL;

	mutex_lock(&mango_mgmt_projects_lock);
	entry = xa_load(&mango_mgmt_device_xa, devid);
	if (!entry) {
		ret = -ENODEV;
		goto out;
	}
	if (rcu_dereference_protected(
		    entry->inst_devid[prj_id],
		    lockdep_is_held(&mango_mgmt_projects_lock))) {
		ret = -EEXIST;
		goto out;
	}
	rcu_assign_pointer(entry->inst_devid[prj_id], priv);
out:
	mutex_unlock(&mango_mgmt_projects_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mango_mgmt_attach_devid);

void mango_mgmt_detach_devid(u32 prj_id, int devid)
{
	struct mango_mgmt_device_entry *entry;
	bool cleared = false;

	if (prj_id >= MANGO_MGMT_PRJ_MAX)
		return;

	mutex_lock(&mango_mgmt_projects_lock);
	entry = xa_load(&mango_mgmt_device_xa, devid);
	if (entry && rcu_dereference_protected(
			     entry->inst_devid[prj_id],
			     lockdep_is_held(&mango_mgmt_projects_lock))) {
		rcu_assign_pointer(entry->inst_devid[prj_id], NULL);
		cleared = true;
	}
	mutex_unlock(&mango_mgmt_projects_lock);

	if (cleared)
		synchronize_rcu();
}
EXPORT_SYMBOL_GPL(mango_mgmt_detach_devid);

int mango_mgmt_attach_port(u32 prj_id, int devid, u32 port, void *priv)
{
	struct mango_mgmt_device_entry *entry;
	int ret = 0;

	if (prj_id >= MANGO_MGMT_PRJ_MAX || port >= MANGO_MGMT_MAX_PORTS ||
	    !priv)
		return -EINVAL;

	mutex_lock(&mango_mgmt_projects_lock);
	entry = xa_load(&mango_mgmt_device_xa, devid);
	if (!entry) {
		ret = -ENODEV;
		goto out;
	}
	if (rcu_dereference_protected(
		    entry->inst_port[prj_id][port],
		    lockdep_is_held(&mango_mgmt_projects_lock))) {
		ret = -EEXIST;
		goto out;
	}
	rcu_assign_pointer(entry->inst_port[prj_id][port], priv);
out:
	mutex_unlock(&mango_mgmt_projects_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mango_mgmt_attach_port);

void mango_mgmt_detach_port(u32 prj_id, int devid, u32 port)
{
	struct mango_mgmt_device_entry *entry;
	bool cleared = false;

	if (prj_id >= MANGO_MGMT_PRJ_MAX || port >= MANGO_MGMT_MAX_PORTS)
		return;

	mutex_lock(&mango_mgmt_projects_lock);
	entry = xa_load(&mango_mgmt_device_xa, devid);
	if (entry && rcu_dereference_protected(
			     entry->inst_port[prj_id][port],
			     lockdep_is_held(&mango_mgmt_projects_lock))) {
		rcu_assign_pointer(entry->inst_port[prj_id][port], NULL);
		cleared = true;
	}
	mutex_unlock(&mango_mgmt_projects_lock);

	if (cleared)
		synchronize_rcu();
}
EXPORT_SYMBOL_GPL(mango_mgmt_detach_port);
