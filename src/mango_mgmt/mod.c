// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include <linux/module.h>

#include "smi_build_id.h" /* generated: this build, as git names it */
#include "sysfs.h"
#include "monitor/monitor.h"
#include "transport/transport.h"
#include "frontend.h"
#include "term/socket_tty.h"
#include "smi/smi_api.h"
#include "smi_glue.h"
#include "api/api.h"
#include "mpipe/netdev.h"
#include "mango_dev.h"

int mango_mgmt_irq_mode = -1;
module_param_named(irq_mode, mango_mgmt_irq_mode, int, 0444);
MODULE_PARM_DESC(irq_mode,
		 "SoC MPIPE IRQ mode: -1=auto, 0=none, 2=interrupt, 3=polling");

unsigned short mango_mgmt_rpc_port = 1023;
module_param_named(rpc_port, mango_mgmt_rpc_port, ushort, 0444);
MODULE_PARM_DESC(rpc_port, "RPC server TCP port (default: 1023)");

unsigned short mango_mgmt_term_port = 1022;
module_param_named(term_port, mango_mgmt_term_port, ushort, 0444);
MODULE_PARM_DESC(term_port, "Terminal console TCP port (default: 1022)");

#define MANGO_MGMT_MOD_AUTHOR "MangoBoost, Inc."
#define MANGO_MGMT_MOD_DESC "MangoBoost Management Module"
#define MANGO_MGMT_MOD_LICENSE "GPL v2"
/* The build's commit in the version, so `modinfo` says which source is loaded
 * -- the same two facts the card's log gets over the wire. */
#define MANGO_MGMT_MOD_VERSION MANGO_DRV_VERSION "+" MANGO_BUILD_ID_STR

static int __init mango_mgmt_mod_init(void)
{
	int err;

	pr_debug("MangoBoost Management Module: Initializing...\n");

	/* Bring up the mango_dev presence layer first: its class + bus notifiers
	 * must exist before mango_core (which loads after us) registers members. */
	err = mango_dev_init(&mango_mgmt_smi_card_ops);
	if (err < 0) {
		pr_err("MangoBoost Management Module: mango_dev init failed: %d\n",
		       err);
		return err;
	}

	pr_debug(
		"MangoBoost Management Module: Registering netdev notifiers...\n");
	err = mango_mgmt_mpipe_netdev_init_notifiers();
	if (err < 0) {
		pr_err("MangoBoost Management Module: netdev registration failed: %d\n",
		       err);
		goto exit_mango_dev;
	}

	pr_debug("MangoBoost Management Module: Initializing API...\n");
	err = mango_mgmt_api_module_init();
	if (err < 0) {
		pr_err("MangoBoost Management Module: API initialization failed: %d\n",
		       err);
		goto exit_netdev_notifiers;
	}

	pr_debug("MangoBoost Management Module: Initializing socket_tty...\n");
	err = mango_mgmt_socket_tty_init();
	if (err < 0) {
		pr_err("MangoBoost Management Module: socket_tty initialization failed: %d\n",
		       err);
		goto exit_api;
	}

	pr_debug("MangoBoost Management Module: Registering sysfs...\n");
	err = mango_mgmt_sysfs_register();
	if (err < 0) {
		pr_err("MangoBoost Management Module: sysfs registration failed: %d\n",
		       err);
		goto exit_socket_tty;
	}

	pr_debug("MangoBoost Management Module: Registering monitor...\n");
	err = mango_mgmt_monitor_register();
	if (err < 0) {
		pr_err("MangoBoost Management Module: monitor registration failed: %d\n",
		       err);
		goto exit_sysfs;
	}

	/* SMI class, cdev and FAL before any probe can attach SMI. */
	pr_debug("MangoBoost Management Module: Initializing mango_smi...\n");
	err = mango_mgmt_smi_init();
	if (err < 0) {
		pr_err("MangoBoost Management Module: mango_smi initialization failed: %d\n",
		       err);
		goto exit_monitor;
	}

	pr_debug("MangoBoost Management Module: Registering transport...\n");
	err = mango_mgmt_transport_register();
	if (err < 0) {
		pr_err("MangoBoost Management Module: transport registration failed: %d\n",
		       err);
		goto exit_smi;
	}

	pr_debug("MangoBoost Management Module: Registration completed\n");
	return 0;

exit_smi:
	mango_mgmt_smi_exit();
exit_monitor:
	mango_mgmt_monitor_unregister();
exit_sysfs:
	mango_mgmt_sysfs_unregister();
exit_socket_tty:
	mango_mgmt_socket_tty_exit();
exit_api:
	mango_mgmt_api_module_exit();
exit_netdev_notifiers:
	mango_mgmt_mpipe_netdev_exit_notifiers();
exit_mango_dev:
	mango_dev_exit();
	return err;
}

static void __exit mango_mgmt_mod_exit(void)
{
	mango_mgmt_transport_unregister();
	mango_mgmt_smi_exit();
	mango_mgmt_monitor_unregister();
	mango_mgmt_sysfs_unregister();
	mango_mgmt_socket_tty_exit();
	mango_mgmt_api_module_exit();
	mango_mgmt_mpipe_netdev_exit_notifiers();
	mango_dev_exit();
}

MODULE_AUTHOR(MANGO_MGMT_MOD_AUTHOR);
MODULE_DESCRIPTION(MANGO_MGMT_MOD_DESC);
MODULE_LICENSE(MANGO_MGMT_MOD_LICENSE);
MODULE_VERSION(MANGO_MGMT_MOD_VERSION);

module_init(mango_mgmt_mod_init);
module_exit(mango_mgmt_mod_exit);
