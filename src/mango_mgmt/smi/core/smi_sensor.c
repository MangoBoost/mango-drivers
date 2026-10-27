// SPDX-License-Identifier: GPL-2.0-only
/*
 * smi_sensor.c - The sensor store: what a card told us, and when.
 *
 * Holds one slot per repository, refreshes them on a timestamp, and copies
 * observations out under one lock. What a repository's bytes look like on the
 * wire is a generation's business, not this file's.
 *
 * Copyright (c) 2023 - 2025 Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include <linux/delay.h>
#include <linux/vmalloc.h>

#include "backend/smi_smc_control.h"
#include "contract/smi/smi_abi_qsfp.h" /* the QSFP link record */

#include "smi_pub_sect.h"
#include "smi_sensor.h"
#include "smi_top.h"

/**
 * repo_records() - A repository's records, or NULL where it holds none.
 * @repo: Repository to read.
 *
 * The payload views share one union, so a view is a pointer only when the
 * kind says so: a flash table's view is a struct by value, and reading
 * sensors out of it would follow its first bytes as an address. Asked here
 * rather than trusted of every caller. Caller holds sensor_lock.
 *
 * Return: The record array, or NULL.
 */
static struct smi_sensor *repo_records(const struct smi_repo *repo)
{
	const struct smi_repo_desc *desc;

	if (!repo)
		return NULL;

	desc = smi_repo_by_rid(repo->rid);
	if (!desc || (desc->kind != SmiRepoKind_Records))
		return NULL;

	return repo->sensors;
}

/* The store's own lookups. Both return a pointer into the store, so they are
 * private to this file and their result lives only under sensor_lock. */
static struct smi_repo *find_repo(struct pf_dev_struct *pf_dev, uint8_t rid);
static struct smi_sensor *find_record(struct pf_dev_struct *pf_dev, uint8_t rid,
				      int sid);

/**
 * repo_reset() - Drop a repository's payload and record why it has none.
 * @dev: Device the payload was allocated against.
 * @repo: Slot to reset.
 * @desc: Descriptor for this slot.
 * @state: New state.
 * @err: errno behind @state.
 *
 * Keeps the rid set so find_repo() still resolves the slot and its readers
 * report "unknown" instead of missing it entirely.
 */
static void repo_reset(struct device *dev, struct smi_repo *repo,
		       const struct smi_repo_desc *desc,
		       enum smi_repo_state state, int err)
{
	smi_repo_free_payload(dev, repo);
	repo->rid = desc->rid;
	repo->state = state;
	repo->last_error = err;
}

/* The kinds this layer parses; another kind is its reader's, and its slot says
 * so without a read. */
static bool core_reads(enum smi_repo_kind kind)
{
	return (SmiRepoKind_Records == kind) || (SmiRepoKind_BdInfo == kind);
}

/**
 * discover_one_repo() - Fetch one repository and classify the outcome.
 * @pf_dev: Pointer to PCI device data.
 * @desc: Descriptor for the repository to fetch.
 * @repo: Slot to populate.
 * @empty_sdr_count: Incremented for a repository that yields no records.
 *
 * Return: 0 to continue discovery, or the error that must end it.
 */
static int discover_one_repo(struct pf_dev_struct *pf_dev,
			     const struct smi_repo_desc *desc,
			     struct smi_repo *repo, int *empty_sdr_count)
{
	struct device *dev = pf_dev->dev;
	int ret;

	if (!core_reads(desc->kind)) {
		repo_reset(dev, repo, desc, SMI_REPO_ABSENT, -EOPNOTSUPP);
		return 0;
	}

	ret = smi_smc_repo_read(pf_dev->smc_ctx, desc, repo);

	if (!ret) {
		repo->state = SMI_REPO_PRESENT;
		repo->last_error = 0;
		return 0;
	}

	/* An abort is not a result: the command was dropped before the device
	 * answered, so end discovery rather than read a teardown as a missing
	 * repository. A non-zero return here releases every slot. */
	if (smi_cmd_aborted(ret)) {
		repo_reset(dev, repo, desc, SMI_REPO_ABSENT, ret);
		return ret;
	}

	/* Only the parser returns this: a well formed frame that carried no
	 * records. An SMC rejection falls through below, as absent. */
	if (ret == -ENOMSG) {
		repo_reset(dev, repo, desc, SMI_REPO_EMPTY, ret);
		*empty_sdr_count += 1;
		return 0;
	}

	repo_reset(dev, repo, desc, SMI_REPO_ABSENT, ret);

	/* An optional repository the firmware lacks fails differently per
	 * release, so any error means "no repository" and discovery goes on. */
	if (!desc->optional)
		return ret;

	*empty_sdr_count += 1;
	return 0;
}

/* Drops the session's scratch and repositories. Clears the pointer before the
 * length so no holder of the lock can read a length for freed memory;
 * devm_kfree(NULL) is a no-op. Caller holds sensor_lock. */
static void delete_sensors_locked(struct pf_dev_struct *pf_dev)
{
	struct smi_pub_scratch *s = &pf_dev->pub_scratch;
	int i;

	devm_kfree(pf_dev->dev, s->sect);
	devm_kfree(pf_dev->dev, s->slot);
	memset(s, 0, sizeof(*s));

	if (!pf_dev->sensor_repos || (pf_dev->num_sensor_repos == 0))
		return;

	for (i = 0; i < pf_dev->num_sensor_repos; i++)
		smi_repo_free_payload(pf_dev->dev, &(pf_dev->sensor_repos[i]));

	devm_kfree(pf_dev->dev, pf_dev->sensor_repos);
	pf_dev->sensor_repos = NULL;
	pf_dev->num_sensor_repos = 0;
}

