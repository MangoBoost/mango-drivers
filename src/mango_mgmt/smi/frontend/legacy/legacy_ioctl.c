// SPDX-License-Identifier: GPL-2.0-only
/*
 * legacy_ioctl.c - The legacy card node: UAPI v1's ioctls.
 *
 * Copyright (c) 2023 - 2025 Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include <linux/build_bug.h> /* static_assert() */
#include <linux/capability.h> /* capable() */
#include <linux/err.h> /* ERR_PTR() */
#include <linux/errno.h> /* error codes */
#include <linux/fs.h> /* file_operations */
#include <linux/kernel.h> /* container_of, u64_to_user_ptr() */
#include <linux/module.h> /* THIS_MODULE */
#include <linux/slab.h> /* kzalloc... */
#include <linux/string.h> /* string funcs */
#include <linux/types.h>
#include <linux/uaccess.h> /* copy_to/from_user() */
#include <linux/vmalloc.h>

#include "core/smi_eeprom.h"
#include "core/smi_flash.h"
#include "core/smi_sensor.h"

#include "legacy_card.h"
#include "legacy_debug.h"
#include "legacy_hwmon.h"
#include "legacy_ioctl.h"
#include "legacy_qsfp.h"
#include "smi_hwmon.h"
#include "contract/smi/smi_abi_v2.h" /* the shell verb's own generation states the card's line */
#include "smi_ioctl.h"
#include "smi_log.h"
#include "smi_top.h"

/* The UAPI restates the card's shell line and the burn service's image bound,
 * which a leaf UAPI cannot include: checked here, where this driver compiles
 * both. */
static_assert(SMI_IOC_SHELL_MAX_LINE == SMI_SHELL_LINE_BYTES - 1,
	      "the UAPI line excludes the terminator the card's buffer spends");
static_assert(SMI_IOC_DATA_PAYLOAD_MAX_SZ == SMI_FLASH_MAX_LEN,
	      "the UAPI's image bound is the one the burn service stages");

/*
 * Open a device file - this increments the pf_dev refcount.
 */
static int smi_cdev_open(struct inode *inode, struct file *filp)
{
	if (!inode || !filp)
		return -EINVAL;

	filp->private_data =
		get_pf_dev_entry((void *)inode, PF_DEV_CACHE_INODE);

	if (!filp->private_data)
		return -ENODEV;

	return 0;
}

/*
 * Close a device file - this decrements the pf_dev refcount.
 */
static int smi_cdev_release(struct inode *inode, struct file *filp)
{
	if (!inode || !filp)
		return -EINVAL;

	if (filp->private_data)
		put_pf_dev_entry((struct pf_dev_struct *)filp->private_data);

	return 0;
}

static int ioctl_set_sensor_refresh(struct pf_dev_struct *pf_dev,
				    unsigned long arg)
{
	/* No range check here: the setter refuses a window it cannot hold. */
	return smi_hwmon_set_interval(pf_dev, (long)arg);
}

/* UAPI v1 names the partition tables by a partition number of its own. */
static int ioctl_download_pdi(struct pf_dev_struct *pf_dev, unsigned long arg)
{
	struct smi_legacy_card *card = smi_legacy_card(pf_dev);
	struct smi_ioc_data_payload data = { 0 };
	struct smi_flash_op op = { 0 };
	void *fence;
	int ret;

	if (copy_from_user(&data, (struct smi_ioc_data_payload *)arg,
			   sizeof(data)))
		return -EFAULT;

	op.kind = (data.partition == SMI_IOC_FPT_UPDATE_MAGIC)
			  ? SMI_FLASH_FPT
			  : SMI_FLASH_IMAGE;
	op.device = data.boot_device;
	op.partition = data.partition;
	op.image = (const void __user *)data.addr;
	op.len = data.size;
	op.progress_efd = data.efd;
	if (op.kind == SMI_FLASH_IMAGE)
		return smi_flash_run(pf_dev, &op);

	/* Read the rewritten tables again under the lock and the fence: a later
	 * rewrite's copy cannot land first, nor a reset fall between. */
	if (mutex_lock_interruptible(&card->fpt_lock))
		return -ERESTARTSYS;
	fence = smi_reset_fence(pf_dev);
	if (IS_ERR(fence)) {
		ret = PTR_ERR(fence);
		goto unlock;
	}
	ret = smi_flash_run(pf_dev, &op);
	if (!ret) {
		ret = smi_fpt_load(pf_dev, &card->fpt);
		if (ret)
			SMI_DEV_ERR(pf_dev->dev, "Could not update FPT SDR");
		else
			SMI_DEV_INFO(pf_dev->dev, "Updated FPT SDR");
	}
	smi_reset_unfence(pf_dev, fence);
unlock:
	mutex_unlock(&card->fpt_lock);
	return ret;
}

