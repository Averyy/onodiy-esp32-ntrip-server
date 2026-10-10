/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Event journal on a raw flash partition (pure logic, no IDF or RTOS
 * includes, host-testable). Flash access goes through callbacks; locking,
 * the write queue and the writer task are diag_journal.c's.
 *
 * Flash format (version 1; all fields little-endian):
 *
 *   The partition is a ring of 4 KB sectors. Each sector holds 128 slots of
 *   32 bytes: slot 0 is the sector header, slots 1..127 hold records in
 *   the order they were written. A slot that reads all 0xFF is free.
 *
 *   Sector header (slot 0):
 *     0  u32 magic          "DJNL" (0x4C4E4A44)
 *     4  u16 hdr_format     1. This header layout is frozen: a later
 *                           firmware may change the record layout, never
 *                           this one, so every version can follow the ring
 *     6  u16 rec_format     layout of this sector's records (1 = below);
 *                           sectors with an unknown one are skipped when
 *                           reading, but keep their place in the ring
 *     8  u32 sector_seq     1, 2, 3, ...: +1 every time a sector is opened;
 *                           the highest is the sector being written
 *    12  u32 first_rec_seq  sequence number of this sector's first record
 *    16  u16 rec_size       32
 *    18  10 bytes 0xFF      reserved
 *    28  u32 crc            CRC-32 over bytes 0..27
 *
 *   Record (rec_format 1):
 *     0  u32 seq            1, 2, 3, ... in write order, never reused
 *     4  u32 uptime_s       seconds since that boot started
 *     8  i64 unix_s         UTC seconds, 0 = no trusted clock at the time
 *    16  u16 kind           diag_event_t
 *    18  u16 reserved       0
 *    20  i32 a
 *    24  i32 b
 *    28  u32 crc            CRC-32 over bytes 0..27
 *   A slot that is neither free nor a valid record (a write cut by a power
 *   loss) is skipped and never reused until its sector is erased.
 *
 * Wear: sectors are used strictly in ring order and each is erased only when
 * the ring comes back to it (never ahead of time, and not at all when it
 * still reads blank), so every sector sees the same number of erases. The
 * number of sectors opened so far (the newest sector_seq) divided by the
 * sector count is the number of erase cycles each sector has seen. The rate
 * of records is bounded by the caller (per-kind interval and an hourly
 * budget, dj_rate_*).
 *
 * Mount: read every sector header; the valid one with the highest
 * sector_seq is current; its slots are scanned for the first free one. The
 * next sequence number follows the highest found (or the sector's
 * first_rec_seq). A sector written in an unknown record format counts as
 * full. A partition without any valid header is empty: the first append
 * opens sector 0.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DJ_MAGIC            0x4C4E4A44u   /* "DJNL" */
#define DJ_HDR_FORMAT       1u
#define DJ_REC_FORMAT       1u
#define DJ_SECTOR_SIZE      4096u
#define DJ_SLOT_SIZE        32u
#define DJ_SLOTS            (DJ_SECTOR_SIZE / DJ_SLOT_SIZE)  /* 128, slot 0 = header */
#define DJ_RECS_PER_SECTOR  (DJ_SLOTS - 1u)
#define DJ_MAX_SECTORS      64u                              /* 256 KB */

/** Errors (0 = success). */
#define DJ_OK         0
#define DJ_ERR_IO     (-1)    /**< a flash call failed */
#define DJ_ERR_SIZE   (-2)    /**< partition too small or too large */
#define DJ_ERR_STATE  (-3)    /**< not mounted */

/** Flash access: offsets relative to the partition; each returns 0 on success. */
typedef struct {
    int (*read)(void *ctx, uint32_t off, void *buf, size_t len);
    int (*write)(void *ctx, uint32_t off, const void *buf, size_t len);
    int (*erase)(void *ctx, uint32_t off, size_t len);   /**< whole sectors */
    void *ctx;
} dj_flash_t;

/** One record. */
typedef struct {
    uint32_t seq;
    uint32_t uptime_s;
    int64_t unix_s;
    uint16_t kind;
    int32_t a;
    int32_t b;
} dj_entry_t;

