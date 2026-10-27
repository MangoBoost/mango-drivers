/* SPDX-License-Identifier: GPL-2.0 */
/*
 * smi_fpt.h - A card's flash partition tables, as the legacy UAPI keeps them.
 *
 * The tables reach the kernel as a Describe chain. The legacy face reads its
 * copy when it presents
 * a card and again after it rewrote them; the card judges every flash op
 * itself.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_FPT_H
#define SMI_FPT_H

#include <linux/mutex.h>
#include <linux/types.h>

#include "smi_facts.h" /* SMI_SMC_BOOT_DEVICE_MAX */
#include "smi_sensor_model.h" /* struct smi_repo_summary */

struct device;
struct pf_dev_struct;

/**
 * struct smi_fpt_header - One flash device's partition-table header.
 * @version: table version
 * @header_size: header bytes
 * @entry_size: bytes per partition entry
 * @num_entries: partitions this device holds
 */
struct smi_fpt_header {
	uint8_t version;
	uint8_t header_size;
	uint8_t entry_size;
	uint8_t num_entries;
};

/**
 * struct smi_partition - One flash partition.
 * @type: partition type
 * @base_addr: offset in the flash device
 * @partition_size: bytes
 */
struct smi_partition {
	uint32_t type;
	uint32_t base_addr;
	uint32_t partition_size;
};

/**
 * struct smi_fpt - The partition tables, one per boot device.
 * @hdr: each device's header.
 * @part: each device's entries, in the block this struct opens.
 */
struct smi_fpt {
	struct smi_fpt_header hdr[SMI_SMC_BOOT_DEVICE_MAX];
	struct smi_partition *part[SMI_SMC_BOOT_DEVICE_MAX];
};

/**
 * struct smi_fpt_copy - The tables this card last stated.
 * @lock: guards all below; a refresh replaces them whole.
 * @fpt: the tables, heading the block they were read into; NULL when none.
 * @state: what the last read established.
 * @last_error: why none are held, 0 when they are.
 * @num_records: the records the repository held.
 */
struct smi_fpt_copy {
	struct mutex lock;
	struct smi_fpt *fpt;
	enum smi_repo_state state;
	int last_error;
	uint8_t num_records;
};

/**
 * smi_fpt_load() - Read the card's tables into a copy.
 * @pf_dev: the card.
 * @f: the copy.
 *
 * A failed read keeps what an earlier one held.
 *
 * Return: 0, -ENOMSG for a repository that held no record, or an errno.
 */
int smi_fpt_load(struct pf_dev_struct *pf_dev, struct smi_fpt_copy *f);

/**
 * smi_fpt_parse() - Land a v2 Describe chain of flash records.
 * @dev: the device the block is charged to.
 * @buf: the chain, @len bytes.
 * @len: its length.
 * @out: the tables, heading the block allocated for them.
 *
 * Return: 0, or -EPROTO for a chain that is no table this host can hold.
 */
int smi_fpt_parse(struct device *dev, const uint8_t *buf, size_t len,
		  struct smi_fpt **out);

/* The row sdr_repos lists for the tables, in the census's shape. */
void smi_fpt_summary(struct smi_fpt_copy *f, struct smi_repo_summary *row);

/* One device's header, or one of its partitions; -ENODATA where none is
 * held, -EINVAL for a device or partition the tables do not have. */
int smi_fpt_header(struct smi_fpt_copy *f, uint8_t boot_device,
		   struct smi_fpt_header *hdr);
int smi_fpt_partition(struct smi_fpt_copy *f, uint8_t boot_device,
		      uint32_t partition_id, struct smi_partition *partition);

#endif /* SMI_FPT_H */