static int ioctl_device_boot(struct pf_dev_struct *pf_dev, unsigned long arg)
{
	struct smi_ioc_data_payload data = { 0 };
	struct smi_flash_op op = { .kind = SMI_FLASH_BOOT };

	if (copy_from_user(&data, (struct smi_ioc_data_payload *)arg,
			   sizeof(data)))
		return -EFAULT;

	op.partition = data.partition;
	return smi_flash_run(pf_dev, &op);
}

int smi_legacy_copy_fits(struct device *dev, struct smi_fpt_copy *f,
			 const struct smi_flash_op *op)
{
	struct smi_partition src = { 0 };
	struct smi_partition dst = { 0 };

	if (smi_fpt_partition(f, op->device, op->partition, &src) ||
	    smi_fpt_partition(f, op->dst_device, op->dst_partition, &dst)) {
		SMI_DEV_ERR(dev, "Partition not found");
		return -EINVAL;
	}
	if (dst.partition_size < src.partition_size) {
		SMI_DEV_ERR(dev, "Destination partition is too small for copy");
		return -EINVAL;
	}
	return 0;
}

static int ioctl_copy_partition(struct pf_dev_struct *pf_dev, unsigned long arg)
{
	struct smi_legacy_card *card = smi_legacy_card(pf_dev);
	struct smi_ioc_data_payload data = { 0 };
	struct smi_flash_op op = { .kind = SMI_FLASH_COPY };
	int ret;

	if (copy_from_user(&data, (struct smi_ioc_data_payload *)arg,
			   sizeof(data)))
		return -EFAULT;

	op.device = data.src_device;
	op.partition = data.src_part;
	op.dst_device = data.dest_device;
	op.dst_partition = data.dest_part;

	/* The tables hold still from the check until the copy is done. */
	if (mutex_lock_interruptible(&card->fpt_lock))
		return -ERESTARTSYS;
	ret = smi_legacy_copy_fits(pf_dev->dev, &card->fpt, &op);
	if (!ret)
		ret = smi_flash_run(pf_dev, &op);
	mutex_unlock(&card->fpt_lock);
	return ret;
}

static int ioctl_get_sensor_value(struct pf_dev_struct *pf_dev,
				  unsigned long arg)
{
	int ret;
	struct smi_ioc_sensor_value data = { 0 };
	enum hwmon_sensor_types hwmon_type = 0;
	const char *status = NULL;

	if (copy_from_user(&data, (struct smi_ioc_sensor_value *)arg,
			   sizeof(data)))
		return -EFAULT;

	if (data.sensor_type == IOC_SENSOR_TYPE_QSFP) {
		uint8_t qsfp_status;

		/* Not a hwmon sensor: raw record from the QSFP repo. */
		ret = read_qsfp_sensor(pf_dev, data.hwmon_channel, &data.val,
				       &qsfp_status, &data.fresh, NULL);
		if (!ret)
			snprintf(data.status, sizeof(data.status), "%s\n",
				 smi_legacy_status_name(qsfp_status));
	} else {
		switch (data.sensor_type) {
		case IOC_SENSOR_TYPE_TEMP:
			hwmon_type = hwmon_temp;
			break;
		case IOC_SENSOR_TYPE_POWER:
			hwmon_type = hwmon_power;
			break;
		case IOC_SENSOR_TYPE_CURRENT:
			hwmon_type = hwmon_curr;
			break;
		case IOC_SENSOR_TYPE_VOLTAGE:
			hwmon_type = hwmon_in;
			break;
		default:
			return -EINVAL;
		}

		/* The reading itself: this asks for a value, not for the file
		 * some surface would put it in. */
		ret = read_sensor_val(pf_dev, hwmon_type, SENSOR_ATTR_INSTANT,
				      data.hwmon_channel, &data.val, &status,
				      &data.fresh);
		/* No newline: sysfs adds its own on the other path, so the two
		 * spellings only match without it. */
		if (status)
			snprintf(data.status, sizeof(data.status), "%s",
				 status);
	}

	if (ret)
		return ret;

	if (copy_to_user((struct smi_ioc_sensor_value *)arg, &data,
			 sizeof(data)))
		return -EFAULT;

	return 0;
}

