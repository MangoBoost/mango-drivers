/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * smi_sensor_model.h - What a card's sensors and board identity are to this
 * driver, with no wire generation in the shape.
 *
 * The currency between the layers, so it sits beside the contract rather than
 * inside one: a generation's reader fills it, the store holds it, and every
 * consumer reads it. Retiring a generation deletes its reader, nothing else.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef SMI_SENSOR_MODEL_H
#define SMI_SENSOR_MODEL_H

#include <linux/types.h>

#include "contract/smi/smi_abi.h" /* SMI_REPO_LIST, SmiThr_*, SMI_THR_CNT */

#define SMI_SENSOR_NAME_MAX (32)
/* A board field's value, at the widest a card can state (6-bit length). */
#define SMI_FIELD_BYTES (64)

/* What a card says about a reading is the shared vocabulary (SmiSensorStatus_*).
 * This value is not in it: a record carrying it came from a card that kept no
 * status for that reading, which denies nothing. Display only. */
#define SMI_SENSOR_STATUS_LEGACY_NA 0x7F

/**
 * smi_status_denies_value() - Does this status deny the reading beside it?
 * @status: SmiSensorStatus_*, as the record carries it.
 *
 * Two codes deny it: no such sensor, and a sensor with no reading. Named
 * rather than "anything but valid", so neither a card that keeps no status
 * nor a code this build never heard of hides a value the card did state.
 *
 * Return: true when the reading must not be reported.
 */
static inline bool smi_status_denies_value(uint8_t status)
{
	return (status == SmiSensorStatus_NotPresent) ||
	       (status == SmiSensorStatus_NoData);
}

/**
 * struct smi_sensor - One reading, and what bounds it.
 * @rid: SmiRepo_*, the repository that holds it.
 * @id: ordinal within that repository, one based.
 * @status: SmiSensorStatus_*.
 * @mask: SmiSensMask_*, which thr[] slots the card gave.
 * @kind: SmiSensorKind_*, what the sensor is. The only column this driver may
 *	  select by; a generation with none answers None for every record.
 * @unit_exp: the reading is value x 10^unit_exp.
 * @name: NUL terminated label, display only.
 * @value: the reading.
 * @avg: average since boot; the Avg bit of @mask says whether one is kept.
 * @max: highest seen since boot; the Max bit of @mask says whether one is kept.
 * @thr: thresholds, indexed by SmiThr_*.
 *
 * The numbers are as wide as the wire's, so no reader has to know which of
 * them a narrower word would have folded.
 */
struct smi_sensor {
	uint8_t rid;
	uint8_t id;
	uint8_t status;
	uint8_t mask;
	uint8_t kind;
	int8_t unit_exp;
	char name[SMI_SENSOR_NAME_MAX];
	s64 value;
	s64 avg;
	s64 max;
	s64 thr[SMI_THR_CNT];
};

/* The board identity a card reports; the number is the order it states the
 * fields in. */
enum smi_board_field_id {
	SmiBoardField_EepromVersion = 0,
	SmiBoardField_ProductName = 1,
	SmiBoardField_BoardRev = 2,
	SmiBoardField_BoardSerial = 3,
	SmiBoardField_MacAddrCount = 4,
	SmiBoardField_FirstMacAddr = 5,
	SmiBoardField_ActiveState = 6,
	SmiBoardField_ConfigMode = 7,
	SmiBoardField_MfgDate = 8,
	SmiBoardField_PartNumber = 9,
	SmiBoardField_Uuid = 10,
	SmiBoardField_PcieId = 11,
	SmiBoardField_MaxPowerMode = 12,
	SmiBoardField_MemorySize = 13,
	SmiBoardField_OemId = 14,
	SmiBoardField_Capability = 15,
	SmiBoardField_MfgPartNumber = 16,
	/* one past the last field this driver keeps */
	SmiBoardField_Max
};

/**
 * struct smi_board_field - One board-identity field.
 * @len: valid bytes; a field carries a length, not a terminator.
 * @bytes: the value, zero filled past @len.
 */
