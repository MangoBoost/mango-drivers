// SPDX-License-Identifier: GPL-2.0-only
/*
 * smi_v2_repo.c - Read a second-generation repository into the neutral model.
 *
 * The second generation answers a chain of {type,len} records, one per
 * instance. This walks it twice: once to count what it will hold, once to fill
 * it -- so the store's payload is sized by what the card actually said.
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include <linux/device.h>
#include <linux/kernel.h> /* min3 */
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "smi_v2_repo.h"
#include "smi_cmd.h"
#include "smi_log.h"
#include "smi_smc_internal.h"
#include "smi_tlv.h" /* the one chain cursor */
#include "smi_cmd_row.h" /* struct smi_slice_result — what the answer filled */

/* What a first description is asked to fit. Not a ceiling: a card that answers
 * NoResource says what its own would take, and that is asked for instead. */
#define SMI_V2_REPO_BYTES (4096)

/* Which payload a record belongs to, so a chain that answers records of
 * another kind is refused rather than half-read. */
static bool desc_kind_holds(enum smi_repo_kind kind, uint16_t type)
{
	switch (type) {
	case SmiTlv_Sensor:
		return kind == SmiRepoKind_Records;
	case SmiTlv_BoardField:
		return kind == SmiRepoKind_BdInfo;
	default:
		return true; /* a record this build does not know */
	}
}

/* What the chain holds, counted before anything is allocated for it. The same
 * walk judges the chain: a malformed tail, a known record short of its
 * baseline or one of another payload's kind ends the read. */
struct desc_census {
	unsigned int sensors;
	bool board;
};

static int desc_count(const uint8_t *buf, size_t cap, enum smi_repo_kind kind,
		      struct desc_census *out)
{
	struct smi_tlv_rec r;
	size_t at = 0;
	int rem = (int)cap;

	memset(out, 0, sizeof(*out));
	while (smi_tlv_next(buf, &rem, &at, &r)) {
		if (!desc_kind_holds(kind, r.type))
			return -EPROTO;

		switch (r.type) {
		case SmiTlv_Sensor:
			if (r.len < SMI_TLV_SENSOR_BASELINE)
				return -EPROTO;
			out->sensors++;
			break;
		case SmiTlv_BoardField:
			if (r.len < SMI_TLV_BOARD_FIELD_BASELINE)
				return -EPROTO;
			out->board = true;
			break;
		default:
			break; /* a record this build does not know */
		}
	}

	return rem ? -EPROTO : 0;
}

/**
 * desc_fill_sensor() - Land one sensor record.
 *
 * Return: 0, or -EPROTO for a record that cannot be one of this repository's:
 *	   the rid it claims is the repository it was read from, the ordinal is
 *	   one based, and no two records share one.
 */
static int desc_fill_sensor(struct smi_repo *repo, unsigned int *n,
			    const struct smi_tlv_rec *r)
{
	struct smi_tlv_sensor w = { 0 };
	struct smi_sensor *s;
	size_t i;

	if (!smi_tlv_take(r, SMI_TLV_SENSOR_BASELINE, &w, sizeof(w)))
		return -EPROTO;

	if ((w.rid != repo->rid) || !w.id)
		return -EPROTO;
	for (i = 0; i < *n; i++)
		if (repo->sensors[i].id == w.id)
			return -EPROTO;

	s = &repo->sensors[*n];
	/* The repository is the one this answer was read from, not a second
	 * spelling of it inside the record: the published pairing and hwmon
	 * would otherwise name a record's repository differently. */
	s->rid = repo->rid;
	s->id = w.id;
	s->unit_exp = w.unit_exp;
	s->mask = w.mask;
	s->kind = w.kind;
	/* The card's status is a value, not a description: the published cell
	 * carries it, and until one is read the record reads as present. */
	s->status = SmiSensorStatus_Valid;
	for (i = 0; i < SMI_THR_CNT; i++)
		s->thr[i] = w.thr[i];
	/* The name is 32 bytes on the wire and need not be terminated. */
	memcpy(s->name, w.name, min(sizeof(s->name) - 1, sizeof(w.name)));
	(*n)++;
	return 0;
}

static void desc_fill_board(struct smi_repo *repo, const struct smi_tlv_rec *r)
{
	struct smi_tlv_board_field w = { 0 };
	struct smi_board_field *f;

	if (!smi_tlv_take(r, SMI_TLV_BOARD_FIELD_BASELINE, &w, sizeof(w)))
		return;
	/* A number this host does not serve: the record's length steps over it,
	 * which is what lets the card grow its identity without this build. */
	if (w.field >= SmiBoardField_Max)
		return;

	/* The source bounds this, not the destination: the length is the card's
	 * and the body is what this build was handed, so a longer length says
	 * the card grew a record this reader has not learnt -- not that there
	 * are more bytes to take. */
	f = &repo->board->f[w.field];
	f->len = min3((size_t)w.len, sizeof(w.body), sizeof(f->bytes));
	memcpy(f->bytes, w.body, f->len);
}