static int ioctl_get_fpt_hdr(struct pf_dev_struct *pf_dev, unsigned long arg)
{
	int ret;
	struct smi_ioc_fpt_hdr_value data = { 0 };
	struct smi_fpt_header hdr = { 0 };

	if (copy_from_user(&data, (struct smi_ioc_fpt_hdr_value *)arg,
			   sizeof(data)))
		return -EFAULT;

	ret = smi_fpt_header(&smi_legacy_card(pf_dev)->fpt, data.boot_device,
			     &hdr);
	if (!ret) {
		data.version = hdr.version;
		data.hdr_size = hdr.header_size;
		data.entry_size = hdr.entry_size;
		data.num_entries = hdr.num_entries;
		if (copy_to_user((struct smi_ioc_fpt_hdr_value *)arg, &data,
				 sizeof(data)))
			return -EFAULT;
	}
	return ret;
}

static int ioctl_get_fpt_partition(struct pf_dev_struct *pf_dev,
				   unsigned long arg)
{
	int ret;
	struct smi_ioc_fpt_partition_value data = { 0 };
	struct smi_partition partition = { 0 };

	if (copy_from_user(&data, (struct smi_ioc_fpt_partition_value *)arg,
			   sizeof(data)))
		return -EFAULT;

	ret = smi_fpt_partition(&smi_legacy_card(pf_dev)->fpt, data.boot_device,
				data.partition, &partition);
	if (!ret) {
		data.type = partition.type;
		data.base_addr = partition.base_addr;
		data.partition_size = partition.partition_size;
		if (copy_to_user((struct smi_ioc_fpt_partition_value *)arg,
				 &data, sizeof(data)))
			return -EFAULT;
	}
	return ret;
}

static int ioctl_read_eeprom(struct pf_dev_struct *pf_dev, unsigned long arg)
{
	int ret;
	struct smi_ioc_eeprom_payload data = { 0 };
	uint8_t *buf = NULL;

	if (copy_from_user(&data, (struct smi_ioc_eeprom_payload *)arg,
			   sizeof(data)))
		return -EFAULT;

	if ((data.len <= 0) || (data.len > SMI_IOC_EEPROM_MAX_XFER) ||
	    (data.addr == 0))
		return -EINVAL;

	buf = vzalloc(data.len * sizeof(uint8_t));
	if (!buf)
		return -ENOMEM;

	ret = smi_pf_dev_eeprom_read(pf_dev, buf, data.len, data.offset);
	if (!ret) {
		if (copy_to_user((uint8_t *)data.addr, buf,
				 data.len * sizeof(uint8_t)))
			ret = -EFAULT;
	}
	vfree(buf);
	return ret;
}

static int ioctl_write_eeprom(struct pf_dev_struct *pf_dev, unsigned long arg)
{
	int ret;
	struct smi_ioc_eeprom_payload data = { 0 };
	uint8_t *buf = NULL;

	if (copy_from_user(&data, (struct smi_ioc_eeprom_payload *)arg,
			   sizeof(data)))
		return -EFAULT;

	if ((data.len <= 0) || (data.len > SMI_IOC_EEPROM_MAX_XFER) ||
	    (data.addr == 0))
		return -EINVAL;

	buf = vzalloc(data.len * sizeof(uint8_t));
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, (uint8_t *)data.addr,
			   data.len * sizeof(uint8_t))) {
		ret = -EFAULT;
		goto out;
	}

	ret = smi_pf_dev_eeprom_write(pf_dev, buf, data.len, data.offset);

out:
	vfree(buf);
	return ret;
}

static int ioctl_app_setup(struct pf_dev_struct *pf_dev, unsigned long arg)
{
	switch ((enum smi_ioc_app_setup)arg) {
	case IOC_APP_SETUP_REGISTER:
		return smi_legacy_app_add(pf_dev, get_current());
	case IOC_APP_SETUP_DEREGISTER:
		return smi_legacy_app_del(pf_dev, get_current());
	default:
		return -EINVAL;
	}
}

