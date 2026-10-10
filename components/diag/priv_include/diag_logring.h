/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Log ring (pure logic, no IDF or RTOS includes, host-testable).
 *
 * A byte ring of log records in one block of memory that survives software,
 * panic and watchdog resets (PSRAM .noinit on the target). diag.c keeps two
 * such blocks: the one this boot writes, and the previous boot's, frozen at
 * start-up. Locking and the cache write-back are the caller's: the ring
 * calls `sync` for every range it has made final, in an order that keeps the
 * stored copy valid if a reset hits between two calls.
 *
 * Memory layout (all multi-byte fields little-endian, the CPU's order):
 *
 *   header (logring_hdr_t, 56 bytes), then the data area (the rest).
 *
 *   magic      "DLOG" (0x474F4C44)
 *   layout     LOGRING_LAYOUT; another value is a different layout: discard
 *   hdr_size   sizeof(logring_hdr_t)
 *   data_size  bytes in the data area (must match the block this image uses)
 *   boot_id    1, 2, 3, ... one per boot that started a block
 *   first_seq  sequence number of the oldest record held
 *   next_seq   sequence number the next record gets (next_seq - first_seq
 *              records are held)
 *   tail       offset of the oldest record in the data area
 *   used       bytes held, record headers included
 *   lost       records dropped to make room since the block was started
 *   bank       which of the caller's blocks wrote it (logring_set_owner)
 *   image      8 bytes naming the program image that wrote it
 *   crc        CRC-32 over the fields above
 *
 *   A block whose bank or image differs from its reader's is refused: when
 *   the program image changes, the memory the blocks live in can move (on
 *   the target by whole MMU pages, which can equal one block), so the old
 *   copy of another block, or of another image, could otherwise pass for
 *   this one.
 *
 *   Record: u32 seq, u16 len, u16 check (CRC-16 over seq, len and the
 *   text), then `len` bytes of text with no terminator. Records are stored
 *   back to back and wrap around the end of the data area byte by byte.
 *   One record is one call of the log output function: a whole line,
 *   newline included, for the default log format.
 *
 * Writes: the record goes in first and is synced, then the header; when old
 * records must be dropped to make room, the header that drops them is
 * written and synced before their bytes are overwritten. So the header in
 * memory never points at a half-written or overwritten record.
 *
 * Recovery (at boot): a block whose header fails any check is discarded.
 * Otherwise the records are walked from the tail; the first one whose
 * sequence number, length or check is wrong ends the ring there (a record
 * cut short by the reset), and the header is rewritten to match.
 *
 * Readers pass a cursor: the sequence number of the last record they have.
 * Sequence numbers keep counting across boots while the previous block was
 * valid, so a cursor from another boot is either older than the oldest
 * record (reported as a gap) or newer than the newest (restart from the
 * oldest, also reported as a gap).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOGRING_MAGIC    0x474F4C44u   /* "DLOG" */
#define LOGRING_LAYOUT   2u
#define LOGRING_REC_HDR  8u            /* u32 seq, u16 len, u16 check */
#define LOGRING_MAX_TEXT 1024u         /* longer records are cut to this */
#define LOGRING_IMAGE_ID 8u            /* bytes naming the program image */
#define LOGRING_HINTS    8u            /* read positions remembered (RAM only) */

typedef struct {
    uint32_t magic;
    uint32_t layout;
    uint32_t hdr_size;
    uint32_t data_size;
    uint32_t boot_id;
    uint32_t first_seq;
    uint32_t next_seq;
    uint32_t tail;
    uint32_t used;
    uint32_t lost;
    uint32_t bank;
    uint8_t image[LOGRING_IMAGE_ID];
    uint32_t crc;
} logring_hdr_t;

/** Make [addr, addr + len) durable (target: data-cache write-back). */
typedef void (*logring_sync_fn)(void *ctx, const void *addr, size_t len);

typedef struct {
    logring_hdr_t *hdr;
    uint8_t *data;
    uint32_t data_size;
    logring_sync_fn sync;      /**< optional */
    void *sync_ctx;
    uint32_t bank;             /**< owner this reader expects and writes (logring_set_owner) */
    uint8_t image[LOGRING_IMAGE_ID];
    /* Where reads stopped: record hint_seq[i] starts at hint_off[i]. A held
     * record never moves, so a hint stays right while its record is held
     * (seq >= first_seq); a reader that continues where it stopped starts
     * there instead of walking every record from the oldest. */
    uint32_t hint_seq[LOGRING_HINTS];   /**< 0 = unused */
    uint32_t hint_off[LOGRING_HINTS];
    uint32_t hint_next;
    uint32_t seek_steps;       /**< records walked to find read positions (statistics) */
} logring_t;

/** What a read found. */
typedef struct {
    bool gap;          /**< records the reader had not seen were overwritten (or the cursor was from another boot) */
    bool more;         /**< more records follow: read again */
    uint32_t records;  /**< records copied */
} logring_read_t;

/**
 * Point `r` at a block of memory: header first, data area after it.
 * `mem_size` must exceed the header by at least LOGRING_REC_HDR + 1 bytes.
 * Nothing in the block is changed.
 */
void logring_attach(logring_t *r, void *mem, size_t mem_size, logring_sync_fn sync, void *sync_ctx);

/**
 * Owner of the block: which of the caller's blocks it is, and the program
 * image (LOGRING_IMAGE_ID bytes). logring_format() stores them and the
 * header checks require them. Both are 0 after logring_attach().
 */
void logring_set_owner(logring_t *r, uint32_t bank, const uint8_t image[LOGRING_IMAGE_ID]);

/** True if the header passes every check for this block (magic, layout, sizes, owner, CRC, offsets). */
bool logring_header_ok(const logring_t *r);

/**
 * Validate a block left by an earlier boot: false if the header is not
 * usable (start it over with logring_format). Otherwise keep the records up
 * to the first damaged one, rewrite the header to match, and return true.
 */
bool logring_recover(logring_t *r);

/** Start an empty ring. `first_seq` (>= 1) is the sequence number of its first record. */
void logring_format(logring_t *r, uint32_t boot_id, uint32_t first_seq);

/**
 * Append one record (text without terminator, cut to LOGRING_MAX_TEXT and
 * to what the data area can hold). Drops the oldest records as needed.
 * @return the record's sequence number (0 if len is 0: nothing stored).
 */
uint32_t logring_append(logring_t *r, const char *text, size_t len);

/** Records currently held. */
uint32_t logring_count(const logring_t *r);

/**
 * Copy whole records with seq > *cursor, oldest first, into out
 * (NUL-terminated; at most cap - 1 text bytes). *cursor becomes the last
 * copied record's sequence number. A record longer than cap - 1 that would
 * be the first one copied is cut to fit, so a reader whose whole buffer is
 * smaller than one record still advances.
 * @return text bytes copied.
 */
size_t logring_read(logring_t *r, uint32_t *cursor, char *out, size_t cap, logring_read_t *info);

/** Lock or unlock the ring for one slice of logring_read_sliced (optional). */
typedef void (*logring_lock_fn)(void *ctx);

/**
 * logring_read into a large buffer, at most `slice` bytes (raised to one
 * longest record plus its terminator) per lock hold, so a long read never
 * keeps a writer waiting for long. Only whole records are copied: a record
 * is cut only when the whole buffer cannot hold it (`size` - 1 bytes), never
 * because a slice ended inside it. info->records counts every record copied.
 * @return text bytes copied (out is NUL-terminated).
 */
size_t logring_read_sliced(logring_t *r, uint32_t *cursor, char *out, size_t size, size_t slice,
                           logring_lock_fn lock, logring_lock_fn unlock, void *lock_ctx, logring_read_t *info);

#ifdef __cplusplus
}
#endif