/* A description read just landed in this slot. Its values come later from
 * the published plane, so an untouched slot must not read as fresh. */
static void repo_landed(struct pf_dev_struct *pf_dev, struct smi_repo *repo)
{
	repo->have_values = false;
	repo->last_update = jiffies;
}

/**
 * discover_sensors() - Read every repository this driver knows.
 * @pf_dev: Pointer to PCI device data.
 * @empty_sdr_count: Pointer to empty repository counter.
 *
 * Called once per PCI device. Allocates (managed, no caller free) and fills a
 * slot per repository, updating each one's `last_update`.
 *
 * Return: 0 or negative error code.
 */
int discover_sensors(struct pf_dev_struct *pf_dev, int *empty_sdr_count)
{
	int i = 0;
	int ret = 0;

	if (!pf_dev || !empty_sdr_count)
		return -EINVAL;

	mutex_lock(&pf_dev->sensor_lock);

	/* Publish the count only once the array exists, so no reader sees a
	 * length for a NULL array. */
	pf_dev->sensor_repos =
		devm_kzalloc(pf_dev->dev,
			     SMI_REPO_CNT * sizeof(struct smi_repo),
			     GFP_KERNEL);

	if (!pf_dev->sensor_repos) {
		mutex_unlock(&pf_dev->sensor_lock);
		return -ENOMEM;
	}

	pf_dev->num_sensor_repos = SMI_REPO_CNT;

	for (i = 0; i < pf_dev->num_sensor_repos; i++) {
		const struct smi_repo_desc *desc = smi_repo_by_index(i);

		ret = discover_one_repo(pf_dev, desc, &pf_dev->sensor_repos[i],
					empty_sdr_count);
		if (ret)
			break;

		repo_landed(pf_dev, &pf_dev->sensor_repos[i]);
	}

	/* Free sensors if we failed. */
	if (ret)
		delete_sensors_locked(pf_dev);

	mutex_unlock(&pf_dev->sensor_lock);
	return ret;
}

/* The ring attaches seconds before the sensor service, so an ungated first read
 * times out and aborts discovery. One answer means the rest are ready. */
#define SENSOR_SERVICE_READY_MAX_ATTEMPTS (10)
#define SENSOR_SERVICE_READY_DELAY_MS (1000)

/**
 * wait_sensor_service_ready() - Block until the SMC sensor service answers.
 * @pf_dev: Pointer to PCI device data.
 *
 * Probes one repository, retrying on timeout. Best-effort: on persistent
 * failure the caller still runs discovery, degraded.
 *
 * Return: 0 once the service answers, else the last negative error code.
 */
int wait_sensor_service_ready(struct pf_dev_struct *pf_dev)
{
	struct smc_control_ctx *smc_ctx = NULL;
	unsigned long t0 = 0;
	int attempt = 0;
	int ret = -ENODEV;

	if (!pf_dev || !pf_dev->smc_ctx)
		return -EINVAL;

	smc_ctx = pf_dev->smc_ctx;

	t0 = jiffies;
	for (attempt = 1; attempt <= SENSOR_SERVICE_READY_MAX_ATTEMPTS;
	     attempt++) {
		/* One repository read is the probe: a card that described
		 * nothing still answered. */
		struct smi_repo probe = { 0 };

		ret = smi_smc_repo_read(smc_ctx, smi_repo_by_index(0), &probe);
		smi_repo_free_payload(pf_dev->dev, &probe);
		if (ret == -ENOMSG)
			ret = 0;
		if (!ret)
			break;

		/* Retrying will not make an aborted probe ready. */
		if (smi_cmd_aborted(ret)) {
			SMI_DBG(smc_ctx, "Sensor service probe aborted (%d)",
				ret);
			break;
		}

		if (attempt < SENSOR_SERVICE_READY_MAX_ATTEMPTS)
			msleep(SENSOR_SERVICE_READY_DELAY_MS);
	}

	if (ret && !smi_cmd_aborted(ret))
		SMI_WARN(
			smc_ctx,
			"SMC sensor service not ready after %d attempts (%u ms); proceeding with degraded discovery",
			SENSOR_SERVICE_READY_MAX_ATTEMPTS,
			jiffies_to_msecs(jiffies - t0));
	else if (!ret && attempt > 1)
		SMI_INFO(smc_ctx,
			 "SMC sensor service ready after %d attempts (%u ms)",
			 attempt, jiffies_to_msecs(jiffies - t0));

	return ret;
}

/**
 * delete_sensors() - Drop every repository this device holds.
 * @pf_dev: Pointer to PCI device data.
 *
 * Idempotent: the first call NULLs sensor_repos, so later ones return at the
 * guard.
 *
 * Return: None.
 */
void delete_sensors(struct pf_dev_struct *pf_dev)
{
	if (!pf_dev)
		return;

	mutex_lock(&pf_dev->sensor_lock);
	delete_sensors_locked(pf_dev);
	mutex_unlock(&pf_dev->sensor_lock);
}

/**
 * find_repo() - Find one repository slot.
 * @pf_dev: Pointer to PCI device data.
 * @rid: SmiRepo_*.
 *
 * NOTE: may return an empty slot with no records.
 *
 * Internal: the returned pointer is only valid while sensor_lock is held.
 *
 * Return: The matched slot or NULL.
 */
static struct smi_repo *find_repo(struct pf_dev_struct *pf_dev, uint8_t rid)
{
	int i;

	if (!pf_dev->sensor_repos)
		return NULL;

	for (i = 0; i < pf_dev->num_sensor_repos; i++)
		if (pf_dev->sensor_repos[i].rid == rid)
			return &pf_dev->sensor_repos[i];

