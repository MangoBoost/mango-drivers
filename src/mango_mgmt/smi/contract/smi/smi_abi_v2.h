// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * smi_abi_v2.h - the second wire generation: its verbs, its frames and its words
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

/*
 * Both repositories hold this under smi_abi.h's law: firmware drops it at the
 * next promotion, and the driver keeps it while a card in the field speaks it.
 */

#ifndef SMI_ABI_V2_H
#define SMI_ABI_V2_H

#include "contract/smi/smi_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* This generation's bit: its opcodes carry nibble 1. */
#define SMI_GEN_BIT_V2    SMI_GEN_BIT(1)

/* The verbs a kernel sends, by opcode: the high byte is the group, its high
 * nibble the generation. */
enum smi_v2_opcode {
  SmiV2Opcode_Negotiate  = 0x1000,
  SmiV2Opcode_Ping       = 0x1001,
  /* 0x1002 Abort: burned */
  SmiV2Opcode_Release    = 0x1003,
  SmiV2Opcode_Describe   = 0x1100,
  SmiV2Opcode_EepromRw   = 0x1200,
  /* 0x1201 ModuleRw: burned */
  SmiV2Opcode_QsfpRw     = 0x1202,
  SmiV2Opcode_LedCtl     = 0x1203,
  SmiV2Opcode_FlashWrite = 0x1300,
  SmiV2Opcode_FlashCopy  = 0x1301,
  SmiV2Opcode_BootSelect = 0x1302,
  SmiV2Opcode_LogLevel   = 0x1400,
  /* 0x1401 StatsReset: burned */
  SmiV2Opcode_ShellExec  = 0x1402,
};

/* A nonzero status reads, known or not, as its range: refused, retry, failed, in
 * progress; 0 and 3 are the probe's. Columns: name * wire value * errno. */
#define SMI_V2_STATUS_LIST(X)                                                   \
  X(Success, 0x00, 0)                                                           \
  X(InvalidValue, 0x02, -EINVAL)  /* a value out of range                    */ \
  X(BadOpcode, 0x03, -EOPNOTSUPP) /* no such verb or op                      */ \
  X(BadLength, 0x04, -EPROTO)     /* pay_len, or the frame                   */ \
  X(BadField, 0x05, -EPROTO)      /* a flag, reserved byte or window         */ \
  X(NoResource, 0x06, -ENOBUFS)   /* val = the bytes it takes                */ \
  X(Stale, 0x07, -ESTALE)         /* gone: val = epoch now, or the stream    */ \
  X(Conflict, 0x08, -EALREADY)    /* val = the control gen now               */ \
  X(Busy, 0x40, -EALREADY)        /* held by another; detail says who        */ \
  X(NotReady, 0x41, -ENODATA)     /* not now: nothing yet, or mode refuses   */ \
  X(Congested, 0x42, -EAGAIN)     /* nothing taken: ask again shortly        */ \
  X(Quiescing, 0x43, -EAGAIN)     /* val = at most ms until earlier asks end */ \
  X(Resync, 0x44, -EAGAIN)        /* negotiate on this lane, then ask        */ \
  X(Failure, 0x80, -EIO)                                                        \
  X(Timeout, 0x81, -ETIMEDOUT) /* the slice is free, the outcome unknown  */    \
  X(VerifyFailed, 0x82, -EBADMSG)                                               \
  X(Accepted, 0xC0, -EPROTO) /* val = the op id; a row that asked reads it */

#define SMI_X(name, v, err) SmiV2Status_##name = (v),
enum smi_v2_status {
  SMI_V2_STATUS_LIST(SMI_X)
};
#undef SMI_X

enum smi_v2_status_class {
  SmiV2Class_Refused  = 0,
  SmiV2Class_Retry    = 1,
  SmiV2Class_Failed   = 2,
  SmiV2Class_Progress = 3,
};
#define SMI_V2_STATUS_CLASS(s) ((enum smi_v2_status_class)(((s) >> 6) & 0x3u))

/*---- what a repository is, told rather than shaped ------------------------
 * A Describe answer is a chain of records: the {type, len} prefix lets one be
 * added and an older reader skip it. Each body states its baseline beside it. */
