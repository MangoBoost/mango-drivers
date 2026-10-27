// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * smi_abi_qsfp.h - the QSFP cage contract: a cage's published block and what a host asks of it
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

/* The part of the wire contract a host reads as it is: the driver passes a cage's
 * block through unread (qsfp<N>_raw) and QsfpRw's op and power byte unchanged, so
 * its UAPI includes this file alone. smi_abi.h includes it, and its laws hold here. */

#ifndef SMI_ABI_QSFP_H
#define SMI_ABI_QSFP_H

#ifdef __KERNEL__
  #include <linux/build_bug.h> /* static_assert */
  #include <linux/stddef.h>    /* offsetof     */
  #include <linux/types.h>
#else
  #include <stddef.h> /* offsetof */
  #include <stdint.h>
  #ifndef __cplusplus
    #include <assert.h>  /* static_assert, from C11 */
    #include <stdbool.h> /* the link record helpers answer bool */
  #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* What a cage is doing, in one word a reader may branch on. Only "is it Ready"
 * is judged; the rest is display. Append only, and a number is never reused.
 * Columns: name * wire value. */
#define SMI_QSFP_STATE_LIST(X)                                                \
  X(Empty, 0)        /* no cable: images, sticky and pages_valid are zero  */ \
  X(Initializing, 1) /* seated, first inventory running: trust nothing     */ \
  X(Ready, 2)        /* inventory committed, live span polling            */  \
  X(Failed, 3)       /* seated, unusable: reads retry, power awaits reset */

#define SMI_X(name, v) SmiQsfpState_##name = (v),
enum smi_qsfp_state {
  SMI_QSFP_STATE_LIST(SMI_X)
};
#undef SMI_X

/* The live span, format 1's floor: a poll that raises sample_seq has re-read all of these bytes of
 * a Ready cage's shown bank, at the standard's addresses (11h only on a module that has it). The
 * floor never moves in format 1; a card may read more, and a host relies on nothing past it. */
#define SMI_QSFP_LIVE_CMIS_LOWER_FIRST 3u
#define SMI_QSFP_LIVE_CMIS_LOWER_LAST  41u
#define SMI_QSFP_LIVE_CMIS_11H_FIRST   128u
#define SMI_QSFP_LIVE_CMIS_11H_LAST    235u
#define SMI_QSFP_LIVE_SFF_LOWER_FIRST  2u
#define SMI_QSFP_LIVE_SFF_LOWER_LAST   57u

/* The prefix every published section opens with, here as this file stands
 * alone; where and how long a section is, only its Region record says. */
struct smi_pub_sect {
  uint16_t fmt;     /* the seats and meanings in use; 0 = not written yet */
  uint16_t hdr_len; /* head bytes, this prefix included */
  uint32_t seq;     /* odd while the section is being written */
  uint32_t boot;    /* the boot that wrote it: the descriptor's Boot record */
};

/* One cage, unasked: the standard's pages verbatim behind a header of what no
 * standard says (seating, sample, seats, links, power), kernel part first. */
struct smi_pub_qsfp_cage {
  struct smi_pub_sect sect;           /* fmt = SMI_QSFP_FMT; hdr_len is where image[] starts */
  uint32_t            module_epoch;   /* [31:16] the boot's low half, [15:0] +1 per present rise or inventory commit */
  uint8_t             state;          /* SmiQsfpState_* */
  uint8_t             fail_reason;    /* SmiQsfpFail_*; display, never branch */
  uint16_t            control_gen;    /* +1 per Write/CdbWrite taken, landed or not */
  uint32_t            sample_seq;     /* +1 per poll that refreshed the live span */
  uint32_t            sample_time_ms; /* uptime at that poll */
  uint8_t             flag_seq;       /* +1 per new-flag byte; wraps, compared as != */
  uint8_t             bank;           /* the bank the banked slots show */
  uint8_t             img_slots;      /* image cells, and page_id entries */
  uint8_t             sticky_slots;   /* sticky cells, and sticky_slot entries */
  uint16_t            sticky_off;     /* to sticky[] */
  uint16_t            poll_err;       /* live and inventory read failures, saturating */
  uint16_t            poll_ms;        /* live span period, so a watcher compiles none */
  uint16_t            tbl_off;        /* to the tail table, whose growth moves nothing above it */
  uint8_t             link_mask;      /* bit i: link i runs through this cage, empty or not */
  uint8_t             power;          /* SmiQsfpPower_*, this seating's policy */
  uint16_t            rsvd;
  /* hdr_len    image[img_slots][128], the pages verbatim
   * sticky_off sticky[sticky_slots][128], same addresses, latched bytes OR-ed
   * tbl_off    page_id[img_slots] ([0] is Lower), pages_valid,
   *            sticky_slot[sticky_slots] and writable_slots, in that order;
   *            an unused entry of the two slot arrays is SMI_QSFP_TBL_UNUSED */
};