	return NULL;
}

/**
 * find_record() - Find one sensor by the channel it belongs to.
 * @pf_dev: Pointer to PCI device data.
 * @rid: SmiRepo_*.
 * @sid: Sensor id (hwmon channel; record id - 1).
 *
 * Internal: the returned pointer is only valid while sensor_lock is held.
 *
 * Return: The matched record or NULL.
 */
static struct smi_sensor *find_record(struct pf_dev_struct *pf_dev, uint8_t rid,
				      int sid)
{
	struct smi_repo *repo = find_repo(pf_dev, rid);
	struct smi_sensor *recs = repo_records(repo);
	int i;

	if (!recs)
		return NULL;

	for (i = 0; i < repo->num_records; i++)
		if (recs[i].id - 1 == sid)
			return &recs[i];

	return NULL;
}

/**
 * find_record_kind() - The record of one kind within a repository.
 *
 * What a sensor IS is a column the card fills, so this is the only selector
 * every consumer -- the sensor API, the alarms, hwmon -- shares. Caller holds
 * sensor_lock.
 *
 * Return: the record, or NULL when this card has no sensor of that kind.
 */
static struct smi_sensor *find_record_kind(struct pf_dev_struct *pf_dev,
					   uint8_t rid, uint8_t kind)
{
	struct smi_repo *repo = find_repo(pf_dev, rid);
	struct smi_sensor *recs = repo_records(repo);
	int i;

	if (!recs)
		return NULL;

	for (i = 0; i < repo->num_records; i++)
		if (recs[i].kind == kind)
			return &recs[i];

	return NULL;
}

/* Records this store holds that a published cell can carry a value for, in the
 * order the pairing indexes them. Caller holds sensor_lock. */
static uint16_t pub_record_census(struct pf_dev_struct *pf_dev)
{
	uint16_t n = 0;
	int i;

	for (i = 0; i < pf_dev->num_sensor_repos; i++)
		if (repo_records(&pf_dev->sensor_repos[i]))
			n += pf_dev->sensor_repos[i].num_records;

	return n;
}

/**
 * scratch_grow() - Make one scratch buffer hold @need bytes.
 *
 * Scratch, so nothing is carried across: the read that needed the room writes
 * every byte of it. That is why this allocates rather than reallocates -- there
 * is no old content to move, and a copy would only cost.
 *
 * Return: the buffer to use, or NULL when @need could not be allocated.
 */
static void *scratch_grow(struct device *dev, void *buf, size_t *cap,
			  size_t need)
{
	void *p;

	if (*cap >= need)
		return buf;

	p = devm_kzalloc(dev, need, GFP_KERNEL);
	if (!p)
		return NULL;

	devm_kfree(dev, buf);
	*cap = need;
	return p;
}

/* A sample older than this many of its own periods is not served as a
 * measurement: the task that takes it has stopped. */
#define SMI_PUB_SAMPLE_PERIODS (3u)

/**
 * pub_scratch_fit() - Size this device's scratch for what it publishes.
 * @pf_dev: Pointer to top level PCI data struct.
 * @len: The Sensors section's length, as the card states it.
 *
 * The buffer is the whole section; the pairing is a slot per record this store
 * holds, fixed by discovery. Caller holds sensor_lock.
 *
 * Return: 0, or -ENOMEM.
 */
static int pub_scratch_fit(struct pf_dev_struct *pf_dev, uint32_t len)
{
	struct smi_pub_scratch *s = &pf_dev->pub_scratch;
	uint16_t records = pub_record_census(pf_dev);
	uint8_t *sect;

	if (s->slot_cnt != records) {
		uint16_t *slot =
			scratch_grow(pf_dev->dev, s->slot, &s->slot_bytes,
				     (size_t)records * sizeof(*slot));

		if (records && !slot)
			return -ENOMEM;
		s->slot = slot;
		s->slot_cnt = records;
		s->paired = false;
	}

	sect = scratch_grow(pf_dev->dev, s->sect, &s->sect_bytes, len);
	if (!sect)
		return -ENOMEM;
	s->sect = sect;
	return 0;
}

int smi_pub_sensors_check(struct smi_pub_sensors *hdr, uint32_t len)
{
	/* A format this build was never taught, or a head short of its floor, is
	 * a section this host does not have. */
	int ret = smi_pub_accept(hdr, sizeof(*hdr), SMI_PUB_SENSORS_FMT,
				 SMI_PUB_SENSORS_BASELINE);

	if (ret)
		return ret;

	/* A cell may grow; it may not shrink under this build. */
	if (!hdr->cnt || (hdr->stride < sizeof(struct smi_pub_val)))
		return -ENODATA;

	/* Both arrays inside the section, in 64-bit arithmetic so a sum near
	 * the top cannot wrap into a pass. */
	if ((((uint64_t)hdr->map_off +
	      (uint64_t)hdr->cnt * sizeof(struct smi_pub_key)) > len) ||
	    (((uint64_t)hdr->val_off + (uint64_t)hdr->cnt * hdr->stride) > len))
		return -EPROTO;

	return 0;
}

/* Does the held pairing still describe what the card publishes? Its key has
 * the boot too, as an epoch restarts each boot. Caller holds sensor_lock. */
static bool pub_pairing_current(struct pf_dev_struct *pf_dev, uint32_t boot,
				const struct smi_pub_sensors *hdr)
{
	const struct smi_pub_scratch *s = &pf_dev->pub_scratch;

	return s->paired && (s->key.boot == boot) &&
	       (s->key.epoch == hdr->epoch) && (s->key.cnt == hdr->cnt) &&
	       (s->key.stride == hdr->stride) &&
	       (s->key.map_off == hdr->map_off) &&
	       (s->key.val_off == hdr->val_off);
}