struct smi_board_field {
	uint8_t len;
	uint8_t bytes[SMI_FIELD_BYTES];
};

/**
 * struct smi_board - The board identity, indexed by SmiBoardField_*.
 * @f: one entry per field; an absent field has len 0.
 */
struct smi_board {
	struct smi_board_field f[SmiBoardField_Max];
};

/**
 * enum smi_repo_kind - Which payload a repository carries.
 * @SmiRepoKind_Records: an array of struct smi_sensor
 * @SmiRepoKind_BdInfo: the board identity
 * @SmiRepoKind_Fpt: the partition tables, which the legacy face alone reads
 * @SmiRepoKind_Max: kinds from here on are one reader's own, named in its header
 *
 * The kind column of SMI_REPO_LIST names one of these or a reader's own, so a
 * repository added there without a reader for its kind does not compile.
 */
enum smi_repo_kind {
	SmiRepoKind_Records = 0,
	SmiRepoKind_BdInfo,
	SmiRepoKind_Fpt,
	SmiRepoKind_Max,
};

/**
 * enum smi_repo_state - What discovery established about a repository.
 * @SMI_REPO_ABSENT: no payload; @last_error says why the read did not land.
 * @SMI_REPO_EMPTY: the repository answered, with zero records.
 * @SMI_REPO_PRESENT: the payload is held.
 */
enum smi_repo_state {
	SMI_REPO_ABSENT = 0,
	SMI_REPO_EMPTY,
	SMI_REPO_PRESENT,
};

/**
 * struct smi_repo_desc - Everything the driver knows about one repository.
 * @rid: the repository byte both sides name it by.
 * @kind: which payload it carries, SmiRepoKind_* or its reader's own.
 * @name: human readable, for logs and the sdr_repos attribute.
 * @optional: firmware may predate it, so a failed read is version skew.
 */
struct smi_repo_desc {
	uint8_t rid;
	uint8_t kind;
	const char *name;
	bool optional;
};

/**
 * struct smi_repo - One repository's payload and what the last read found.
 * @rid: SmiRepo_*.
 * @num_records: records held; one for a board or flash payload.
 * @last_update: jiffies of the last read that landed values.
 * @have_values: has a read landed values in @sensors? A publishing generation
 *		 lands none at discovery, so the freshness interval must not
 *		 call an untouched slot fresh.
 * @state: outcome of that read.
 * @last_error: errno behind @state, 0 when present.
 * @block: owns the allocation the views below point into.
 * @sensors: SmiRepoKind_Records view.
 * @board: SmiRepoKind_BdInfo view.
 *
 * A kind with no view here is its reader's own, which finds it at @block.
 */
struct smi_repo {
	uint8_t rid;
	uint8_t num_records;
	unsigned long last_update;
	bool have_values;
	enum smi_repo_state state;
	int last_error;
	void *block;
	union {
		struct smi_sensor *sensors;
		struct smi_board *board;
	};
};

/* How many repositories there are, for a census array. */
#define SMI_X_REPO_COUNT(name, rid, kind, req) +1
#define SMI_REPO_CNT (0 SMI_REPO_LIST(SMI_X_REPO_COUNT))

/**
 * struct smi_repo_summary - What a repository says about itself, no payload.
 * @rid: SmiRepo_*.
 * @num_records: records it holds.
 * @state: what discovery established.
 * @last_error: why it is not present, when it is not.
 */
struct smi_repo_summary {
	uint8_t rid;
	uint8_t num_records;
	enum smi_repo_state state;
	int last_error;
};

struct device;

void smi_repo_free_payload(struct device *dev, struct smi_repo *repo);

const struct smi_repo_desc *smi_repo_by_rid(uint8_t rid);
const struct smi_repo_desc *smi_repo_by_index(size_t i);
const char *smi_repo_name(uint8_t rid);

#endif /* SMI_SENSOR_MODEL_H */
