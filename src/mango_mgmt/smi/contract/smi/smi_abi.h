// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * smi_abi.h - the SMI wire contract the host driver and the SMC firmware share
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

/*
 * The contract two repositories compile, for every generation:
 * - each generation's words live in smi_abi_vN.h, so retiring one deletes a
 *   file
 * - C99 in kernel spelling; smi_abi.hpp is the firmware's C++ view
 * - a set a consumer enumerates is an X-macro list, any other a plain enum
 * - a change lands in the firmware first, then in each copy, byte for byte
 *   while the copy keeps this spelling: a copy may lag, never lead
 *
 * The law, for every generation -- breaking one takes a new number, never a new
 * meaning for an old one:
 *
 * - Little-endian. A body starts 4-byte aligned with a length a multiple of 4;
 *   an 8-byte field is promised only 4, so a reader copies it.
 * - A record or section head grows at its end only: short of its baseline is
 *   refused, min(len, sizeof) is copied, and presence is told by the length.
 * - Padding is rsvd and written zero, so a size ends at its last field: a new
 *   field goes past the length an earlier build published, never into rsvd.
 * - A request payload grows like a record: short of the verb's baseline is
 *   refused, bytes past what the card understands must be zero, the rest checked.
 * - A request's cid and slice are the card's until its one answer, sent after its
 *   last access: reused only after that answer or a completed negotiation.
 * - A chain read once holds one record per instance; a page read every tick
 *   holds {offset, count}, and a stride where its element can grow.
 * - A macro here is a fact that never changes; what a build chooses -- a
 *   deadline, a limit, a place, a count -- is stated on the wire.
 * - A section is written under its own seq: odd, body, even, each flushed. A
 *   reader takes seq, body, seq, keeps an even seq that held, retries boundedly.
 * - Numbers are never reused: a retired one stays spent, noted where it stood.
 * - A layout discriminator -- a prefix's fmt, a record's type or kind, a verb --
 *   is matched by membership; an unknown one stops that domain alone. A new one
 *   comes only with a seat or meaning change, declared beside the old one.
 * - Every other number is data: act on what this build was taught and keep the
 *   rest visible. A meaning read into an unknown one becomes a driver bug.
 * - Magic is written last, after the body is flushed, so a visible page is a
 *   whole one. An edit no compiler sees is not a wire change.
 * - The publish area holds only bytes any reader may see: no secret or
 *   credential -- a CDB password -- is ever published.
 * - The asserts below reach sizes and offsets, never bit order: a generation
 *   that spends bitfields pins it against a frame.
 */

#ifndef SMI_ABI_H
#define SMI_ABI_H

#ifdef __KERNEL__
  #include <linux/build_bug.h> /* static_assert */
  #include <linux/stddef.h>    /* offsetof     */
  #include <linux/types.h>
#else
  #include <stddef.h> /* offsetof */
  #include <stdint.h>
#endif

#include "smi_abi_qsfp.h" /* the QSFP cage contract, which a host UAPI includes alone */