/** What the index knows about one sector. */
typedef struct {
    bool valid;            /**< header checked out */
    uint16_t rec_format;
    uint32_t sector_seq;
    uint32_t first_seq;
} dj_sector_t;

typedef struct {
    dj_flash_t fl;
    uint32_t sectors;
    bool mounted;
    int32_t cur;           /**< sector being written, -1 = none yet */
    uint32_t cur_slot;     /**< next free slot in cur (DJ_SLOTS = full) */
    uint32_t next_seq;
    uint32_t top_sector_seq;
    dj_sector_t sec[DJ_MAX_SECTORS];
    /* statistics since mount */
    uint32_t written;
    uint32_t erases;
    uint32_t io_errors;
    uint32_t bad_slots;    /**< unreadable slots seen by the mount scan */
} dj_t;

/** Encode/decode one 32-byte record. Decode: 1 valid, 0 free slot, -1 damaged. */
void dj_encode_record(const dj_entry_t *e, uint8_t out[DJ_SLOT_SIZE]);
int dj_decode_record(const uint8_t in[DJ_SLOT_SIZE], dj_entry_t *e);

/** Encode/decode a sector header. Decode: 1 valid, 0 free slot, -1 not a header of ours. */
void dj_encode_sector_hdr(uint32_t sector_seq, uint32_t first_seq, uint16_t rec_format, uint8_t out[DJ_SLOT_SIZE]);
int dj_decode_sector_hdr(const uint8_t in[DJ_SLOT_SIZE], dj_sector_t *s);

/** Read the partition's index. `size` must be a whole number of sectors, 2..DJ_MAX_SECTORS of them. */
int dj_mount(dj_t *j, const dj_flash_t *fl, uint32_t size);

/**
 * Write one record; e->seq is assigned here. Opens (erases, if needed) the
 * next sector when the current one is full.
 */
int dj_append(dj_t *j, dj_entry_t *e);

/** True if the next append has to open (and maybe erase) a sector first. */
bool dj_needs_open(const dj_t *j);

/**
 * Records with seq > *cursor, oldest first, at most cap; *cursor becomes the
 * last one's seq. *more (optional) = records remain.
 * @return number of records (0 also when unmounted).
 */
size_t dj_read(dj_t *j, uint32_t *cursor, dj_entry_t *out, size_t cap, bool *more);

/** Oldest sequence number still held (0 if empty). */
uint32_t dj_oldest_seq(const dj_t *j);

/** Records the ring can hold. */
uint32_t dj_capacity(const dj_t *j);

/* ---- rate limits ------------------------------------------------------------------ */

#define DJ_RATE_KINDS 32u   /**< kinds 0..31 are tracked separately; higher ones share the last entry */

/** Result of a rate check. */
typedef enum {
    DJ_RATE_OK = 0,
    DJ_RATE_KIND,          /**< same kind too soon */
    DJ_RATE_BUDGET,        /**< the hourly budget is used up */
} dj_rate_result_t;

/**
 * Per-kind minimum interval plus an hourly budget (a token bucket that holds
 * at most ten minutes' worth). Kinds in `exempt_mask` (bit = kind) skip both
 * limits but still use a token when one is left, so they count toward the
 * budget of the others.
 */
typedef struct {
    uint32_t min_interval_ms;
    uint32_t per_hour;
    uint32_t exempt_mask;
    uint32_t last_ms[DJ_RATE_KINDS];
    uint32_t seen_mask;
    uint64_t tokens_milli;  /**< thousandths of a record */
    uint64_t burst_milli;   /**< bucket size */
    uint32_t refill_ms;     /**< time of the last refill */
    uint32_t refill_rem;    /**< remainder of the last refill (no drift from rounding) */
    bool started;
    uint32_t refused_kind;
    uint32_t refused_budget;
} dj_rate_t;

/** `per_hour` 0 = no budget (per-kind interval only). The bucket starts full. */
void dj_rate_init(dj_rate_t *r, uint32_t min_interval_ms, uint32_t per_hour, uint32_t exempt_mask);

/** Check (and, if accepted, account) one record of `kind` at `now_ms` (wraps freely). */
dj_rate_result_t dj_rate_check(dj_rate_t *r, uint16_t kind, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