enum smi_tlv_type {
  /* 0x02 Caps, 0x03 Features, 0x04 Repos: burned */
  /* 0x05 Versions, 0x06 Limits: burned */
  /* 0x07 Cells: burned */
  SmiTlv_Sensor     = 0x08,
  SmiTlv_BoardField = 0x09,
  SmiTlv_FptHeader  = 0x0A,
  SmiTlv_Partition  = 0x0B,
};

/* One record per instance: a card may gain a sensor, board field or partition
 * and an older host reads those it knows; its kind is SMI_REPO_LIST's payload. */

/* One sensor. Its type is its rid, so nothing here restates it; the reading
 * itself is a published cell, because it changes and these do not. */
struct smi_tlv_sensor {
  uint8_t rid;      /* SmiRepo_*                                    */
  uint8_t id;       /* ordinal within that repository, one based    */
  int8_t  unit_exp; /* the reading is value x 10^unit_exp           */
  uint8_t mask;     /* SmiSensMask_*: which thr[] slots were given  */
  uint8_t kind;     /* SmiSensorKind_*: the column a host may branch on */
  uint8_t rsvd[3];
  int32_t thr[SMI_THR_CNT]; /* indexed by SmiThr_*, the mask's bit position */
  char    name[32];         /* NUL terminated, display only                  */
};
#define SMI_TLV_SENSOR_BASELINE 64u
static_assert(sizeof(struct smi_tlv_sensor) >= SMI_TLV_SENSOR_BASELINE, "Sensor may only grow");

/* One board-identity field, named by its number rather than its place. The
 * numbering follows the identity EEPROM, which is firmware's to grow: a host
 * declares only the numbers it serves and steps over the rest by length. */
struct smi_tlv_board_field {
  uint8_t field; /* the number, firmware's to grow       */
  uint8_t len;   /* valid bytes of body                 */
  uint8_t rsvd[2];
  uint8_t body[32];
};
#define SMI_TLV_BOARD_FIELD_BASELINE 36u
static_assert(sizeof(struct smi_tlv_board_field) >= SMI_TLV_BOARD_FIELD_BASELINE, "BoardField may only grow");

/* One flash device's partition table, followed by its entries. */
struct smi_tlv_fpt_header {
  uint8_t device; /* which boot device                       */
  uint8_t version;
  uint8_t hdr_size;
  uint8_t entry_size;
  uint8_t num_entries; /* Partition records this device has  */
  uint8_t rsvd[3];
};
#define SMI_TLV_FPT_HEADER_BASELINE 8u
static_assert(sizeof(struct smi_tlv_fpt_header) >= SMI_TLV_FPT_HEADER_BASELINE, "FptHeader may only grow");

struct smi_tlv_partition {
  uint8_t  device;
  uint8_t  index;
  uint16_t rsvd;
  uint32_t type;
  uint32_t base;
  uint32_t size;
};
#define SMI_TLV_PARTITION_BASELINE 16u
static_assert(sizeof(struct smi_tlv_partition) >= SMI_TLV_PARTITION_BASELINE, "Partition may only grow");

/* Both flash records open with the device they belong to, so a reader can sort
 * a chain by device before it knows which of the two it is holding. */
static_assert(offsetof(struct smi_tlv_fpt_header, device) == 0, "a flash header names its device first");
static_assert(offsetof(struct smi_tlv_partition, device) == 0, "a partition names its device first");

/*---- frames --------------------------------------------------------------*/

/* A request is a header and a payload within one SQ slot: the payload grows at
 * its end, up to the slot's 32 bytes less the header. */
#define SMI_V2_REQ_HDR_BYTES 16
#define SMI_V2_PAY_MAX       16

struct smi_v2_req_hdr { /* 16 bytes, no bitfields */
  uint16_t opcode;      /* [0,2)   the generation anchor firmware reads */
  uint16_t pay_len;     /* [2,4)   payload bytes that follow: the verb's baseline up to
                         *         SMI_V2_PAY_MAX, a multiple of 4 */
  uint16_t cid;         /* [4,6)   v1's cid halfword: a v1 card refusing this
                         *         frame echoes a cid to match (anchor below) */
  uint8_t  ctx;         /* [6,7)   the sending host context, per lane: 0 = the kernel */
  uint8_t  flags;       /* [7,8)   none defined: nonzero is refused */
  uint32_t win_off;     /* [8,12)  slice offset FROM THE DATA REGION; 0 with no window */
  uint32_t win_len;     /* [12,16) slice length this request owns; 0 with no window */
};

