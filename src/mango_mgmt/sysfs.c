// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include "sysfs.h"
#include "cdev.h"
#include "monitor/monitor.h"
#include "transport/transport.h"
#include "mpipe/netdev.h"
#include "mpipe/hw.h"

#include <linux/if_ether.h>
#include <linux/module.h>
#include <linux/idr.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/netdevice.h>
#include <linux/inet.h>

#include <mango_compat/device_class.h>

#define MANGO_MGMT_CDEV_MAX (256)
#define MANGO_MGMT_CDEV_NAME ("mango-mgmt")

static DEFINE_IDA(mango_mgmt_cdev_ida);
static dev_t mango_mgmt_cdev_devt;
static struct class *mango_mgmt_cdev_class;

/*
 * Read one field of one sensor from the platform monitor. The monitor reports a
 * whole record now, so the four attributes below differ only in which sensor
 * and which field they pick.
 */
static ssize_t show_monitor_field(struct mango_mgmt_dev *mgmt, char *buf,
				  enum mango_mgmt_monitor_sensor sensor,
				  bool max, const char *unit, u64 divisor)
{
	struct mango_mgmt_sensor_reading reading;
	u64 value;
	int ret;

	if (!mgmt)
		return -EIO;

	if (!mgmt->monitor) {
		dev_err(mgmt->dev, "No available monitor");
		return -EIO;
	}

	if (!mgmt->monitor->get_sensor)
		return -EOPNOTSUPP;

	ret = mgmt->monitor->get_sensor(mgmt, sensor, &reading);
	if (ret)
		return ret;

	value = (u64)(max ? reading.max : reading.avg);

	return snprintf(buf, PAGE_SIZE, "%llu%s\n", value / divisor, unit);
}

static ssize_t power_max_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	return show_monitor_field(dev_get_drvdata(dev), buf,
				  MANGO_MGMT_MONITOR_SENSOR_TOTAL_POWER, true,
				  "W", 1000000);
}

static DEVICE_ATTR_RO(power_max);

static ssize_t power_avg_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	return show_monitor_field(dev_get_drvdata(dev), buf,
				  MANGO_MGMT_MONITOR_SENSOR_TOTAL_POWER, false,
				  "W", 1000000);
}

static DEVICE_ATTR_RO(power_avg);

static ssize_t serial_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct mango_mgmt_dev *mgmt = dev_get_drvdata(dev);
	u8 serial[MANGO_MGMT_MONITOR_SERIAL_LEN];
	ssize_t ret;

	if (!mgmt)
		return -EIO;

	if (!mgmt->monitor) {
		dev_err(mgmt->dev, "No available monitor");
		return -EIO;
	}

	ret = mgmt->monitor->get_info(mgmt, MGMT_MONITOR_INFO_SERIAL, serial,
				      sizeof(serial));
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%.*s\n", (int)ret, serial);
}

static DEVICE_ATTR_RO(serial);

static ssize_t temp_max_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	return show_monitor_field(dev_get_drvdata(dev), buf,
				  MANGO_MGMT_MONITOR_SENSOR_TEMP_FPGA, true,
				  "C", 1);
}

static DEVICE_ATTR_RO(temp_max);

static ssize_t temp_avg_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	return show_monitor_field(dev_get_drvdata(dev), buf,
				  MANGO_MGMT_MONITOR_SENSOR_TEMP_FPGA, false,
				  "C", 1);
}

static DEVICE_ATTR_RO(temp_avg);

static ssize_t macaddr_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct mango_mgmt_dev *mgmt = dev_get_drvdata(dev);
	u8 mac[MANGO_MGMT_MONITOR_MACADDR_LEN];
	ssize_t ret;

	if (!mgmt)
		return -EIO;

	if (!mgmt->monitor) {
		dev_err(mgmt->dev, "No available monitor");
		return -EIO;
	}

	ret = mgmt->monitor->get_info(mgmt, MGMT_MONITOR_INFO_MACADDR, mac,
				      sizeof(mac));
	if (ret < 0)
		return ret;
	if (ret < ETH_ALEN)
		return -ENODATA;

	return sysfs_emit(buf, "%pM\n", mac);
}

static DEVICE_ATTR_RO(macaddr);

static struct attribute *mango_mgmt_attrs_info[] = { &dev_attr_power_max.attr,
						     &dev_attr_power_avg.attr,
						     &dev_attr_temp_max.attr,
						     &dev_attr_temp_avg.attr,
						     &dev_attr_serial.attr,
						     &dev_attr_macaddr.attr,
						     NULL };