/**
 * pub_pair() - Work out which cell carries each record's value.
 *
 * The map says which sensor each cell is; the records say which sensor each
 * record is. Pairing them rather than assuming an order is why a repository
 * can gain a record without this changing.
 *
 * Return: 0, or -ENODATA when a record has no cell -- the caller then asks the
 *         wire rather than serving an unpaired record as fresh.
 */
static int pub_pair(struct pf_dev_struct *pf_dev, uint32_t boot,
		    const struct smi_pub_sensors *hdr)
{
	struct smi_pub_scratch *s = &pf_dev->pub_scratch;
	const struct smi_pub_key *map =
		(const struct smi_pub_key *)(s->sect + hdr->map_off);
	uint16_t at = 0;
	int i, r;

	for (i = 0; i < pf_dev->num_sensor_repos; i++) {
		struct smi_repo *repo = &pf_dev->sensor_repos[i];
		struct smi_sensor *recs = repo_records(repo);

		if (!recs)
			continue;

		for (r = 0; r < repo->num_records; r++, at++) {
			uint16_t c;

			s->slot[at] = SMI_PUB_NO_SLOT;
			for (c = 0; c < hdr->cnt; c++) {
				if ((map[c].rid != repo->rid) ||
				    (map[c].id != recs[r].id))
					continue;
				s->slot[at] = c;
				break;
			}
			if (s->slot[at] == SMI_PUB_NO_SLOT)
				return -ENODATA;
		}
	}

	s->paired = true;
	s->key.boot = boot;
	s->key.epoch = hdr->epoch;
	s->key.cnt = hdr->cnt;
	s->key.stride = hdr->stride;
	s->key.map_off = hdr->map_off;
	s->key.val_off = hdr->val_off;

	return 0;
}

/* The repositories whose values are the sensing task's sample, which a sample
 * too old to trust leaves unserved; the others hold the card's state. */
static bool repo_sampled(uint8_t rid)
{
	return (rid != SmiRepo_Card) && (rid != SmiRepo_Qsfp);
}

/* Is the cells' sample older than its period allows, by the card's clock? An
 * unreadable Status leaves it open, and the values are served as before. */
static bool pub_sample_stale(struct pf_dev_struct *pf_dev,
			     const struct smi_pub_sensors *hdr)
{
	struct smi_pub_status_head st;

	if (!hdr->period_ms || smi_smc_pub_status_head(pf_dev->smc_ctx, &st))
		return false;

	return (uint32_t)(st.uptime_ms - hdr->sample_ms) >
	       (SMI_PUB_SAMPLE_PERIODS * hdr->period_ms);
}

/**
 * read_sensors_published() - Refresh every records repository at once.
 * @pf_dev: Pointer to top level PCI data struct.
 * @rid: The repository the caller asked for.
 *
 * One read of the Sensors section serves every repository under its own seq,
 * and the map is paired again only when what the pairing was keyed on moved.
 *
 * Return: 0 when @rid's records were updated, -ENODATA when too old to serve,
 *         or an errno; a failure leaves them untouched for a wire fallback.
 */
static int read_sensors_published(struct pf_dev_struct *pf_dev, uint8_t rid)
{
	const struct smi_span *sec =
		smi_smc_pub_span(pf_dev->smc_ctx, SmiDescRegion_Sensors, 0);
	struct smi_pub_scratch *s = &pf_dev->pub_scratch;
	struct smi_pub_sensors hdr = { 0 };
	uint16_t at = 0;
	bool stale;
	int ret, i, r;

	if (!sec)
		return -EOPNOTSUPP;

	ret = pub_scratch_fit(pf_dev, sec->len);
	if (ret)
		return ret;

	ret = smi_smc_pub_read(pf_dev->smc_ctx, sec, s->sect, sec->len);
	if (ret)
		return ret;

	/* The head as far as the section reached; the check takes the rest. */
	memcpy(&hdr, s->sect, min_t(size_t, sizeof(hdr), sec->len));
	ret = smi_pub_sensors_check(&hdr, sec->len);
	if (ret == -EOPNOTSUPP) {
		/* Once per session: the card will keep publishing it. */
		if (!s->fmt_warned) {
			s->fmt_warned = true;
			SMI_DEV_WARN(
				pf_dev->dev,
				"published sensor format %u (head %u B) unknown: values unavailable",
				hdr.sect.fmt, hdr.sect.hdr_len);
		}
		return -ENODATA;
	}
	if (ret)
		return ret;

	if (!pub_pairing_current(pf_dev, pf_dev->facts.boot_id, &hdr)) {
		ret = pub_pair(pf_dev, pf_dev->facts.boot_id, &hdr);
		if (ret) {
			s->paired = false;
			return ret;
		}
	}

	stale = pub_sample_stale(pf_dev, &hdr);

	for (i = 0; i < pf_dev->num_sensor_repos; i++) {
		struct smi_repo *repo = &pf_dev->sensor_repos[i];
		struct smi_sensor *recs = repo_records(repo);
		const bool keep = stale && repo_sampled(repo->rid);

		if (!recs)
			continue;

		for (r = 0; r < repo->num_records; r++, at++) {
			struct smi_pub_val cell = { 0 };

			if (keep)
				continue;
			/* As much of the cell as this build knows; a card that
			 * grew one leaves the rest of it unread. */
			memcpy(&cell,
			       s->sect + hdr.val_off +
				       (size_t)s->slot[at] * hdr.stride,
			       sizeof(cell));
			/* Signed as the contract spells them, and whether a
			 * running pair is kept is the record's mask -- so the
			 * cell is taken as it stands. */
			recs[r].value = cell.value;
			recs[r].avg = cell.avg;
			recs[r].max = cell.max;
			recs[r].status = cell.status;
		}
		/* A measurement too old to serve keeps its last refresh time, so
		 * the next read asks again rather than serving it. */
		if (keep)
			continue;
		repo->have_values = true;
		repo->last_update = jiffies;
	}

	return (stale && repo_sampled(rid)) ? -ENODATA : 0;
}