/* Payloads carry only what the verb itself needs: the slice moved to the header,
 * which took v1's addr and its duplicated length with it. */

/* The line between two host sessions on a lane: earlier answers leave first.
 * The host names itself for the card's log. */
struct smi_v2_nego_pay {
  uint32_t nonce;     /* echoed in detail, so an earlier Negotiate's answer cannot pass */
  uint32_t host_ver;  /* the host kernel's version code, 0 if not stated */
  uint32_t drv_build; /* how this host build is named: the head of its
                       * commit, else a packed package version, else 0 */
};

struct smi_v2_ping_pay {
  uint32_t nonce; /* echoed in the answer's val */
};

/* Drop what one host context holds on this lane. */
struct smi_v2_release_pay {
  uint8_t ctx; /* nonzero */
  uint8_t rsvd[3];
};

/* Ask a repository what it holds: a chain of records in the slice, one per
 * instance, of the kind its payload kind decides. Values are published. */
struct smi_v2_desc_pay {
  uint8_t rid; /* SmiRepo_* */
  uint8_t rsvd[3];
};

/* One window of a cage's map. Anti-ABA: the epoch says "the module I read is
 * still seated", control_gen "no other host wrote since I read". The answer's
 * val and win_used are as smi_abi_qsfp.h states beside the op list. */
struct smi_v2_qsfp_pay {
  uint8_t  cage;
  uint8_t  bank;                 /* banked pages (10h and up) only, else 0                      */
  uint8_t  page;                 /* Upper page, never FFh; an offset below 128 is Lower whatever it says */
  uint8_t  offset;               /* the standard's own byte address                             */
  uint8_t  len;                  /* 1..128, whatever the op                                     */
  uint8_t  op;                   /* SmiQsfpOp_*                                                 */
  uint16_t expected_control_gen; /* Write and CdbWrite: a mismatch is Conflict    */
  uint32_t expected_module_epoch;
};

struct smi_v2_led_pay {
  uint8_t  op;    /* SmiLedOp_* */
  uint8_t  id;    /* the LED a force or a release names */
  uint8_t  color; /* ForceOn: 0 green, 1 yellow */
  uint8_t  rsvd;
  uint32_t timeout_s; /* BeaconOn and the forces: 1 to 600 */
};

struct smi_v2_rw_pay {
  uint32_t dev_off; /* offset within the device, not the window */
  uint8_t  rw;      /* SmiRw_*  */
  uint8_t  rsvd[3];
};

/* Flash-write flags. Named bits rather than a bitfield, for the reason the file
 * header gives. */
#define SMI_WR_LAST_PACKET    0x01u /* final packet of the download */
#define SMI_WR_UPDATE_FPT     0x02u /* the image is a partition table */

/* The answer side, in the last packet's val: the image landed with no SHA
 * footer, so nothing was checked. ANS names the side; it never rides flags. */
#define SMI_WR_ANS_UNVERIFIED 0x01u

struct smi_v2_wr_pay {
  uint32_t off; /* this packet's byte offset in the image: a multiple of its Verb's off_align */
  uint8_t  partition;
  uint8_t  device;
  uint8_t  flags; /* SMI_WR_LAST_PACKET | SMI_WR_UPDATE_FPT */
  uint8_t  rsvd;
};

struct smi_v2_cp_pay {
  uint8_t src_device;
  uint8_t src_partition;
  uint8_t dst_device;
  uint8_t dst_partition;
};

struct smi_v2_boot_pay {
  uint8_t partition;
  uint8_t rsvd[3];
};

/*---- log level -- one module, or the global mirror gate --------------------
 * A module is named by the FNV-1a 32 of its tag string: an id is build-local,
 * the tag is what crosses. Over the tag's bytes, terminator excluded, from
 * 2166136261, per byte xor then multiply by 16777619, 32-bit wraparound.
 * Which tags exist is the card's to answer (`log list` prints them). Hash 0
 * is not a module but the global mirror gate, and takes a level 0..4 only.
 *--------------------------------------------------------------------------*/