static int ioctl_read_module(struct pf_dev_struct *pf_dev, unsigned long arg)
{
	int ret;
	struct smi_ioc_module_payload data = { 0 };
	uint8_t *buf = NULL;

	if (copy_from_user(&data, (struct smi_ioc_module_payload *)arg,
			   sizeof(data)))
		return -EFAULT;

	if ((data.len <= 0) || (data.len > SMI_IOC_MODULE_MAX_XFER) ||
	    (data.addr == 0))
		return -EINVAL;

	buf = vzalloc(data.len * sizeof(uint8_t));
	if (!buf)
		return -ENOMEM;

	ret = smi_pf_dev_module_read(pf_dev, data.device_id, data.page,
				     data.offset, buf, data.len);
	if (!ret) {
		if (copy_to_user((uint8_t *)data.addr, buf,
				 data.len * sizeof(uint8_t)))
			ret = -EFAULT;
	}
	vfree(buf);
	return ret;
}

static int ioctl_write_module(struct pf_dev_struct *pf_dev, unsigned long arg)
{
	int ret;
	struct smi_ioc_module_payload data = { 0 };
	uint8_t *buf = NULL;

	if (copy_from_user(&data, (struct smi_ioc_module_payload *)arg,
			   sizeof(data)))
		return -EFAULT;

	if ((data.len <= 0) || (data.len > SMI_IOC_MODULE_MAX_XFER) ||
	    (data.addr == 0))
		return -EINVAL;

	buf = vzalloc(data.len * sizeof(uint8_t));
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, (uint8_t *)data.addr,
			   data.len * sizeof(uint8_t))) {
		ret = -EFAULT;
		goto out;
	}

	ret = smi_pf_dev_module_write(pf_dev, data.device_id, data.page,
				      data.offset, buf, data.len);

out:
	vfree(buf);
	return ret;
}

static int ioctl_debug_verbosity(struct pf_dev_struct *pf_dev,
				 unsigned long arg)
{
	/* One byte on the wire, so a wider argument is refused rather than
	 * truncated onto a level nobody asked for; the card owns the range. */
	if (arg > U8_MAX)
		return -EINVAL;

	return smi_debug_set_verbosity(pf_dev, (uint8_t)arg);
}

static int ioctl_led(struct pf_dev_struct *pf_dev, unsigned long arg)
{
	struct smi_ioc_led_payload data = { 0 };

	if (copy_from_user(&data, (struct smi_ioc_led_payload *)arg,
			   sizeof(data)))
		return -EFAULT;

	if (data.op >= 2 && data.op <= 4 && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (data.op >= 1 && data.op <= 3 &&
	    (data.timeout_s < 1 || data.timeout_s > SMI_IOC_LED_MAX_TIMEOUT_S))
		return -EINVAL;

	return smi_debug_led(pf_dev, (uint8_t *)&data, sizeof(data));
}

/* IOCTL Handler Table */
struct smi_ioctl_handler {
	unsigned int cmd;
	int (*handler)(struct pf_dev_struct *pf_dev, unsigned long arg);
	u32 valid_states;
	/* A read, an app registering itself, or LED, whose handler gates its own
	 * force and release; the rest need CAP_SYS_ADMIN, as does QSFP_RW's Read:
	 * it consumes a module's clear-on-read flags. */
	bool any_user;
};

#define STATE_MASK(s) (1 << (s))
#define STATE_ALL_BUT_INIT_SHUTDOWN                                            \
	(STATE_MASK(PF_DEV_STATE_READY) |                                      \
	 STATE_MASK(PF_DEV_STATE_MISSING_INFO) |                               \
	 STATE_MASK(PF_DEV_STATE_NO_SMC) |                                     \
	 STATE_MASK(PF_DEV_STATE_INIT_ERROR))

/* One shell line, staged and answered in the same slice. The output buffer is
 * the slice: the line goes in at its head and the answer overwrites it, which
 * is what the verb's InOut column means. */
static int ioctl_shell_exec(struct pf_dev_struct *pf_dev, unsigned long arg)
{
	struct smi_ioc_shell_payload data = { 0 };
	uint8_t *buf = NULL;
	int ret;

	if (copy_from_user(&data, (struct smi_ioc_shell_payload *)arg,
			   sizeof(data)))
		return -EFAULT;

	if (!data.cmd || !data.out || !data.cmd_len || !data.out_cap ||
	    data.cmd_len > SMI_IOC_SHELL_MAX_LINE ||
	    data.out_cap > SMI_IOC_SHELL_MAX_OUT || data.cmd_len > data.out_cap)
		return -EINVAL;

	/* One level deeper than its siblings reach, so the check is here
	 * rather than inside: the answer buffer is up to 64 KiB. */
	if (!pf_dev->facts.present)
		return -ENODEV;

	buf = vzalloc(data.out_cap);
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, u64_to_user_ptr(data.cmd), data.cmd_len)) {
		vfree(buf);
		return -EFAULT;
	}

	ret = smi_debug_shell_exec(pf_dev, buf, data.out_cap, data.cmd_len,
				   &data.out_len, &data.dropped);
	if (!ret) {
		if (copy_to_user(u64_to_user_ptr(data.out), buf,
				 data.out_len) ||
		    copy_to_user((struct smi_ioc_shell_payload *)arg, &data,
				 sizeof(data)))
			ret = -EFAULT;
	}

	vfree(buf);
	return ret;
}

