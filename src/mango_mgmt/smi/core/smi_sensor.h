// SPDX-License-Identifier: GPL-2.0-only
/*
 * smi_sensor.h - The sensor store's readers.
 *
 * Every observation is copied out under the store's lock, so a caller holds a
 * whole one and nothing a refresh can free.
 *
 * Copyright (c) 2023 Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved. */

#ifndef SMI_SENSOR_H
#define SMI_SENSOR_H

#include "smi_api.h"
#include "smi_sensor_model.h"

/*
 * Number of milliseconds before a sensor reading is considered "stale". This is
 * a default value only - the actual value may be configured via hwmon. */
#define SENSOR_REFRESH_TIMEOUT_MS 1000

/* Forward declaration of pf_dev_struct */
struct pf_dev_struct;

int wait_sensor_service_ready(struct pf_dev_struct *pf_dev);
int discover_sensors(struct pf_dev_struct *pf_dev, int *empty_sdr_count);
void delete_sensors(struct pf_dev_struct *pf_dev);

int read_sensor_record(struct pf_dev_struct *pf_dev, uint8_t rid, int sid,
		       struct smi_sensor *out);
int read_sensor_kind(struct pf_dev_struct *pf_dev, uint8_t rid, uint8_t kind,
		     struct smi_sensor *out);
int read_sensor_record_at(struct pf_dev_struct *pf_dev, uint8_t rid, int index,
			  struct smi_sensor *out);
int read_sensor_fresh(struct pf_dev_struct *pf_dev, uint8_t rid, int sid,
		      struct smi_sensor *out, bool *fresh);
int read_sensor_repos(struct pf_dev_struct *pf_dev,
		      struct smi_repo_summary *out, int max);

/**
 * smi_scale_pow10() - Move a reading from the exponent it carries to another.
 * @val: The reading.
 * @from: The exponent the card states.
 * @to: The exponent wanted.
 * @out: The scaled reading.
 *
 * The exponent IS the power of ten, so this multiplies or divides by it rather
 * than matching it against a table of named exponents, which an exponent the
 * table does not name falls off.
 *
 * Return: 0, or -E2BIG when the result would not fit.
 */
int smi_scale_pow10(s64 val, int from, int to, s64 *out);

int read_thermal_sensors(struct pf_dev_struct *pf_dev, bool *fresh);

/**
 * smi_pub_sensors_check() - Can this build read a Sensors section's head?
 * @hdr: the head, as far as the section reached; what the card did not write
 *	 is zeroed (smi_pub_accept()).
 * @len: the section's length.
 *
 * Return: 0; -EOPNOTSUPP an untaught format or a head short of its floor;
 *         -ENODATA no cell it can read; -EPROTO arrays the section cannot hold.
 */
int smi_pub_sensors_check(struct smi_pub_sensors *hdr, uint32_t len);

int read_qsfp_sensor(struct pf_dev_struct *pf_dev, int channel, long *val,
		     uint8_t *status, bool *fresh, uint8_t *num_links);

int read_board_info(struct pf_dev_struct *pf_dev, struct smi_board *bd_info);

/* Reads on a device the caller holds a reference to; -ENODEV until ready.
 * board_info returns the bytes the card stated for the field. */
int smi_pf_dev_board_info(struct pf_dev_struct *pf_dev,
			  enum smi_board_info type, u8 *buf, size_t len);
int smi_pf_dev_sensor(struct pf_dev_struct *pf_dev, enum smi_sensor_type type,
		      struct smi_sensor_reading *out);
int smi_pf_dev_qsfp(struct pf_dev_struct *pf_dev, u8 link,
		    struct smi_qsfp_status *status, u8 *num_links);

#endif /* SMI_SENSOR_H */