static struct attribute_group mango_mgmt_attrs_info_group = {
	.name = "info",
	.attrs = mango_mgmt_attrs_info,
};

/* ===== MPIPE attributes ===== */

/*
 * Helper: retrieve mpipe_netdev_priv from mgmt_dev, or NULL if netdev
 * is not yet registered.
 */
static struct mango_mgmt_mpipe_netdev_priv *
mango_mgmt_get_mpipe_priv(struct mango_mgmt_dev *mgmt)
{
	struct net_device *ndev = mango_mgmt_mpipe_get_netdev(mgmt);

	if (!ndev)
		return NULL;
	return netdev_priv(ndev);
}

static ssize_t host_ip_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct mango_mgmt_dev *mgmt = dev_get_drvdata(dev);
	struct mango_mgmt_mpipe_netdev_priv *priv;
	long ext;

	if (!mgmt)
		return -EIO;
	priv = mango_mgmt_get_mpipe_priv(mgmt);
	if (!priv)
		return sysfs_emit(buf, "(none)\n");

	ext = atomic_long_read(&priv->nat.external);
	if (ext == MANGO_MGMT_IP_PAIR_UNSET)
		return sysfs_emit(buf, "(unset)\n");

	return sysfs_emit(buf, "%pI4\n", &((__be32[]){ nat_host(ext) }));
}

static DEVICE_ATTR_RO(host_ip);

static ssize_t soc_ip_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct mango_mgmt_dev *mgmt = dev_get_drvdata(dev);
	struct mango_mgmt_mpipe_netdev_priv *priv;
	long ext;

	if (!mgmt)
		return -EIO;
	priv = mango_mgmt_get_mpipe_priv(mgmt);
	if (!priv)
		return sysfs_emit(buf, "(none)\n");

	ext = atomic_long_read(&priv->nat.external);
	if (ext == MANGO_MGMT_IP_PAIR_UNSET)
		return sysfs_emit(buf, "(unset)\n");

	return sysfs_emit(buf, "%pI4\n", &((__be32[]){ nat_soc(ext) }));
}

static ssize_t soc_ip_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct mango_mgmt_dev *mgmt = dev_get_drvdata(dev);
	__be32 new_soc_ext;
	int ret;

	if (!mgmt)
		return -EIO;

	if (in4_pton(buf, count, (u8 *)&new_soc_ext, '\n', NULL) == 0)
		return -EINVAL;

	ret = mango_mgmt_mpipe_nat_set_soc_ip(mgmt, new_soc_ext);
	if (ret)
		return ret;

	return count;
}

static DEVICE_ATTR_RW(soc_ip);

static ssize_t soc_internal_ip_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct mango_mgmt_dev *mgmt = dev_get_drvdata(dev);
	struct mango_mgmt_mpipe_netdev_priv *priv;
	__be32 ip;

	if (!mgmt)
		return -EIO;
	priv = mango_mgmt_get_mpipe_priv(mgmt);
	if (!priv)
		return sysfs_emit(buf, "(none)\n");

	ip = (__force __be32)atomic_read(&priv->nat.soc_internal_ip);
	return sysfs_emit(buf, "%pI4\n", &ip);
}

static DEVICE_ATTR_RO(soc_internal_ip);

static ssize_t netdev_name_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct mango_mgmt_dev *mgmt = dev_get_drvdata(dev);
	struct net_device *ndev;

	if (!mgmt)
		return -EIO;

	ndev = mango_mgmt_mpipe_get_netdev(mgmt);
	if (!ndev)
		return sysfs_emit(buf, "(none)\n");

	return sysfs_emit(buf, "%s\n", ndev->name);
}

static DEVICE_ATTR_RO(netdev_name);

static ssize_t tx_words_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct mango_mgmt_dev *mgmt = dev_get_drvdata(dev);
	struct mango_mgmt_mpipe_netdev_priv *priv;

	if (!mgmt)
		return -EIO;

	priv = mango_mgmt_get_mpipe_priv(mgmt);
	if (!priv)
		return sysfs_emit(buf, "0\n");

	return sysfs_emit(buf, "%ld\n", atomic_long_read(&priv->tx_words));
}

static DEVICE_ATTR_RO(tx_words);