/* One line per request, after the answer, so a refusal never reads as applied:
 * - debug: a read or a CDB payload page, inert until the command page names it
 * - info, ratelimited: the CDB command page, sent once per firmware block
 * - info: any other change
 */
#define SMI_QSFP_AUDIT_FMT                                                     \
	"qsfp%u: op %s%s bank %u page %u offset %u len %u (epoch %u) -> %d (val %u)"

static void qsfp_audit(struct device *dev, const struct smi_ioc_qsfp_payload *d,
		       const char *content, int ret)
{
	char num[16];
	const char *op = smi_qsfp_op_word(d->op, num, sizeof(num));

#define SMI_QSFP_AUDIT(log)                                                    \
	log(dev, SMI_QSFP_AUDIT_FMT, d->cage, op, content, d->bank, d->page,   \
	    d->offset, d->len, d->epoch, ret, d->val)

	if ((d->op == SmiQsfpOp_Read) ||
	    ((d->op == SmiQsfpOp_CdbWrite) && (d->page != SMI_QSFP_CDB_PAGE)))
		SMI_QSFP_AUDIT(SMI_DEV_DBG);
	else if (d->op == SmiQsfpOp_CdbWrite)
		SMI_QSFP_AUDIT(SMI_DEV_INFO_RATELIMITED);
	else
		SMI_QSFP_AUDIT(SMI_DEV_INFO);
#undef SMI_QSFP_AUDIT
}

/* One window of one cage, privileged: a write reaches control bytes, a read or
 * ack clears flags others await; the card judges all but the host buffer. */
static int ioctl_qsfp_rw(struct pf_dev_struct *pf_dev, unsigned long arg)
{
	struct smi_ioc_qsfp_payload data = { 0 };
	struct smi_qsfp_rw_arg req = { 0 };
	uint8_t buf[SMI_IOC_QSFP_MAX_XFER] = { 0 };
	char content[32] = "";
	char num[16];
	uint32_t used = 0;
	int ret;

	if (copy_from_user(&data, (struct smi_ioc_qsfp_payload *)arg,
			   sizeof(data)))
		return -EFAULT;
	data.val = 0; /* out only: an unanswered request returns 0 */

	if (!data.len || (data.len > SMI_IOC_QSFP_MAX_XFER) || !data.addr)
		return -EINVAL;

	/* Staged whatever the op: the answer says how much of it came back */
	if (copy_from_user(buf, u64_to_user_ptr(data.addr), data.len))
		return -EFAULT;

	req.epoch = data.epoch;
	req.control_gen = data.control_gen;
	req.cage = data.cage;
	req.bank = data.bank;
	req.page = data.page;
	req.offset = data.offset;
	req.len = data.len;
	req.op = data.op;

	/* Audited before the card may answer into buf: Power's policy byte and
	 * the command id opening a CDB command page. A Write's bytes are not:
	 * they may be a module password. */
	if (data.op == SmiQsfpOp_Power)
		scnprintf(content, sizeof(content), " power %s",
			  smi_qsfp_power_word(buf[0], num, sizeof(num)));
	else if ((data.op == SmiQsfpOp_CdbWrite) &&
		 (data.page == SMI_QSFP_CDB_PAGE) && (data.len >= 2))
		scnprintf(content, sizeof(content), " cmd %02x%02x", buf[0],
			  buf[1]);

	ret = smi_qsfp_rw(pf_dev, &req, buf, &data.val, &used);
	qsfp_audit(pf_dev->dev, &data, content, ret);

	/* The answer's value is what a refusal is retried on, so it goes back
	 * whether the request was served or refused. */
	if (copy_to_user((struct smi_ioc_qsfp_payload *)arg, &data,
			 sizeof(data)))
		return -EFAULT;
	if (used && copy_to_user(u64_to_user_ptr(data.addr), buf, used))
		return -EFAULT;

	return ret;
}