/* Refresh one repository if its records are older than the refresh interval.
 * Caller holds sensor_lock: this writes what every reader copies. */
static int read_sensors(struct pf_dev_struct *pf_dev, uint8_t rid, bool *fresh)
{
	unsigned long stamp = 0;
	unsigned long delta = 0;
	uint16_t refresh_ms = 0;
	struct smi_repo *repo = NULL;
	int ret;

	/* `fresh` may be NULL */

	if (!pf_dev)
		return -EINVAL;

	/* The shared runtime sensor gate. INIT is accepted on purpose: the
	 * alarm worker may run after discovery but before probe publishes
	 * enabled=true. Terminal states must never reach SMC. */
	ret = smi_pf_dev_refusal(pf_dev);
	if (ret)
		return ret;
	if (!pf_dev->smc_ctx)
		return -ENODEV;

	repo = find_repo(pf_dev, rid);

	if (!repo)
		return -EINVAL;

	stamp = jiffies;
	delta = (long)stamp - (long)repo->last_update;
	refresh_ms = READ_ONCE(pf_dev->sensor_refresh);

	/* A slot that has never held a value is not fresh, it is empty: an
	 * interval measured from a read that landed none would serve zeroes. */
	if (!repo->have_values || (refresh_ms == 0) ||
	    ((delta * 1000 / HZ) > refresh_ms)) {
		if (fresh)
			*fresh = true;

		/* Values are only published; a card that publishes none has
		 * no value to give. One read serves every records repository,
		 * so the one asked for is fresh whichever it was. */
		if (!smi_smc_publishes_values(pf_dev->smc_ctx))
			return -EOPNOTSUPP;

		return read_sensors_published(pf_dev, rid);
	}

	if (fresh)
		*fresh = false;

	return 0;
}

/**
 * read_sensor_fresh() - Refresh a record's own repository, then copy it.
 * @pf_dev: Pointer to PCI device data.
 * @rid: Repository the record lives in.
 * @sid: Its ordinal there (record id - 1).
 * @out: Populated with a copy of the record.
 * @fresh: whether the refresh went to the card (output, may be NULL).
 *
 * One lock for both, so the copy is what that refresh landed. The repository
 * refreshed is the record's own, which is what a surface whose channels span
 * two of them needs.
 *
 * Return: 0, or an error from the refresh or the lookup.
 */
int read_sensor_fresh(struct pf_dev_struct *pf_dev, uint8_t rid, int sid,
		      struct smi_sensor *out, bool *fresh)
{
	int ret;

	if (!pf_dev || !out)
		return -EINVAL;

	mutex_lock(&pf_dev->sensor_lock);
	ret = read_sensors(pf_dev, rid, fresh);
	if (!ret) {
		const struct smi_sensor *rec = find_record(pf_dev, rid, sid);

		if (rec)
			*out = *rec;
		else
			ret = -ENOENT;
	}
	mutex_unlock(&pf_dev->sensor_lock);

	return ret;
}

/* Take the store's lock for one refresh. The value itself is copied out by a
 * separate call, which takes it again: a refresh landing in between only makes
 * that copy newer, and both observations are whole. */
static int refresh_sensors(struct pf_dev_struct *pf_dev, uint8_t rid,
			   bool *fresh)
{
	int ret;

	if (!pf_dev)
		return -EINVAL;

	mutex_lock(&pf_dev->sensor_lock);
	ret = read_sensors(pf_dev, rid, fresh);
	mutex_unlock(&pf_dev->sensor_lock);

	return ret;
}

/**
 * read_thermal_sensors() - Refresh the temperature readings.
 * @pf_dev: Pointer to top level PCI data struct.
 * @fresh: whether this call went to the card (output, may be NULL).
 *
 * Return: 0 on success or negative error code.
 */
int read_thermal_sensors(struct pf_dev_struct *pf_dev, bool *fresh)
{
	return refresh_sensors(pf_dev, SmiRepo_Temp, fresh);
}

/**
 * read_qsfp_sensor() - Read a single QSFP link status record.
 * @pf_dev: Pointer to PCI device data.
 * @channel: Sensor channel number (0-based, record id - 1).
 * @val: Pointer to raw packed sensor value (output).
 * @status: The record's status byte, SmiSensorStatus_* (output, may be NULL).
 * @fresh: Whether the value was read from the SMC or the cache (output).
 * @num_links: How many link records the repository holds (output, may be NULL).
 *
 * The raw packed u32 (link speed, status, cable detect, cage index); the
 * caller decodes it. -ENODATA when the firmware has no QSFP repository or no
 * records, with no wire traffic. @num_links comes from the same observation as
 * the value, so a caller cannot pair a count with a different refresh.
 *
 * Return: 0 or negative error code.
 */