static ssize_t rx_words_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct mango_mgmt_dev *mgmt = dev_get_drvdata(dev);
	struct mango_mgmt_mpipe_netdev_priv *priv;

	if (!mgmt)
		return -EIO;

	priv = mango_mgmt_get_mpipe_priv(mgmt);
	if (!priv)
		return sysfs_emit(buf, "0\n");

	return sysfs_emit(buf, "%ld\n", atomic_long_read(&priv->rx_words));
}

static DEVICE_ATTR_RO(rx_words);

static ssize_t rx_resyncs_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct mango_mgmt_dev *mgmt = dev_get_drvdata(dev);
	struct mango_mgmt_mpipe_netdev_priv *priv;

	if (!mgmt)
		return -EIO;

	priv = mango_mgmt_get_mpipe_priv(mgmt);
	if (!priv)
		return sysfs_emit(buf, "0\n");

	return sysfs_emit(buf, "%ld\n", atomic_long_read(&priv->rx_resyncs));
}

static DEVICE_ATTR_RO(rx_resyncs);

static struct attribute *mango_mgmt_attrs_mpipe[] = {
	&dev_attr_host_ip.attr,		&dev_attr_soc_ip.attr,
	&dev_attr_soc_internal_ip.attr, &dev_attr_netdev_name.attr,
	&dev_attr_tx_words.attr,	&dev_attr_rx_words.attr,
	&dev_attr_rx_resyncs.attr,	NULL
};

static umode_t mango_mgmt_mpipe_attr_visible(struct kobject *kobj,
					     struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct mango_mgmt_dev *mgmt = dev_get_drvdata(dev);

	/* NAT attrs are Host-only — hide on SoC */
	if (mgmt && mango_mgmt_transport_is_soc_device(mgmt)) {
		if (attr == &dev_attr_host_ip.attr ||
		    attr == &dev_attr_soc_ip.attr ||
		    attr == &dev_attr_soc_internal_ip.attr)
			return 0; /* hidden */
	}

	return attr->mode;
}

static struct attribute_group mango_mgmt_attrs_mpipe_group = {
	.name = "mpipe",
	.attrs = mango_mgmt_attrs_mpipe,
	.is_visible = mango_mgmt_mpipe_attr_visible,
};

/* ===== MPIPE debug attributes (mpipe_debug/) ===== */

/* Live FIFO state */

static ssize_t tx_fill_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct mango_mgmt_dev *mgmt = dev_get_drvdata(dev);

	return mgmt ? sysfs_emit(buf, "%zu\n",
				 mango_mgmt_mpipe_hw_tx_fill(mgmt))
		    : -EIO;
}
static DEVICE_ATTR_RO(tx_fill);

static ssize_t rx_fill_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct mango_mgmt_dev *mgmt = dev_get_drvdata(dev);

	return mgmt ? sysfs_emit(buf, "%zu\n",
				 mango_mgmt_mpipe_hw_rx_fill(mgmt))
		    : -EIO;
}
static DEVICE_ATTR_RO(rx_fill);

/* IRQ counters */

static ssize_t irq_count_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct mango_mgmt_dev *mgmt = dev_get_drvdata(dev);

	return mgmt ? sysfs_emit(buf, "%ld\n",
				 mango_mgmt_mpipe_hw_irq_count(mgmt))
		    : -EIO;
}
static DEVICE_ATTR_RO(irq_count);

static ssize_t tx_irq_count_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct mango_mgmt_dev *mgmt = dev_get_drvdata(dev);

	return mgmt ? sysfs_emit(buf, "%ld\n",
				 mango_mgmt_mpipe_hw_tx_irq_count(mgmt))
		    : -EIO;
}
static DEVICE_ATTR_RO(tx_irq_count);

static ssize_t rx_irq_count_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct mango_mgmt_dev *mgmt = dev_get_drvdata(dev);

	return mgmt ? sysfs_emit(buf, "%ld\n",
				 mango_mgmt_mpipe_hw_rx_irq_count(mgmt))
		    : -EIO;
}
static DEVICE_ATTR_RO(rx_irq_count);

static struct attribute *mango_mgmt_attrs_mpipe_debug[] = {
	/* live state */
	&dev_attr_tx_fill.attr, &dev_attr_rx_fill.attr,
	/* IRQ counters */
	&dev_attr_irq_count.attr, &dev_attr_tx_irq_count.attr,
	&dev_attr_rx_irq_count.attr, NULL
};

static struct attribute_group mango_mgmt_attrs_mpipe_debug_group = {
	.name = "mpipe_debug",
	.attrs = mango_mgmt_attrs_mpipe_debug,
};

