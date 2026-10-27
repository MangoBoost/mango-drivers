// SPDX-License-Identifier: GPL-2.0-only
/*
 * legacy_utils.h - The growing string the device map is written into.
 *
 * Copyright (c) 2023 Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_LEGACY_UTILS_H
#define SMI_LEGACY_UTILS_H

#include <linux/types.h>

int my_krealloc(void **buf, int old_size, int new_size, gfp_t flags);
int strconcat(char **dst, char src[], int *size);

#endif /* SMI_LEGACY_UTILS_H */