int read_qsfp_sensor(struct pf_dev_struct *pf_dev, int channel, long *val,
		     uint8_t *status, bool *fresh, uint8_t *num_links)
{
	struct smi_repo *repo = NULL;
	struct smi_sensor *rec = NULL;
	int ret = 0;

	if (!pf_dev || !val || (channel < 0))
		return -EINVAL;

	mutex_lock(&pf_dev->sensor_lock);

	repo = find_repo(pf_dev, SmiRepo_Qsfp);
	if (!repo || !repo->num_records) {
		ret = -ENODATA;
		goto out;
	}

	ret = read_sensors(pf_dev, SmiRepo_Qsfp, fresh);
	if (ret)
		goto out;

	rec = find_record(pf_dev, SmiRepo_Qsfp, channel);
	if (!rec) {
		ret = -EINVAL;
		goto out;
	}

	*val = (long)rec->value;
	if (status)
		*status = rec->status;
	if (num_links)
		*num_links = repo->num_records;

out:
	mutex_unlock(&pf_dev->sensor_lock);
	return ret;
}

/**
 * read_sensor_record() - Copy one record out of the store, by sensor id.
 * @pf_dev: Pointer to PCI device data.
 * @rid: Repository the record lives in.
 * @sid: Sensor id (hwmon channel; record id - 1).
 * @out: Populated with a copy of the record.
 *
 * A record holds no pointers, so a copy is a whole observation the caller may
 * read at its leisure -- which is why nothing outside this file is handed the
 * record itself.
 *
 * Return: 0, or -ENOENT when this card has no such sensor.
 */
int read_sensor_record(struct pf_dev_struct *pf_dev, uint8_t rid, int sid,
		       struct smi_sensor *out)
{
	struct smi_sensor *rec;
	int ret = 0;

	if (!pf_dev || !out)
		return -EINVAL;

	mutex_lock(&pf_dev->sensor_lock);
	rec = find_record(pf_dev, rid, sid);
	if (rec)
		*out = *rec;
	else
		ret = -ENOENT;
	mutex_unlock(&pf_dev->sensor_lock);

	return ret;
}

/**
 * read_sensor_kind() - Copy the record of one kind out of the store.
 * @pf_dev: Pointer to PCI device data.
 * @rid: Repository the record lives in.
 * @kind: SmiSensorKind_*, what the sensor is.
 * @out: Populated with a copy of the record.
 *
 * Return: 0, or -ENOENT when this card has no sensor of that kind.
 */
int read_sensor_kind(struct pf_dev_struct *pf_dev, uint8_t rid, uint8_t kind,
		     struct smi_sensor *out)
{
	struct smi_sensor *rec;
	int ret = 0;

	if (!pf_dev || !out)
		return -EINVAL;

	mutex_lock(&pf_dev->sensor_lock);
	rec = find_record_kind(pf_dev, rid, kind);
	if (rec)
		*out = *rec;
	else
		ret = -ENOENT;
	mutex_unlock(&pf_dev->sensor_lock);

	return ret;
}

/**
 * read_sensor_record_at() - Copy one record out of the store, by position.
 * @pf_dev: Pointer to PCI device data.
 * @rid: Repository the record lives in.
 * @index: Position within the repository.
 * @out: Populated with a copy of the record.
 *
 * For a repository whose single record answers for the whole repository, where
 * the caller has no sensor id to name.
 *
 * Return: 0, or -ENODATA when the repository or the position is absent.
 */
int read_sensor_record_at(struct pf_dev_struct *pf_dev, uint8_t rid, int index,
			  struct smi_sensor *out)
{
	struct smi_repo *repo;
	int ret = 0;

	if (!pf_dev || !out || (index < 0))
		return -EINVAL;

	mutex_lock(&pf_dev->sensor_lock);
	repo = find_repo(pf_dev, rid);
	if (repo_records(repo) && (index < repo->num_records))
		*out = repo_records(repo)[index];
	else
		ret = -ENODATA;
	mutex_unlock(&pf_dev->sensor_lock);

	return ret;
}

/**
 * read_sensor_repos() - Copy what each repository says about itself.
 * @pf_dev: Pointer to PCI device data.
 * @out: Array to populate.
 * @max: How many entries @out holds.
 *
 * The payload stays behind: this is the census (which repositories, in what
 * state, with how many records) that a report or a channel-ceiling check needs.
 *
 * Return: Number of entries written, or a negative error code.
 */

int read_sensor_repos(struct pf_dev_struct *pf_dev,
		      struct smi_repo_summary *out, int max)
{
	int i, n = 0;

	if (!pf_dev || !out || (max < 0))
		return -EINVAL;

	mutex_lock(&pf_dev->sensor_lock);
	if (pf_dev->sensor_repos) {
		for (i = 0; (i < pf_dev->num_sensor_repos) && (n < max); i++) {
			const struct smi_repo *repo = &pf_dev->sensor_repos[i];

			out[n].rid = repo->rid;
			out[n].num_records = repo->num_records;
			out[n].state = repo->state;
			out[n].last_error = repo->last_error;
			n++;
		}
	}
	mutex_unlock(&pf_dev->sensor_lock);

	return n;
}

/*
 * read_board_info() - Retrieve the board information.
 * @pf_dev: Pointer to top level PCI data struct.
 * @bd_info: Pointer to populated with the board info data.
 *
 * Return: 0 on success or negative error code. */
int read_board_info(struct pf_dev_struct *pf_dev, struct smi_board *bd_info)
{
	struct smi_repo *repo = NULL;
	int ret = 0;

	if (!pf_dev || !bd_info)
		return -EINVAL;

	mutex_lock(&pf_dev->sensor_lock);

	repo = find_repo(pf_dev, SmiRepo_BoardInfo);

	if (!repo)
		ret = -EINVAL;
	else if (!repo->num_records || !repo->board)
		ret = -ENODATA;
	else
		*bd_info = *repo->board;

	mutex_unlock(&pf_dev->sensor_lock);
	return ret;
}

int smi_pf_dev_board_info(struct pf_dev_struct *pf_dev,
			  enum smi_board_info type, u8 *buf, size_t len)
{
	struct smi_repo *repo = NULL;
	struct smi_board_field *field = NULL;
	int ret = 0;