static const struct smi_ioctl_handler ioctl_handlers[] = {
	{ SMI_IOC_SET_SENSOR_REFRESH, ioctl_set_sensor_refresh,
	  SMI_PF_SERVING_STATES },
	{ SMI_IOC_DOWNLOAD_PDI, ioctl_download_pdi, SMI_PF_SERVING_STATES },
	{ SMI_IOC_DEVICE_BOOT, ioctl_device_boot, SMI_PF_SERVING_STATES },
	{ SMI_IOC_COPY_PARTITION, ioctl_copy_partition, SMI_PF_SERVING_STATES },
	{ SMI_IOC_GET_SENSOR_VALUE, ioctl_get_sensor_value,
	  SMI_PF_SERVING_STATES, true },
	{ SMI_IOC_GET_FPT_HDR, ioctl_get_fpt_hdr, SMI_PF_SERVING_STATES, true },
	{ SMI_IOC_GET_FPT_PARTITION, ioctl_get_fpt_partition,
	  SMI_PF_SERVING_STATES, true },
	{ SMI_IOC_READ_EEPROM, ioctl_read_eeprom, SMI_PF_SERVING_STATES, true },
	{ SMI_IOC_WRITE_EEPROM, ioctl_write_eeprom, SMI_PF_SERVING_STATES },
	{ SMI_IOC_APP_SETUP, ioctl_app_setup, STATE_ALL_BUT_INIT_SHUTDOWN,
	  true },
	{ SMI_IOC_READ_MODULE, ioctl_read_module, SMI_PF_SERVING_STATES, true },
	{ SMI_IOC_WRITE_MODULE, ioctl_write_module, SMI_PF_SERVING_STATES },
	{ SMI_IOC_DEBUG_VERBOSITY, ioctl_debug_verbosity,
	  SMI_PF_SERVING_STATES },
	{ SMI_IOC_SHELL_EXEC, ioctl_shell_exec, SMI_PF_SERVING_STATES },
	{ SMI_IOC_LED, ioctl_led, SMI_PF_SERVING_STATES, true },
	{ SMI_IOC_QSFP_RW, ioctl_qsfp_rw, SMI_PF_SERVING_STATES },
};

/* The node is 0666: CAP_SYS_ADMIN is asked here, only of a verb that needs it.
 * A PF outside the verb's states is -ENXIO, which nothing else returns. */
const struct smi_ioctl_handler *smi_ioctl_gate(unsigned int cmd, int state,
					       bool (*admin)(void))
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ioctl_handlers); i++) {
		const struct smi_ioctl_handler *h = &ioctl_handlers[i];

		if (h->cmd != cmd)
			continue;
		if (!h->any_user && !admin())
			return ERR_PTR(-EPERM);
		if (!(h->valid_states & STATE_MASK(state)))
			return ERR_PTR(-ENXIO);
		return h;
	}

	return ERR_PTR(-ENOTTY);
}

static bool caller_is_admin(void)
{
	return capable(CAP_SYS_ADMIN);
}

/*
 * This function will be called when we use IOCTL with command on the Device file
 */
static long smi_cdev_ioctl(struct file *filp, unsigned int cmd,
			   unsigned long arg)
{
	struct pf_dev_struct *pf_dev = NULL;
	const struct smi_ioctl_handler *handler = NULL;

	if (!filp)
		return -EINVAL;

	/* Check for valid magic and type before proceeding */
	if (_IOC_TYPE(cmd) != SMI_IOC_MAGIC)
		return -ENOTTY;
	if (_IOC_NR(cmd) > SMI_IOC_MAX)
		return -ENOTTY;

	if (!access_ok((void __user *)arg, _IOC_SIZE(cmd)))
		return -ENOTTY;

	pf_dev = filp->private_data;
	if (!pf_dev) {
		PR_ERR("smi_cdev_ioctl: unable to find card");
		return -ENODEV;
	}

	handler = smi_ioctl_gate(cmd, pf_dev->state, caller_is_admin);
	if (IS_ERR(handler))
		return PTR_ERR(handler);

	return handler->handler(pf_dev, arg);
}

const struct file_operations smi_legacy_fops = {
	.owner = THIS_MODULE,
	.open = smi_cdev_open,
	.release = smi_cdev_release,
	.unlocked_ioctl = smi_cdev_ioctl,
};
