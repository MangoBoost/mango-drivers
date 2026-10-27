// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - SMI component of a management device
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include "dev.h"
#include "mango_dev.h"
#include "regmap.h"
#include "transport/transport.h"
#include "smi_glue.h"
#include "smi_top.h"
#include "core/smi_eeprom.h"
#include "core/smi_rpc.h"
#include "core/smi_sensor.h"

static bool smi_host_card_busy(struct device *dev)
{
	struct pf_dev_struct *pf_dev = get_pf_dev_entry(dev, PF_DEV_CACHE_DEV);
	bool busy = smi_pub_busy(pf_dev);

	put_pf_dev_entry(pf_dev);
	return busy;
}

const struct mango_dev_card_ops mango_mgmt_smi_card_ops = {
	.card_busy = smi_host_card_busy,
};

static void *smi_host_reset_fence(struct device *dev)
{
	return mango_dev_reset_fence(dev);
}

static void smi_host_reset_unfence(void *fence)
{
	mango_dev_reset_unfence(fence);
}

static const struct smi_reset_fence_ops smi_host_reset_fence_ops = {
	.begin = smi_host_reset_fence,
	.end = smi_host_reset_unfence,
};

int mango_mgmt_smi_attach(struct mango_mgmt_dev *mgmt)
{
	struct smi_cmd_resource_view view = { 0 };
	struct smi_init_data init = {
		.msg_view = &view,
		.transport_type = FW_IF_TYPE_GCQ,
		.fence_ops = &smi_host_reset_fence_ops,
	};
	struct pf_dev_struct *pf_dev;
	u64 len;
	int ret;

	ret = mango_mgmt_regmap_block(mgmt, MGMT_BLK_GCQ, &view.queue_base,
				      &len);
	if (ret)
		return ret;
	view.queue_size = len;

	ret = mango_mgmt_regmap_block(mgmt, MGMT_BLK_GCQ_PAYLOAD,
				      &view.payload_base, &len);
	if (ret)
		return ret;
	view.payload_size = len;

	pf_dev = smi_device_init(mgmt->dev, &init);
	if (IS_ERR(pf_dev))
		return PTR_ERR(pf_dev);

	mgmt->smi = pf_dev;
	return 0;
}

void mango_mgmt_smi_detach(struct mango_mgmt_dev *mgmt)
{
	struct pf_dev_struct *pf_dev = mgmt->smi;

	if (!pf_dev)
		return;

	/* Unpublish first: smi_device_remove() frees the handle, and a reader
	 * that still finds the field set would follow it into freed slab. */
	mgmt->smi = NULL;
	smi_device_remove(pf_dev);
}

void mango_mgmt_smi_quiesce(struct mango_mgmt_dev *mgmt)
{
	smi_device_quiesce(mgmt->smi);
}

/* The SMI serving this device: its own, else a sibling function's on the
 * same slot. Counted; release with put_pf_dev_entry(). */
static struct pf_dev_struct *mango_mgmt_smi_get(struct mango_mgmt_dev *mgmt)
{
	struct pf_dev_struct *pf_dev;

	if (mgmt->smi) {
		pf_dev = smi_pf_dev_get(mgmt->smi);
		if (pf_dev)
			return pf_dev;
	}

	return smi_pf_dev_of_addr(mgmt->transport->get_mgmt_addr(mgmt));
}

int mango_mgmt_smi_dev_board_info(struct mango_mgmt_dev *mgmt,
				  enum smi_board_info type, u8 *buf, size_t len)
{
	struct pf_dev_struct *pf_dev = mango_mgmt_smi_get(mgmt);
	int ret;

	if (!pf_dev)
		return -ENODEV;

	ret = smi_pf_dev_board_info(pf_dev, type, buf, len);
	put_pf_dev_entry(pf_dev);
	return ret;
}

int mango_mgmt_smi_dev_sensor(struct mango_mgmt_dev *mgmt,
			      enum smi_sensor_type type,
			      struct smi_sensor_reading *out)
{
	struct pf_dev_struct *pf_dev = mango_mgmt_smi_get(mgmt);
	int ret;

	if (!pf_dev)
		return -ENODEV;

	ret = smi_pf_dev_sensor(pf_dev, type, out);
	put_pf_dev_entry(pf_dev);
	return ret;
}

int mango_mgmt_smi_dev_qsfp(struct mango_mgmt_dev *mgmt, u8 link,
			    struct smi_qsfp_status *status, u8 *num_links)
{
	struct pf_dev_struct *pf_dev = mango_mgmt_smi_get(mgmt);
	int ret;

	if (!pf_dev)
		return -ENODEV;

	ret = smi_pf_dev_qsfp(pf_dev, link, status, num_links);
	put_pf_dev_entry(pf_dev);
	return ret;
}

int mango_mgmt_smi_dev_eeprom_read(struct mango_mgmt_dev *mgmt, u8 *buf,
				   u16 len, u8 offset)
{
	struct pf_dev_struct *pf_dev = mango_mgmt_smi_get(mgmt);
	int ret;

	if (!pf_dev)
		return -ENODEV;

	ret = smi_pf_dev_eeprom_read(pf_dev, buf, len, offset);
	put_pf_dev_entry(pf_dev);
	return ret;
}

int mango_mgmt_smi_dev_eeprom_write(struct mango_mgmt_dev *mgmt, u8 *buf,
				    u16 len, u8 offset)
{
	struct pf_dev_struct *pf_dev = mango_mgmt_smi_get(mgmt);
	int ret;

	if (!pf_dev)
		return -ENODEV;

	ret = smi_pf_dev_eeprom_write(pf_dev, buf, len, offset);
	put_pf_dev_entry(pf_dev);
	return ret;
}

int mango_mgmt_smi_dev_eeprom_update(struct mango_mgmt_dev *mgmt, u8 offset,
				     u8 mask, u8 bits)
{
	struct pf_dev_struct *pf_dev = mango_mgmt_smi_get(mgmt);
	int ret;

	if (!pf_dev)
		return -ENODEV;

	ret = smi_pf_dev_eeprom_update(pf_dev, offset, mask, bits);
	put_pf_dev_entry(pf_dev);
	return ret;
}