struct smi_v2_lvl_pay {
  uint32_t name_hash; /* FNV-1a 32 of the module's tag; 0 = the global mirror gate */
  uint8_t  level;     /* 0..4 = hold at that level, 0xFE = follow, 0xFF = off */
  uint8_t  rsvd[3];
};

/* One debug-shell line, run and answered in the slice the request names: the
 * host stages the text at [0, cmd_len) and the output comes back from 0, with
 * win_used the output and val what did not fit -- so a host that guessed too
 * small resubmits at win_used + val. status reports delivery and the tier. */
struct smi_v2_shex_pay {
  uint16_t cmd_len;
  uint16_t rsvd;
};

/* The buffer the card's shell reads a line into, terminator included -- so the
 * longest line a host may send is one less. Stated here so the firmware can
 * assert its own limit against it; the ioctl header, which every tool compiles,
 * carries the host-facing number as a leaf. */
#define SMI_SHELL_LINE_BYTES             256

/* Every verb answers in this shape: a scalar in val, as its status says, more in
 * the slice, with win_used what the card wrote there -- 0 for a write. */
struct smi_v2_rsp {
  uint16_t cid;      /* bytes [0,2): the anchor the host matches on   */
  uint16_t status;   /* SmiV2Status_*; halfword [2,4) is also what a
                      * probe reads -- v1 puts a non-zero completion
                      * state there, v2 puts Success (0)             */
  uint32_t win_used; /* bytes the card wrote into the slice           */
  uint32_t val;      /* the one scalar a verb may answer with         */
  /* A firmware syndrome for the log: a kernel prints it and branches on
   * nothing. What a verb answers belongs in val. */
  uint32_t detail;
};
/* v2. Sizes first, then the two anchors, then what the shapes promise about
 * each other. Bit order needs no assert here: v2 has no bitfield. */
static_assert(sizeof(struct smi_v2_req_hdr) == SMI_V2_REQ_HDR_BYTES, "v2 request header is 16 bytes");
static_assert(sizeof(struct smi_v2_rsp) == SMI_COMPLETION_FRAME_BYTES, "the CQ entry IS the v2 answer");
/* The generation anchors, v2 half: firmware reads the request opcode and the
 * host reads the answer cid before either knows anything else, so both sit at
 * byte 0. The halfword after the answer's cid is what a probe reads. */
static_assert(offsetof(struct smi_v2_req_hdr, opcode) == 0, "the v2 opcode is the first halfword");
/* The anchor the probe depends on: a request's cid sits at [4,6) in BOTH
 * generations, because a v1 card refuses an unknown opcode by echoing the cid
 * from the halfword it keeps it in -- anywhere else and the refusal carries a
 * cid the host cannot match. */
static_assert(offsetof(struct smi_v2_req_hdr, cid) == 4, "a v2 request's cid is at [4,6)");
static_assert(offsetof(struct smi_v2_rsp, cid) == 0, "cid occupies bytes [0,2) of a v2 answer");
static_assert(offsetof(struct smi_v2_rsp, status) == 2, "the probe reads the halfword at [2,4)");
/* v1 puts its completion state in that same halfword (a bitfield, so offsetof
 * cannot say so here) and a refused probe carries Invalid there, which is
 * non-zero. That is the whole probe rule: zero means v2 answered. */

/* The window slice is in the header, at the same offsets for every verb -- that
 * is what lets the dispatcher bound it once. */
static_assert(offsetof(struct smi_v2_req_hdr, win_off) == 8, "win_off");
static_assert(offsetof(struct smi_v2_req_hdr, win_len) == 12, "win_len");

/* What this generation promises about the anchors and the probe. */
static_assert(offsetof(struct smi_v2_req_hdr, cid) == SMI_REQ_CID_OFF, "the request cid anchor");
static_assert(offsetof(struct smi_v2_rsp, cid) == SMI_RSP_CID_OFF, "the answer cid anchor");
static_assert(offsetof(struct smi_v2_rsp, status) == SMI_RSP_PROBE_OFF, "the probe halfword");
static_assert((int)SmiV2Status_Success == SMI_PROBE_DONE, "success reads as done");
static_assert((int)SmiV2Status_BadOpcode == SMI_PROBE_NO_VERB, "bad opcode reads as no-such-verb");

#ifdef __cplusplus
}
#endif
#endif /* SMI_ABI_V2_H */