/* Host ops on one cage; append only, never reused. Columns: name * wire value.
 * - an op at or past SmiQsfpOp_Max is one the card lacks, answered BadOpcode
 * - an operand the op reads is refused out of range, never ignored
 * - Lower reads no page byte; only Write and CdbWrite read control_gen */
#define SMI_QSFP_OP_LIST(X)                                                \
  X(Read, 0)      /* the window takes the bytes                         */ \
  X(Write, 1)     /* the window carries them, and the cage reads back   */ \
  X(AckSticky, 2) /* the window is a mask: the bits a host has seen     */ \
  X(Power, 3)     /* the window is one SMI_QSFP_POWER_LIST byte         */ \
  X(CdbWrite, 4)  /* the window is one CDB page; 9Fh's command id last  */

#define SMI_X(name, v) SmiQsfpOp_##name = (v),
enum smi_qsfp_op {
  SMI_QSFP_OP_LIST(SMI_X)
  /* one past the last op, so the list alone moves it */
  SmiQsfpOp_Max
};
#undef SMI_X

/* What an answer carries beside its status:
 * - win_used: the window bytes the card wrote, all of them for an op that fills it (Read) and
 *   none for any other; only those are valid
 * - val: the epoch on Stale and NotReady and after a Power (a Reset's new one); the control_gen
 *   on Conflict, the new one after Write or CdbWrite; else 0 */

/* The CDB command page: a CdbWrite of it starts the command its id names. */
#define SMI_QSFP_CDB_PAGE 0x9Fu

/* A Power op: HighPower and LowPower set the seating's LPMode policy, Reset
 * re-runs power-up under it; 0 = none, refused. Columns: name * wire value. */
#define SMI_QSFP_POWER_LIST(X) \
  X(HighPower, 1)              \
  X(LowPower, 2)               \
  X(Reset, 3)

#define SMI_X(name, v) SmiQsfpPower_##name = (v),
enum smi_qsfp_power {
  SMI_QSFP_POWER_LIST(SMI_X)
  /* one past the last policy, so the list alone moves it */
  SmiQsfpPower_Max
};
#undef SMI_X

/* Why a cage is Failed: a host displays the word and never branches on it, and
 * an unknown value prints as its number. Columns: name * value. */
#define SMI_QSFP_FAIL_LIST(X)                                                \
  X(None, 0)       /* not failed                                          */ \
  X(NoPower, 1)    /* never acknowledged the power-up request             */ \
  X(NotReady, 2)   /* the readiness gate did not pass inside its budget   */ \
  X(BadEeprom, 3)  /* the identifier is not one this firmware serves      */ \
  X(I2cStuck, 4)   /* the bus stopped moving and a deadline ran out       */ \
  X(ReadFailed, 5) /* plain read failures, retried and still failing      */

#define SMI_X(name, v) SmiQsfpFail_##name = (v),
enum smi_qsfp_fail {
  SMI_QSFP_FAIL_LIST(SMI_X)
};
#undef SMI_X

/* One block must fit a single sysfs pread(), so it stays under the smallest
 * PAGE_SIZE any host has. Firmware asserts its stride against it; a driver
 * asserts it against PAGE_SIZE. */
#define SMI_QSFP_BLK_MAX      4096u
/* The block's format. It rises only when a seat or a meaning changes, never
 * when the header grows at its end. */
#define SMI_QSFP_FMT          1u
/* Format 1's header floor, the whole header: a reader refuses a shorter hdr_len
 * and copies min(hdr_len, sizeof) of what it does get. */
#define SMI_QSFP_HDR_BASELINE 44u
/* One image or sticky cell: a page of the standard. */
#define SMI_QSFP_PAGE_BYTES   128u
/* An unused entry of page_id[] and sticky_slot[]: a cell with no page, a shadow with no cell */
#define SMI_QSFP_TBL_UNUSED   0xFFu

/* Folded where a C++ consumer asserts against them; the kernel's C has no constexpr */
#ifdef __cplusplus
  #define SMI_FOLD constexpr
#else
  #define SMI_FOLD static inline
#endif

/* The tail table's arrays, as offsets from tbl_off, and where it ends. When the tail first grows,
 * the header gains tbl_len (0 = format 1's four arrays): an array exists if it ends within it. */