static const struct attribute_group *mango_mgmt_attrs_groups[] = {
	&mango_mgmt_attrs_info_group, &mango_mgmt_attrs_mpipe_group,
	&mango_mgmt_attrs_mpipe_debug_group, NULL
};

static void mango_mgmt_cdev_release(struct device *dev)
{
	struct mango_mgmt_cdev *cdev =
		container_of(dev, struct mango_mgmt_cdev, dev);

	cleanup_srcu_struct(&cdev->srcu);
	ida_free(&mango_mgmt_cdev_ida, cdev->devid);
	kfree(cdev);
}

int mango_mgmt_sysfs_setup(struct mango_mgmt_dev *mgmt)
{
	struct mango_mgmt_cdev *cdev;
	int err, devid;

	cdev = kzalloc(sizeof(*cdev), GFP_KERNEL);
	if (!cdev)
		return -ENOMEM;

	if (init_srcu_struct(&cdev->srcu)) {
		kfree(cdev);
		return -ENOMEM;
	}

	devid = ida_alloc_range(&mango_mgmt_cdev_ida, 0,
				MANGO_MGMT_CDEV_MAX - 1, GFP_KERNEL);
	if (devid < 0) {
		dev_err(mgmt->dev, "Too many %s devices", MANGO_MGMT_CDEV_NAME);
		cleanup_srcu_struct(&cdev->srcu);
		kfree(cdev);
		return devid;
	}

	cdev->devid = devid;
	cdev->devno = MKDEV(MAJOR(mango_mgmt_cdev_devt), devid);
	rcu_assign_pointer(cdev->mgmt, mgmt);

	device_initialize(&cdev->dev);
	cdev->dev.devt = cdev->devno;
	cdev->dev.class = mango_mgmt_cdev_class;
	cdev->dev.parent = mgmt->dev;
	cdev->dev.groups = mango_mgmt_attrs_groups;
	cdev->dev.release = mango_mgmt_cdev_release;
	dev_set_drvdata(&cdev->dev, mgmt);

	err = dev_set_name(&cdev->dev, "%s%d", MANGO_MGMT_CDEV_NAME, devid);
	if (err)
		goto err_put_device;

	/* Initial fops: open + mmap of the MPIPE register window.
	 * Host's term_client_init may overlay sty_fops afterward; SoC
	 * keeps these defaults for the device's lifetime. */
	cdev_init(&cdev->cdev, &mango_mgmt_cdev_fops);
	cdev->cdev.owner = THIS_MODULE;

	err = cdev_device_add(&cdev->cdev, &cdev->dev);
	if (err < 0) {
		dev_err(mgmt->dev, "Failed to add device %d:%d",
			MAJOR(cdev->devno), MINOR(cdev->devno));
		goto err_put_device;
	}

	mgmt->mgmt.cdev = cdev;
	mgmt->mgmt.devid = devid;

	return 0;

err_put_device:
	put_device(&cdev->dev);
	return err;
}

void mango_mgmt_sysfs_teardown(struct mango_mgmt_dev *mgmt)
{
	struct mango_mgmt_cdev *cdev = mgmt->mgmt.cdev;

	if (!cdev)
		return;

	cdev_device_del(&cdev->cdev, &cdev->dev);
	rcu_assign_pointer(cdev->mgmt, NULL);
	synchronize_srcu(&cdev->srcu);
	mgmt->mgmt.cdev = NULL;
	mgmt->mgmt.devid = -1;
	put_device(&cdev->dev);
}

int mango_mgmt_sysfs_register()
{
	struct class *class;
	int err;

	class = mango_compat_class_create(MANGO_MGMT_CDEV_NAME);
	if (IS_ERR(class)) {
		pr_err("Failed to create class\n");
		return PTR_ERR(class);
	}

	err = alloc_chrdev_region(&mango_mgmt_cdev_devt, 0, MANGO_MGMT_CDEV_MAX,
				  MANGO_MGMT_CDEV_NAME);
	if (err < 0) {
		class_destroy(class);
		pr_err("Failed to allocate chrdev region\n");
		return err;
	}

	mango_mgmt_cdev_class = class;

	return 0;
}

void mango_mgmt_sysfs_unregister()
{
	unregister_chrdev_region(mango_mgmt_cdev_devt, MANGO_MGMT_CDEV_MAX);
	if (mango_mgmt_cdev_class)
		class_destroy(mango_mgmt_cdev_class);
}
