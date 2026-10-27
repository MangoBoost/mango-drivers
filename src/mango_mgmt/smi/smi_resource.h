/* SPDX-License-Identifier: GPL-2.0 */
/*
 * smi_resource.h - What a bus hands the driver: the windows it already mapped.
 *
 * Filled by the bus glue, carried in the attach data and consumed by the
 * message layer, so it sits where all three may name it.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_RESOURCE_H
#define SMI_RESOURCE_H

#include <linux/types.h>

/**
 * struct smi_cmd_resource_view - Pre-mapped resource view for Message layer.
 * Filled by Core (or Glue) after BARs are mapped.
 */
struct smi_cmd_resource_view {
	void __iomem *queue_base; /* Queue region base */
	size_t queue_size; /* Optional, for validation */
	void __iomem *payload_base; /* Payload/shared memory base */
	size_t payload_size; /* Optional, for validation */
};

#endif /* SMI_RESOURCE_H */