SMI_FOLD uint32_t smi_qsfp_slot_bits(uint8_t img_slots) /* bytes of pages_valid or writable_slots */
{
  return ((uint32_t)img_slots + 7u) / 8u;
}
SMI_FOLD uint32_t smi_qsfp_tbl_valid_at(uint8_t img_slots) /* after page_id[img_slots] */
{
  return img_slots;
}
SMI_FOLD uint32_t smi_qsfp_tbl_sticky_at(uint8_t img_slots)
{
  return smi_qsfp_tbl_valid_at(img_slots) + smi_qsfp_slot_bits(img_slots);
}
SMI_FOLD uint32_t smi_qsfp_tbl_writable_at(uint8_t img_slots, uint8_t sticky_slots)
{
  return smi_qsfp_tbl_sticky_at(img_slots) + sticky_slots;
}
/* Where format 1's four arrays end, and stay ending: a grown tail ends at tbl_len */
SMI_FOLD uint32_t smi_qsfp_tbl_bytes(uint8_t img_slots, uint8_t sticky_slots)
{
  return smi_qsfp_tbl_writable_at(img_slots, sticky_slots) + smi_qsfp_slot_bits(img_slots);
}

/* How much of a header a reader trusts: hdr_len of it, and no more than this build knows */
SMI_FOLD uint32_t smi_qsfp_hdr_bytes(const struct smi_pub_qsfp_cage *h)
{
  return (h->sect.hdr_len < sizeof(struct smi_pub_qsfp_cage)) ? h->sect.hdr_len
                                                              : (uint32_t)sizeof(struct smi_pub_qsfp_cage);
}
/* Whether a header lays its arrays inside a block of @size bytes: the floor, then each array's end */
SMI_FOLD bool smi_qsfp_block_fits(const struct smi_pub_qsfp_cage *h, uint32_t size)
{
  return (h->sect.hdr_len >= SMI_QSFP_HDR_BASELINE)
      && (((uint32_t)h->sect.hdr_len + ((uint32_t)h->img_slots * SMI_QSFP_PAGE_BYTES)) <= size)
      && (((uint32_t)h->sticky_off + ((uint32_t)h->sticky_slots * SMI_QSFP_PAGE_BYTES)) <= size)
      && (((uint32_t)h->tbl_off + smi_qsfp_tbl_bytes(h->img_slots, h->sticky_slots)) <= size);
}

/* One QSFP link record, the sensor value IOC_SENSOR_TYPE_QSFP answers per link:
 * - [3:0] the cage backing the link, [4] a cable detected in it
 * - [7:5] reserved, [15:8] the link state (1 up, 0 down), [31:16] its speed in Gbps
 * A reserved bit or another state is a newer format, which a reader shows as unknown. */
#define SMI_QSFP_LINK_CAGE_MASK   0x0000000Fu
#define SMI_QSFP_LINK_CABLE       0x00000010u
#define SMI_QSFP_LINK_RSVD_MASK   0x000000E0u
#define SMI_QSFP_LINK_STATE_MASK  0x0000FF00u
#define SMI_QSFP_LINK_STATE_SHIFT 8u
#define SMI_QSFP_LINK_UP          1u
#define SMI_QSFP_LINK_SPEED_SHIFT 16u

SMI_FOLD uint32_t smi_qsfp_link_state(uint32_t rec)
{
  return (rec & SMI_QSFP_LINK_STATE_MASK) >> SMI_QSFP_LINK_STATE_SHIFT;
}
SMI_FOLD bool smi_qsfp_link_known(uint32_t rec)
{
  return (0u == (rec & SMI_QSFP_LINK_RSVD_MASK)) && (smi_qsfp_link_state(rec) <= SMI_QSFP_LINK_UP);
}
SMI_FOLD bool smi_qsfp_link_up(uint32_t rec) /* whatever the cable bit says */
{
  return smi_qsfp_link_known(rec) && (SMI_QSFP_LINK_UP == smi_qsfp_link_state(rec));
}
#undef SMI_FOLD

/* The block's seats, which a host reaches by arithmetic; checked wherever static_assert
 * exists (C11, C++, the kernel), so a C99 program can still include the UAPI */
#if defined(static_assert) || defined(__cplusplus)
static_assert(sizeof(struct smi_pub_sect) == 12, "a section prefix: fmt, hdr_len, seq, boot");
static_assert(offsetof(struct smi_pub_sect, fmt) == 0 && offsetof(struct smi_pub_sect, seq) == 4,
              "every section opens with its format, and its seq is one aligned word");
static_assert(offsetof(struct smi_pub_qsfp_cage, sect) == 0, "a cage block is a section");
static_assert(sizeof(struct smi_pub_qsfp_cage) >= SMI_QSFP_HDR_BASELINE, "the header grows at its end only");
static_assert(offsetof(struct smi_pub_qsfp_cage, module_epoch)
                  == 12
                  && offsetof(struct smi_pub_qsfp_cage, state)
                  == 16
                  && offsetof(struct smi_pub_qsfp_cage, control_gen)
                  == 18
                  && offsetof(struct smi_pub_qsfp_cage, tbl_off)
                  == 38
                  && offsetof(struct smi_pub_qsfp_cage, link_mask)
                  == 40
                  && offsetof(struct smi_pub_qsfp_cage, power)
                  == 41,
              "format 1 seats");
#endif

#ifdef __cplusplus
}
#endif
#endif /* SMI_ABI_QSFP_H */