#ifdef __cplusplus
extern "C" {
#endif

/* Names: a generation's vocabulary carries its prefix (smi_vN_* / SMI_VN_*) in
 * its own file; unprefixed here is shared, or read before one is known. */

/* A release version is telemetry: generation is settled by asking, what a card
 * serves by its descriptor, a bootstrap break by the magic at offset 0. */

/* One CQ entry IS one answer, this many bytes in every generation -- a law, not
 * a size that grows: a larger result rides the window. */
#define SMI_COMPLETION_FRAME_BYTES 16

/* The repositories a card answers for; the rid names one on the wire and inside
 * both sides. Columns: name * rid * payload kind * mandatory. */
#define SMI_REPO_LIST(X)          \
  X(Temp, 0xC1, Records, 1)       \
  X(Voltage, 0xC2, Records, 1)    \
  X(Current, 0xC3, Records, 1)    \
  X(Power, 0xC4, Records, 1)      \
  X(TotalPower, 0xC6, Records, 1) \
  X(BoardInfo, 0xC0, BdInfo, 1)   \
  X(Fpt, 0xF0, Fpt, 1)            \
  X(Card, 0xC7, Records, 0)       \
  X(Qsfp, 0xC5, Records, 0)

/* The Card repository's records, by id: facts the card has exactly one of. A
 * new one takes the next id. */
enum smi_card_item {
  SmiCardItem_BootSource = 1,
  SmiCardItem_BootReason = 2,
  SmiCardItem_Health     = 3,
};

/* The wire numbers of those values: the card maps its own states onto them. */
enum smi_boot_source {
  SmiBootSource_Unknown = 0,
  SmiBootSource_Primary = 1,
  SmiBootSource_Golden  = 2,
};

enum smi_boot_reason {
  SmiBootReason_Unknown   = 0,
  SmiBootReason_PoweredUp = 1,
  /* 2: burned */
  SmiBootReason_ExtPor  = 3,
  SmiBootReason_SwPor   = 4,
  SmiBootReason_SlrPor  = 5,
  SmiBootReason_ErrPor  = 6,
  SmiBootReason_DapSrst = 7,
  SmiBootReason_ErrSrst = 8,
  SmiBootReason_SwSrst  = 9,
  SmiBootReason_SlrSrst = 10,
};

enum smi_health {
  SmiHealth_Healthy  = 0,
  SmiHealth_Degraded = 1,
  SmiHealth_Critical = 2,
  SmiHealth_Fatal    = 3,
};

/* What a sensor IS -- the one column a host may branch on. A card states a kind
 * at most once, so a kind this build was never taught selects nothing. */
enum smi_sensor_kind {
  SmiSensorKind_None     = 0,
  SmiSensorKind_Board    = 1,
  SmiSensorKind_Fpga     = 2,
  SmiSensorKind_Ethernet = 3,
};

/* What a card says about the reading beside it -- one byte, carried by a
 * cell. NotPresent and NoData deny the reading. */
enum smi_sensor_status {
  SmiSensorStatus_NotPresent = 0, /* no sensor populates this slot */
  SmiSensorStatus_Valid      = 1, /* the reading is good */
  SmiSensorStatus_NoData     = 2, /* the sensor is there, the reading is not */
};

/* A threshold's slot in thr[] is its bit position, so mask & (1u << SmiThr_x)
 * says it was given; the last two bits name values, so the first is a count. */
enum smi_thr {
  SmiThr_UpperWarn  = 0,
  SmiThr_UpperCrit  = 1,
  SmiThr_UpperFatal = 2,
  SmiThr_LowerWarn  = 3,
  SmiThr_LowerCrit  = 4,
  SmiThr_LowerFatal = 5,
  SmiThr_Avg        = 6,
  SmiThr_Max        = 7,
};

/* The SDR support mask, one wire byte. The lower-ladder bits stay declared and
 * unset: frozen is not the same as read. */
enum smi_sens_mask {
  SmiSensMask_UpperWarn  = 1u << SmiThr_UpperWarn,
  SmiSensMask_UpperCrit  = 1u << SmiThr_UpperCrit,
  SmiSensMask_UpperFatal = 1u << SmiThr_UpperFatal,
  SmiSensMask_LowerWarn  = 1u << SmiThr_LowerWarn,
  SmiSensMask_LowerCrit  = 1u << SmiThr_LowerCrit,
  SmiSensMask_LowerFatal = 1u << SmiThr_LowerFatal,
  SmiSensMask_Avg        = 1u << SmiThr_Avg,
  SmiSensMask_Max        = 1u << SmiThr_Max,
};

#define SMI_THR_CNT ((size_t)SmiThr_Avg)

/* The unit modifier a record carries in one signed byte. The value IS the power
 * of ten, so a reader scales by it rather than looking it up. */
enum smi_unit_mod {
  SmiUnitMod_Mega  = 6,
  SmiUnitMod_Kilo  = 3,
  SmiUnitMod_None  = 0,
  SmiUnitMod_Milli = -3,
  SmiUnitMod_Micro = -6,
};

/* Read/write selector, shared by the eeprom and module payloads. */
enum smi_rw {
  SmiRw_Read  = 0,
  SmiRw_Write = 1,
};

/* LedCtl's op, the same byte in every generation's payload; one at or past
 * Max is an op the card lacks. */
enum smi_led_op {
  SmiLedOp_BeaconOff = 0,
  SmiLedOp_BeaconOn  = 1,
  SmiLedOp_ForceOff  = 2,
  SmiLedOp_ForceOn   = 3,
  SmiLedOp_Release   = 4,
  SmiLedOp_Max, /* one past the last op */
};

#define SMI_X_RID(name, rid, kind, req) SmiRepo_##name = (rid),
enum smi_repo_rid {
  SMI_REPO_LIST(SMI_X_RID)
};
#undef SMI_X_RID

/*---- The descriptor -- what a region says about itself ---------------------
 * One per region, at the base the medium handed the host: whether firmware
 * wrote, which generations the card serves, where frames and data are, and in
 * records where the log and each published section sit. A non-lane region
 * leaves the lane block zero; changing the fixed block takes a new magic.
 *--------------------------------------------------------------------------*/

#define SMI_MAGIC_SMID          0x534D4944 /* "SMID" */

/* The rpmsg channel a card publishing this descriptor advertises. Matched
 * before anything is read, so an unexpected name is a refusal with a reason. */
#define SMI_RPMSG_EPT_NAME_SMID "rpmsg-smc"

/* The record encoding -- the one thing a {type,len} chain cannot state about
 * itself. A reader that does not know the value still keeps the fixed block,
 * where the ring and the data region are. */
#define SMI_DESC_REC_FMT        1

/* Which generations a card serves: bit N is the one whose opcodes carry nibble
 * N. A set, not a number -- a host takes the intersection. Nibble 0 is the
 * frozen dialect's. */
#define SMI_GEN_BIT(nibble)     (1u << (nibble))

/*---- Anchors and the probe ------------------------------------------------
 * What every generation puts in the same place, so a receiver can act before it
 * knows the sender: the cid it matches by, and the halfword a probe reads. Each
 * generation asserts these about itself, never against one that may be gone.
 *--------------------------------------------------------------------------*/

#define SMI_REQ_CID_OFF         4 /* a request's cid, bytes [4,6)   */
#define SMI_RSP_CID_OFF         0 /* an answer's cid, bytes [0,2)   */
#define SMI_RSP_PROBE_OFF       2 /* the probe's halfword, [2,4)    */
#define SMI_PROBE_DONE          0 /* that halfword: answered        */
#define SMI_PROBE_NO_VERB       3 /* that halfword: no such verb    */

/* What the pair mapping this region uses it for; all zero when it is not a
 * lane. The ring's extent and its two slot sizes, never a slot count. */
struct smi_desc_lane {
  uint32_t ring_off; /* frames, from this region's base            */
  uint32_t ring_len; /* 0 = frames do not ride this region         */
  uint16_t sq_slot;
  uint16_t cq_slot;
  uint32_t rsvd;
  uint32_t data_off; /* the only extent a request may address      */
  uint32_t data_len;
};

/* Where the carve sits in the card's own address space, so a host that mapped a
 * region by its own means can check its mapping. A host that arrived at a
 * translated base cannot compare, and does not try. */
struct smi_desc_carve {
  uint64_t base_pa;
};
#define SMI_DESC_CARVE_BASELINE 8u
static_assert(sizeof(struct smi_desc_carve) >= SMI_DESC_CARVE_BASELINE, "Carve may only grow");

/* One region of the carve: which pair it serves, where it is, and its geometry
 * where frames ride it -- the only place a headless region's geometry lives. A
 * region the card did not lay out has no row. */
struct smi_desc_map {
  uint16_t             kind; /* SmiDescCarve_*                       */
  uint16_t             rsvd;
  uint32_t             off; /* from smi_desc_carve.base_pa           */
  uint32_t             len;
  struct smi_desc_lane lane; /* all zero where no frames ride it     */
};
#define SMI_DESC_MAP_BASELINE 36u
static_assert(sizeof(struct smi_desc_map) >= SMI_DESC_MAP_BASELINE, "Map may only grow");

struct smi_desc {
  uint32_t             magic;    /* SMI_MAGIC_SMID -- written last        */
  uint16_t             rec_fmt;  /* SMI_DESC_REC_FMT                      */
  uint16_t             rec_off;  /* where records start, >= the fixed block */
  uint16_t             desc_len; /* fixed block + records                 */
  uint16_t             gens;     /* SMI_GEN_BIT set                       */
  uint32_t             rsvd;
  struct smi_desc_lane lane;
};

/* The descriptor's records, in the negotiation's TLV, each with its baseline
 * beside it; a driver skips types from 0x80, the firmware's and its tools'. */
enum smi_desc_rec {
  SmiDesc_Region = 0x01, /* one per region, and per cage */
  SmiDesc_Log    = 0x02,
  SmiDesc_Build  = 0x03,
  /* 0x04 PubT1: burned */
  SmiDesc_Carve   = 0x05,
  SmiDesc_Map     = 0x06,
  SmiDesc_Boot    = 0x07, /* written first */
  SmiDesc_Verb    = 0x08, /* one per verb served */
  SmiDesc_Limits  = 0x09,
  SmiDesc_Counter = 0x0A, /* one per published counter */
};

/* The prefix every record carries, descriptor record or generation's answer. */
struct smi_tlv_hdr {
  uint16_t type; /* the list its chain belongs to names the value */
  uint16_t len;  /* body bytes that follow                       */
};

/* Region kinds: the head is smi_log_head for Log and smi_pub_<kind> otherwise;
 * kinds from 0x80 are the firmware's and its tools', and a driver skips them. */
enum smi_desc_region_kind {
  SmiDescRegion_Log = 0x00,
  /* 0x01 the whole publish area: burned */
  SmiDescRegion_Status   = 0x02,
  SmiDescRegion_Sensors  = 0x03,
  SmiDescRegion_Counters = 0x04,
  SmiDescRegion_QsfpCage = 0x05, /* one record per cage */
};

/* The carve's regions as the map names them: Shared is read by every host and
 * written by none; the rest are one per pair, named by the host they serve --
 * which is how a host finds its own row. */
enum smi_desc_carve_kind {
  SmiDescCarve_Shared   = 0,
  SmiDescCarve_LaneHost = 1,
  SmiDescCarve_LaneX86  = 2,
  SmiDescCarve_LaneApu  = 3,
};

/* Where a region firmware writes into sits, from this region's base. No record
 * means the card has none here -- which is not the same as a length of zero. */
struct smi_desc_region {
  uint16_t kind; /* [0,2)  SmiDescRegion_* */
  uint16_t inst; /* [2,4)  which one of a repeatable kind -- the cage number; else 0 */
  uint32_t off;  /* [4,8) */
  uint32_t len;  /* [8,12) */
};
#define SMI_DESC_REGION_BASELINE 12u
static_assert(sizeof(struct smi_desc_region) >= SMI_DESC_REGION_BASELINE, "Region may only grow");

/* This boot's identity: a counter the card raises each boot, never 0, so never
 * the last boot's. Every section carries it. */
struct smi_desc_boot {
  uint32_t boot; /* [0,4) */
};
#define SMI_DESC_BOOT_BASELINE 4u
static_assert(sizeof(struct smi_desc_boot) >= SMI_DESC_BOOT_BASELINE, "Boot may only grow");

/* Who may send a verb through the host's RPC. Zero opens nothing; the others
 * are fwctl's scopes, one past their numbers. */
enum smi_scope {
  SmiScope_Kernel     = 0, /* a protocol verb no host hands to user space */
  SmiScope_Config     = 1, /* fwctl CONFIGURATION */
  SmiScope_DebugRead  = 2, /* fwctl DEBUG_READ_ONLY */
  SmiScope_DebugWrite = 3, /* fwctl DEBUG_WRITE */
  SmiScope_DebugFull  = 4, /* fwctl DEBUG_WRITE_FULL */
};

/* One verb this card serves, a descriptor record per verb; the opcode's high
 * nibble is its generation. A verb with no record is one the card lacks. */
struct smi_desc_verb {
  uint16_t opcode;      /* [0,2)   a generation's opcode */
  uint8_t  pay_bytes;   /* [2,3)   payload bytes this card understands */
  uint8_t  scope;       /* [3,4)   SmiScope_* */
  uint32_t deadline_ms; /* [4,8)   the longest a host waits, from writing the frame */
  uint32_t win_max;     /* [8,12)  the largest win_len taken; 0 = no window, ~0 = the data region */
  uint32_t off_align;   /* [12,16) an offset the payload names is a multiple of this; 0 = any */
};
#define SMI_DESC_VERB_BASELINE 16u
static_assert(sizeof(struct smi_desc_verb) >= SMI_DESC_VERB_BASELINE, "Verb may only grow");

/* What a lane takes, beside what each verb takes: one record. */
struct smi_desc_limits {
  uint32_t eeprom_bytes; /* [0,4) the identity EEPROM's addressable extent */
  uint32_t max_inflight; /* [4,8) the most requests one lane may have parked at once */
};
#define SMI_DESC_LIMITS_BASELINE 8u
static_assert(sizeof(struct smi_desc_limits) >= SMI_DESC_LIMITS_BASELINE, "Limits may only grow");

/* The log ring's geometry, beside the Region record that says where it is. */
struct smi_desc_log {
  uint32_t rec_bytes;
  uint32_t rec_cnt;
};
#define SMI_DESC_LOG_BASELINE 8u
static_assert(sizeof(struct smi_desc_log) >= SMI_DESC_LOG_BASELINE, "Log may only grow");

/* The head of the log region a descriptor points at. Firmware writes fixed-size
 * records and advances write_idx, which counts them and never wraps: the slot
 * is idx % rec_cnt, so a reader that fell behind sees the jump, not a lap. */
struct smi_log_head {
  uint32_t write_idx;
  uint32_t rsvd;
};

/*---- The publish region ---------------------------------------------------
 * Firmware writes it unasked and a host only reads it; it sits before the data
 * region, where no request can address it. Every section opens with the prefix
 * smi_abi_qsfp.h declares, and its Region record says where it is.
 *--------------------------------------------------------------------------*/

/* How a published operation ended, in the block's own words: it outlives the
 * generation whose request filled it, so these numbers are frozen. */
enum smi_pub_op {
  SmiPubOp_Success      = 0,
  SmiPubOp_Failed       = 1,
  SmiPubOp_InvalidValue = 2,
  SmiPubOp_Busy         = 7,
  SmiPubOp_VerifyFailed = 11,
  SmiPubOp_Running      = 12,
};

/* A background op's progress, for a request answered Accepted. */
struct smi_pub_bg {
  uint16_t op_id;
  uint16_t percent;
  uint32_t status; /* SmiPubOp_*, once it ends */
};

/* Whether firmware serves, as one halfword a host may branch on. */
enum smi_pub_state {
  SmiPubState_Ready  = 0, /* serving */
  SmiPubState_Busy   = 1, /* a session, or a closed session's walk, holds the card */
  /* 2 Degraded: burned */
  SmiPubState_Wedged = 3, /* a device did not answer and a deadline expired */
};

/* Status as a kernel reads it each slice: whether the card serves and its task
 * runs. Never grown: a new fact goes to the Card repository or a new section. */
struct smi_pub_status_head {
  struct smi_pub_sect sect;
  uint32_t            tick;      /* +1 per slice of the serving task */
  uint32_t            uptime_ms; /* monotonic, for correlating with logs */
  uint16_t            tick_ms;   /* the slice's ceiling: how often tick can rise */
  uint16_t            state;     /* whether it serves: 0 ready, else busy or wedged */
};

/* Status, written every slice: the head, then the tools' tail. */
struct smi_pub_status {
  struct smi_pub_status_head head;
  uint16_t                   busy_op; /* the inband opcode holding the card; 0: none (another channel, a closed walk) */
  uint16_t                   rsvd;
  uint32_t                   qsfp_gen; /* +1 when a cage's module epoch or state moves */
  struct smi_pub_bg          bg;       /* the latest background op */
  struct smi_pub_bg          bg_prev;  /* the one before it, as it ended */
};
#define SMI_PUB_STATUS_FMT      1u
#define SMI_PUB_STATUS_BASELINE 48u
static_assert(sizeof(struct smi_pub_status) >= SMI_PUB_STATUS_BASELINE, "Status may only grow");

/* The sensor values, written on each sample and each move of a Card value's
 * source; value i sits at val_off + i * stride, named by map slot i. */
struct smi_pub_sensors {
  struct smi_pub_sect sect;
  uint32_t            epoch;     /* moves with the map, never with a value */
  uint32_t            sample_ms; /* uptime of the sample the cells hold */
  uint16_t            period_ms; /* how often a sample is taken, at most */
  uint16_t            cnt;       /* value cells */
  uint16_t            stride;    /* bytes per cell */
  uint16_t            map_off;   /* from this section's start */
  uint16_t            val_off;
  uint16_t            rsvd;
};
#define SMI_PUB_SENSORS_FMT      1u
#define SMI_PUB_SENSORS_BASELINE 32u
static_assert(sizeof(struct smi_pub_sensors) >= SMI_PUB_SENSORS_BASELINE, "Sensors may only grow");

/* One map slot: which sensor the value cell at the same index belongs to. */
struct smi_pub_key {
  uint8_t  rid;
  uint8_t  id; /* record ordinal within the repository */
  uint16_t rsvd;
};

/* One value cell, read periodically. No alarm: the host computes it from the
 * limits it cached. All three numbers are signed, because a temperature goes
 * below zero; whether the running pair is kept is the record's Avg and Max
 * mask bits, not a value spent inside the cell. */
struct smi_pub_val {
  int32_t value;
  int32_t avg;
  int32_t max;
  uint8_t status; /* SmiSensorStatus_* */
  uint8_t rsvd[3];
};

/* This firmware's counters, written when one moved: u32 slots at val_off, each
 * named by a Counter record. The slots are firmware's and append-only. */
struct smi_pub_counters {
  struct smi_pub_sect sect;
  uint16_t            cnt;     /* slots */
  uint16_t            val_off; /* from this section's start */
};
#define SMI_PUB_COUNTERS_FMT      1u
#define SMI_PUB_COUNTERS_BASELINE 16u
static_assert(sizeof(struct smi_pub_counters) >= SMI_PUB_COUNTERS_BASELINE, "Counters may only grow");

/* A descriptor record: the Counters section's slot `slot` is `name`. */
struct smi_desc_counter {
  uint16_t slot;     /* [0,2) */
  uint16_t rsvd;     /* [2,4) */
  char     name[16]; /* [4,20) NUL padded */
};
#define SMI_DESC_COUNTER_BASELINE 20u
static_assert(sizeof(struct smi_desc_counter) >= SMI_DESC_COUNTER_BASELINE, "Counter may only grow");

/* The card's build, in the eight bytes every path that states one uses: the
 * descriptor's Build record, a bootstrap read's product, and this record. */
struct smi_identify_rsp {
  uint8_t ver_major;
  uint8_t ver_minor;
  uint8_t ver_patch;
  uint8_t ver_date[3];    /* YY MM DD, BCD */
  uint8_t link_ver_major; /* GCQ library -- telemetry, not a gate */
  uint8_t link_ver_minor;
};

/* What build a card is running: the eight-byte answer first, so a reader that
 * knows only that still reads it whole, then the name the tree gave it -- two
 * cards can answer the same numbers from different trees. */
struct smi_desc_build {
  struct smi_identify_rsp id;
  char                    tag[32]; /* NUL terminated */
};
#define SMI_DESC_BUILD_BASELINE 8u
static_assert(sizeof(struct smi_desc_build) >= SMI_DESC_BUILD_BASELINE, "Build may only grow");

/*----------------------------------------------------------------------------
 * The gate -- the sizes and offsets this file promises
 *
 * Here rather than in smi_abi.hpp so both consumers gate on them:
 * static_assert is a C++ keyword and a kernel macro, so one spelling serves
 * both. Bit order is not covered -- see the top of the file.
 *--------------------------------------------------------------------------*/

static_assert(sizeof(struct smi_identify_rsp) == 8, "the identify answer fills the CQ payload exactly");
/* The descriptor's fixed block: 16 of head and 24 of lane. A reader takes
 * rec_off rather than this number, but a different fixed block is a different
 * dialect with its own magic -- so the size is pinned. */
static_assert(sizeof(struct smi_desc) == 40, "the descriptor's fixed block");
static_assert(offsetof(struct smi_desc, magic) == 0, "magic is what a host reads first");
static_assert(offsetof(struct smi_desc, lane) == 16, "the lane block follows the head");
static_assert(sizeof(struct smi_desc_lane) == 24, "the lane block");
static_assert(sizeof(struct smi_desc_carve) == 8, "the carve's anchor");
static_assert(sizeof(struct smi_desc_map) == 36, "one map row: a region and its geometry");
static_assert(sizeof(struct smi_log_head) == 8, "the log region's head");

static_assert(sizeof(struct smi_tlv_hdr) == 4, "a record prefix is type + length");

/* The published sections: each opens with its prefix, and a host reaches a
 * cell by arithmetic, so its width is a promise; stride states it for growth. */
static_assert(offsetof(struct smi_pub_status, head) == 0 && offsetof(struct smi_pub_status_head, sect) == 0,
              "Status opens with its prefix");
static_assert(offsetof(struct smi_pub_sensors, sect) == 0 && offsetof(struct smi_pub_counters, sect) == 0,
              "every section opens with its prefix");
static_assert(sizeof(struct smi_pub_key) == 4, "one map slot");
static_assert(sizeof(struct smi_pub_val) == 16, "one value cell: value, avg, max, status");
static_assert(sizeof(struct smi_pub_bg) == 8, "background progress");

/*----------------------------------------------------------------------------
 * Media-neutral invariant
 *
 * HOW a frame is carried, and how a host is notified, are the medium's; this
 * file owns the frame and the window descriptor. Hence no poll interval, no
 * IRQ id.
 *--------------------------------------------------------------------------*/

#ifdef __cplusplus
}
#endif
#endif /* SMI_ABI_H */