int smi_v2_repo_parse(struct smc_control_ctx *smc_ctx,
		      const struct smi_repo_desc *desc, const uint8_t *buf,
		      size_t cap, struct smi_repo *repo)
{
	struct desc_census census;
	struct smi_tlv_rec r;
	unsigned int n = 0;
	size_t at = 0;
	size_t bytes = 0;
	int rem;
	int ret;

	ret = desc_count(buf, cap, desc->kind, &census);
	if (ret)
		return ret;
	repo->rid = desc->rid;

	switch (desc->kind) {
	case SmiRepoKind_Records:
		bytes = census.sensors * sizeof(struct smi_sensor);
		repo->num_records =
			(uint8_t)min_t(unsigned int, census.sensors, U8_MAX);
		break;
	case SmiRepoKind_BdInfo:
		bytes = census.board ? sizeof(struct smi_board) : 0;
		repo->num_records = census.board ? 1 : 0;
		break;
	default:
		return -EINVAL;
	}

	if (!repo->num_records || !bytes)
		return -ENOMSG;

	if (bytes) {
		repo->block = devm_kzalloc(smc_ctx->dev, bytes, GFP_KERNEL);
		if (!repo->block)
			return -ENOMEM;
	}
	if (desc->kind == SmiRepoKind_Records)
		repo->sensors = repo->block;
	else
		repo->board = repo->block;

	/* The census walked this same chain, so what it counted is what lands
	 * here -- the kind decides the filler and the count bounds it. */
	rem = (int)cap;
	while (smi_tlv_next(buf, &rem, &at, &r)) {
		switch (r.type) {
		case SmiTlv_Sensor:
			if (n < census.sensors) {
				ret = desc_fill_sensor(repo, &n, &r);
				if (ret)
					goto fail;
			}
			break;
		case SmiTlv_BoardField:
			if (repo->board)
				desc_fill_board(repo, &r);
			break;
		default:
			break;
		}
	}

	return 0;

fail:
	/* A refusal leaves no allocation and no view into one: half a census is
	 * what the two-pass walk exists to avoid publishing. */
	smi_repo_free_payload(smc_ctx->dev, repo);
	return ret;
}

/* One ask at one size; the chain it answered is the caller's to free. */
static int desc_ask(struct smc_control_ctx *smc_ctx, uint8_t rid, uint32_t cap,
		    uint8_t **chain, uint32_t *len, uint32_t *needed)
{
	struct smi_slice_result slice = { 0 };
	uint8_t *buf;
	int ret;

	buf = vmalloc(cap);
	if (!buf)
		return -ENOMEM;

	ret = smi_submit_command_slice(smc_ctx, SMC_CMD_ID_GET_SDR, rid, NULL,
				       buf, cap, &slice);
	if (ret) {
		/* Refused for room: the card says what the whole answer takes,
		 * and that is the only size worth asking for. */
		if ((ret == -ENOBUFS) && !slice.used)
			*needed = slice.val;
		vfree(buf);
		return ret;
	}

	/* The chain ends where the answer says it does, not where the card
	 * stopped writing into a buffer this host happened to size. */
	*chain = buf;
	*len = slice.used;
	return 0;
}

int smi_v2_describe(struct smc_control_ctx *smc_ctx, uint8_t rid,
		    uint8_t **chain, uint32_t *len)
{
	uint32_t needed = 0;
	uint32_t room;
	int ret;

	ret = desc_ask(smc_ctx, rid, SMI_V2_REPO_BYTES, chain, len, &needed);
	if ((ret != -ENOBUFS) || (needed <= SMI_V2_REPO_BYTES))
		return ret;

	/* Once more, at the size the card named; a second refusal is a card
	 * asking past what a request carries. Judged before any allocation: an
	 * answer rides one slice. */
	room = smi_cmd_data_bytes(smc_ctx->cmd);
	if (!room || (needed > room)) {
		SMI_DEV_ERR(smc_ctx->dev,
			    "repo 0x%x wants %u bytes; a slice carries %u", rid,
			    needed, room);
		return -E2BIG;
	}
	SMI_DEV_DBG(smc_ctx->dev, "repo 0x%x wants %u bytes; asking again", rid,
		    needed);
	return desc_ask(smc_ctx, rid, needed, chain, len, &needed);
}

int smi_v2_repo_read(struct smc_control_ctx *smc_ctx,
		     const struct smi_repo_desc *desc, struct smi_repo *repo)
{
	uint8_t *chain = NULL;
	uint32_t len = 0;
	int ret;

	if (!smc_ctx || !desc || !repo)
		return -EINVAL;

	ret = smi_v2_describe(smc_ctx, desc->rid, &chain, &len);
	if (!ret) {
		ret = smi_v2_repo_parse(smc_ctx, desc, chain, len, repo);
		vfree(chain);
	}

	if (ret && (ret != -ENOMSG))
		smi_repo_free_payload(smc_ctx->dev, repo);

	return ret;
}
