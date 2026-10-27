/* SPDX-License-Identifier: GPL-2.0 */
/*
 * legacy.h - The legacy surface: the vendor hwmon/ioctl spelling.
 *
 * What the frame's registry names: the row, its face and its driver files.
 * Every file under frontend/legacy/ is this surface's.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_LEGACY_H
#define SMI_LEGACY_H

#include "smi_frontend.h"

struct attribute_group;

extern const struct smi_frontend_ops smi_legacy_frontend_ops;
extern const struct smi_face_ops smi_legacy_face;
/* The driver-wide files CLI reads: the device map and the version. */
extern const struct attribute_group smi_legacy_driver_group;

#endif /* SMI_LEGACY_H */