	if (!smi_pf_dev_ready(pf_dev))
		return -ENODEV;

	mutex_lock(&pf_dev->sensor_lock);

	repo = find_repo(pf_dev, SmiRepo_BoardInfo);

	if (!repo || !repo->board) {
		ret = -ENODATA;
		goto out;
	}

	switch (type) {
	case SMI_BOARD_INFO_MAC_ADDR:
		field = &repo->board->f[SmiBoardField_FirstMacAddr];
		break;
	case SMI_BOARD_INFO_SERIAL_NUMBER:
		field = &repo->board->f[SmiBoardField_BoardSerial];
		break;
	default:
		ret = -EINVAL;
		goto out;
	}

	if (field->len > len) {
		ret = -EINVAL;
	} else {
		memcpy(buf, field->bytes, field->len);
		/* The card's own length, which only this scope knows. */
		ret = field->len;
	}

out:
	mutex_unlock(&pf_dev->sensor_lock);
	return ret;
}

int smi_scale_pow10(s64 val, int from, int to, s64 *out)
{
	int shift = from - to;

	for (; shift < 0; shift++)
		val = div_s64(val, 10);

	for (; shift > 0; shift--) {
		if ((val > (S64_MAX / 10)) || (val < (S64_MIN / 10)))
			return -E2BIG;
		val *= 10;
	}

	*out = val;
	return 0;
}

/* Resolve a sensor type to its record: power and temperature live in different
 * repositories, and a per-part temperature is the record of that kind. Caller
 * holds sensor_lock, and the record it hands back is only valid while that
 * lock is held. */
static int find_sensor_record(struct pf_dev_struct *pf_dev,
			      enum smi_sensor_type type,
			      struct smi_sensor **out_rec)
{
	struct smi_sensor *recs;
	struct smi_repo *repo;
	struct smi_sensor *rec;
	uint8_t rid;
	uint8_t kind = SmiSensorKind_None;

	if (!smi_pf_dev_ready(pf_dev))
		return -ENODEV;
	if (!pf_dev->sensor_repos || pf_dev->num_sensor_repos == 0)
		return -ENODATA;

	/*
	 * The repo this reads from is the repo it refreshes: hwmon's power
	 * channel serves TOTAL_POWER, so that is the one that has to be fresh.
	 *
	 * SMI_SENSOR_TEMP reads the first temperature record; the per-part
	 * variants select one by the kind the card gave it.
	 */
	switch (type) {
	case SMI_SENSOR_POWER:
		rid = SmiRepo_TotalPower;
		break;
	case SMI_SENSOR_TEMP:
		rid = SmiRepo_Temp;
		break;
	case SMI_SENSOR_TEMP_BOARD:
		rid = SmiRepo_Temp;
		kind = SmiSensorKind_Board;
		break;
	case SMI_SENSOR_TEMP_FPGA:
		rid = SmiRepo_Temp;
		kind = SmiSensorKind_Fpga;
		break;
	case SMI_SENSOR_TEMP_ETHERNET:
		rid = SmiRepo_Temp;
		kind = SmiSensorKind_Ethernet;
		break;
	default:
		return -EINVAL;
	}

	/* Refresh sensor data (best-effort, ignore errors for stale data) */
	read_sensors(pf_dev, rid, NULL);

	repo = find_repo(pf_dev, rid);
	recs = repo_records(repo);
	if (!recs || !repo->num_records)
		return -ENODATA;

	rec = kind ? find_record_kind(pf_dev, rid, kind) : &recs[0];
	if (!rec)
		return -ENODATA;

	*out_rec = rec;
	return 0;
}

/* Scale a raw record value into the caller's unit: microwatts for power, whole
 * degrees for temperature. */
static int scale_sensor_value(enum smi_sensor_type type, int8_t unit_exp,
			      s64 raw, s64 *out)
{
	return smi_scale_pow10(raw, unit_exp,
			       (type == SMI_SENSOR_POWER) ? -6 : 0, out);
}

int smi_pf_dev_sensor(struct pf_dev_struct *pf_dev, enum smi_sensor_type type,
		      struct smi_sensor_reading *out)
{
	struct smi_sensor *rec = NULL;
	int ret, i;

	/* One row per threshold this API reports, naming the model slot it
	 * reads and the mask bit that says the card gave it. */
	static const struct {
		enum smi_sensor_threshold threshold;
		enum smi_thr slot;
	} threshold_map[] = {
		{ SMI_SENSOR_THRESHOLD_LOWER_FATAL, SmiThr_LowerFatal },
		{ SMI_SENSOR_THRESHOLD_LOWER_CRIT, SmiThr_LowerCrit },
		{ SMI_SENSOR_THRESHOLD_LOWER_WARN, SmiThr_LowerWarn },
		{ SMI_SENSOR_THRESHOLD_UPPER_WARN, SmiThr_UpperWarn },
		{ SMI_SENSOR_THRESHOLD_UPPER_CRIT, SmiThr_UpperCrit },
		{ SMI_SENSOR_THRESHOLD_UPPER_FATAL, SmiThr_UpperFatal },
	};

	if (!pf_dev || !out)
		return -EINVAL;

	/* Held across the whole reading: every field below belongs to the same
	 * record, and the record is the store's, not a copy. */
	mutex_lock(&pf_dev->sensor_lock);

	ret = find_sensor_record(pf_dev, type, &rec);
	if (ret) {
		mutex_unlock(&pf_dev->sensor_lock);
		return ret;
	}

	memset(out, 0, sizeof(*out));

	/* A record whose exponent puts any of its fields out of range is not a
	 * reading with one bad field: it is a record this host cannot report,
	 * and half of it would be read as the whole. */
	ret = scale_sensor_value(type, rec->unit_exp, rec->value, &out->value);
	/* No card reports a historical low; the field stays zero. */
	out->min = 0;
	/* The mask says whether the card keeps a running pair at all, and it is
	 * the only thing that says so: what sits in a cell the card does not
	 * keep is its own business, and a below-zero average is a reading, not
	 * a sentinel. A card that keeps neither reports the reading for both. */
	out->max = out->value;
	out->avg = out->value;
	if (!ret && (rec->mask & BIT(SmiThr_Max)))
		ret = scale_sensor_value(type, rec->unit_exp, rec->max,
					 &out->max);
	if (!ret && (rec->mask & BIT(SmiThr_Avg)))
		ret = scale_sensor_value(type, rec->unit_exp, rec->avg,
					 &out->avg);
	if (ret) {
		mutex_unlock(&pf_dev->sensor_lock);
		return ret;
	}
	out->status = rec->status;
	out->fields_valid = SMI_SENSOR_HAS_VALUE | SMI_SENSOR_HAS_MIN |
			    SMI_SENSOR_HAS_MAX | SMI_SENSOR_HAS_AVG;

	for (i = 0; i < (int)ARRAY_SIZE(threshold_map); i++) {
		const enum smi_sensor_threshold threshold =
			threshold_map[i].threshold;

		if (!(rec->mask & BIT(threshold_map[i].slot)))
			continue;

		ret = scale_sensor_value(type, rec->unit_exp,
					 rec->thr[threshold_map[i].slot],
					 &out->thresholds[threshold]);
		if (ret) {
			mutex_unlock(&pf_dev->sensor_lock);
			return ret;
		}
		out->thresholds_valid |= BIT(threshold);
	}

	mutex_unlock(&pf_dev->sensor_lock);
	return 0;
}

/* One link record, read by the contract's fields and rule. A newer format (a
 * reserved bit or state) is carried by raw alone, its fields unknown. */
static void decode_qsfp_status(u32 raw, struct smi_qsfp_status *status)
{
	*status = (struct smi_qsfp_status){ .raw = raw };
	if (!smi_qsfp_link_known(raw))
		return;

	status->speed_gbps = raw >> SMI_QSFP_LINK_SPEED_SHIFT;
	status->link_up = smi_qsfp_link_up(raw);
	status->cable_present = !!(raw & SMI_QSFP_LINK_CABLE);
	status->cage = raw & SMI_QSFP_LINK_CAGE_MASK;
}

int smi_pf_dev_qsfp(struct pf_dev_struct *pf_dev, u8 link,
		    struct smi_qsfp_status *status, u8 *num_links)
{
	long val = 0;
	int ret;

	if (!smi_pf_dev_ready(pf_dev))
		return -ENODEV;

	ret = read_qsfp_sensor(pf_dev, link, &val, NULL, NULL, num_links);
	if (ret)
		return ret;

	decode_qsfp_status((u32)val, status);
	return 0;
}

/* Address-keyed reads: a design without a management IP registers no
 * management device and still answers monitoring from its SMI. */
int mango_mgmt_smi_get_board_info(const char *hw_addr, enum smi_board_info type,
				  u8 *buf, size_t len)
{
	struct pf_dev_struct *pf_dev;
	int ret;

	if (!hw_addr || !buf)
		return -EINVAL;
	if (type == SMI_BOARD_INFO_MAC_ADDR && len < 6)
		return -EINVAL;

	pf_dev = smi_pf_dev_of_addr(hw_addr);
	if (!pf_dev)
		return -ENODEV;

	ret = smi_pf_dev_board_info(pf_dev, type, buf, len);
	put_pf_dev_entry(pf_dev);
	return ret;
}

int mango_mgmt_smi_get_sensor_info(const char *hw_addr,
				   enum smi_sensor_type type,
				   struct smi_sensor_reading *out)
{
	struct pf_dev_struct *pf_dev;
	int ret;

	if (!hw_addr || !out)
		return -EINVAL;

	pf_dev = smi_pf_dev_of_addr(hw_addr);
	if (!pf_dev)
		return -ENODEV;

	ret = smi_pf_dev_sensor(pf_dev, type, out);
	put_pf_dev_entry(pf_dev);
	return ret;
}

int mango_mgmt_smi_get_fw_version(const char *hw_addr, char *buf, size_t len)
{
	struct pf_dev_struct *pf_dev;
	int ret;

	if (!hw_addr || !buf || !len)
		return -EINVAL;

	pf_dev = smi_pf_dev_of_addr(hw_addr);
	if (!pf_dev)
		return -ENODEV;

	if (!pf_dev->facts.present) {
		put_pf_dev_entry(pf_dev);
		return -ENODEV;
	}

	ret = snprintf(buf, len, SMI_FW_VERSION_FMT,
		       SMI_FW_VERSION_ARGS(&pf_dev->facts.version));
	put_pf_dev_entry(pf_dev);

	return (size_t)ret >= len ? -ENOSPC : 0;
}

int mango_mgmt_smi_get_qsfp_status(const char *hw_addr, u8 link,
				   struct smi_qsfp_status *status,
				   u8 *num_links)
{
	struct pf_dev_struct *pf_dev;
	int ret;

	if (!hw_addr || !status)
		return -EINVAL;

	pf_dev = smi_pf_dev_of_addr(hw_addr);
	if (!pf_dev)
		return -ENODEV;

	ret = smi_pf_dev_qsfp(pf_dev, link, status, num_links);
	put_pf_dev_entry(pf_dev);
	return ret;
}
