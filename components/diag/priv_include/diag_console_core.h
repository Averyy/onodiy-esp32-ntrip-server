/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Console writer (pure logic, no IDF or RTOS includes, host-tested).
 *
 * Every log line is appended to this boot's log ring (diag_logring.h)
 * before the console sees it, so the console sends the ring's records in
 * their order and never needs a copy of its own:
 *  - Fast path: a logger hands its own line straight to the UART's transmit
 *    buffer only when nothing older waits (the line is the record right
 *    after the console's cursor) and the whole line fits the free room.
 *    Lines then reach the UART in the order of their records, also with
 *    printf output in between.
 *  - Otherwise the line waits in the ring. The writer (diag's low-priority
 *    console task, and a planned restart's drain) copies records from its
 *    own cursor in batches (dcon_pump) and hands over what fits the free
 *    room, whole lines where possible, blocking nobody but itself.
 *  - Lines are lost only when the ring overwrote records the console had
 *    not sent. The writer sees that as a jump in sequence numbers and says
 *    so exactly once per gap with "[diag: N console lines dropped]", always
 *    at the start of a line: when the last byte handed over did not end a
 *    line (a library that logs one line in fragments), a '\n' comes first.
 *    Records are never split by the console itself, so a fragmented line
 *    stays contiguous unless its own continuation was lost.
 *
 * The bytes of lost records are counted exactly: the ring keeps the text
 * bytes of every record it dropped (logring_t.dropped_text), the console
 * the text bytes of every record up to its cursor; the difference at a gap
 * is what was lost.
 *
 * Locking is the caller's: one mutex around the fast path and every pump
 * (dcon_t), the ring's own lock around each read (passed to dcon_pump).
 *
 * Text that is not a log line (diag_console_write: the bench console's
 * replies) goes to the UART under that same mutex, whole, with the same line
 * endings, so a log line can never land inside one of its lines; when the
 * console stopped inside a line, a line break comes first
 * (dcon_other_text). Such text written in pieces (a reply printed in parts)
 * is gathered first and handed over a whole line at a time (dcon_lines_t).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "diag_logring.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Room reserved in front of a batch for the "lines dropped" note. */
#define DCON_NOTE_MAX 64u

/** Batch buffer size dcon_init needs: the note plus one longest record and its terminator. */
#define DCON_BUF_SIZE (DCON_NOTE_MAX + LOGRING_MAX_TEXT + 1u)

/** The UART side, as the writer sees it. */
typedef struct {
    /** Free bytes in the transmit buffer now. */
    size_t (*room)(void *ctx);
    /** Hand over len text bytes (line endings are converted by the callee; they fit the room the core checked). */
    void (*write)(void *ctx, const char *s, size_t len);
    void *ctx;
    size_t total;     /**< transmit buffer size (a line longer than this can never fit whole) */
    size_t margin;    /**< room kept free per write (the buffer's own item headers) */
    bool crlf;        /**< every '\n' becomes "\r\n" on the wire (counts double) */
} dcon_tx_t;

typedef struct {
    uint32_t cursor;          /**< sequence number of the last record handed over or found lost */
    uint64_t bytes;           /**< text bytes of every record up to the cursor, sent or lost (this boot's ring) */
    bool mid_line;            /**< the last byte handed over did not end a line */
    uint32_t dropped;         /**< records lost: overwritten in the ring before they were sent */
    uint64_t dropped_bytes;
    /* the batch being handed over */
    char *buf;
    size_t cap;
    const char *start;        /**< note (if any) followed by the records' text */
    size_t len;               /**< bytes of the batch (0 = none) */
    size_t off;               /**< bytes of it handed over */
    uint32_t batch_cursor;    /**< cursor once the batch is handed over */
    uint64_t batch_bytes;     /**< bytes once the batch is handed over */
} dcon_t;

/**
 * Start: `cursor` = the sequence number before the first record the console
 * is to send (the ring's next_seq - 1 when the console comes up), `buf` a
 * batch buffer of at least DCON_BUF_SIZE bytes.
 */
void dcon_init(dcon_t *c, uint32_t cursor, char *buf, size_t cap);

/** Bytes on the wire for `len` text bytes. */
size_t dcon_wire_len(const char *s, size_t len, bool crlf);

/**
 * Fast path: may record `seq` (wire_len bytes on the wire) go to the UART now?
 * Only when no batch is being handed over, it is the record right after the
 * cursor, and it fits `room` with the margin.
 */
bool dcon_direct_ok(const dcon_t *c, uint32_t seq, size_t wire_len, size_t room, size_t margin);

/** The fast path handed record `seq` over (its text and length as stored in the ring). */
void dcon_sent(dcon_t *c, uint32_t seq, const char *text, size_t len);

typedef enum {
    DCON_IDLE = 0,    /**< caught up: nothing waits */
    DCON_MORE,        /**< handed something over; more may follow at once */
    DCON_BUSY,        /**< something waits, but the transmit buffer has no room for it now */
} dcon_pump_t;

/** What one pump found lost (for the caller's counters). */
typedef struct {
    uint32_t lost;
    uint64_t lost_bytes;
} dcon_loss_t;

/**
 * One writer step: with no batch in flight, read the records after the
 * cursor (whole records, under `lock`/`unlock`, together with the ring's
 * dropped-bytes count) and put the note in front when records were lost;
 * then hand over as much of the batch as the room allows, whole lines
 * first. The cursor moves only once the whole batch is handed over.
 * `loss` (optional) gets what this step found lost.
 */
dcon_pump_t dcon_pump(dcon_t *c, logring_t *ring, logring_lock_fn lock, logring_lock_fn unlock, void *lock_ctx,
                      const dcon_tx_t *tx, dcon_loss_t *loss);

/** Has the console handed over every record before `next_seq` (no batch in flight)? */
bool dcon_caught_up(const dcon_t *c, uint32_t next_seq);

/**
 * Text that is not a record is about to be handed over (under the same
 * mutex): true when a line break has to come first, because the last
 * record byte handed over did not end a line (a record longer than the
 * transmit buffer handed over in parts, or a line logged in fragments).
 * Afterwards the console counts as at a line start: such text ends its
 * line, or it is a prompt that waits on its line, which the typed input's
 * echo ends (that echo never comes through here). Empty text changes
 * nothing (false).
 */
bool dcon_other_text(dcon_t *c, const char *text, size_t len);

/* ---- lines gathered from pieces ---------------------------------------------------------- */

/** Hands over `len` bytes: whole lines, or a part of a line longer than the gatherer. */
typedef void (*dcon_emit_fn)(void *ctx, const char *s, size_t len);

/**
 * A gatherer: text written in pieces is kept until its line ends; every
 * whole line gathered then goes to `emit` in one call (several lines
 * together when one piece ended several). A line longer than the buffer is
 * handed over in parts of the buffer's size, the only way a line is split.
 * One writer per gatherer.
 */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;               /**< bytes of the line not ended yet */
} dcon_lines_t;

void dcon_lines_init(dcon_lines_t *g, char *buf, size_t cap);

/** Append `len` bytes, handing over every line they end. */
void dcon_lines_put(dcon_lines_t *g, const char *s, size_t len, dcon_emit_fn emit, void *ctx);

/**
 * The caller wrote `len` bytes into the buffer itself, at buf + g->len (at
 * most cap - g->len): take them as dcon_lines_put would.
 */
void dcon_lines_added(dcon_lines_t *g, size_t len, dcon_emit_fn emit, void *ctx);

/**
 * Hand over what is gathered of a line that has not ended: as it is (a
 * prompt that waits for input on its line), or, with `end_line`, ended with
 * '\n' first (the end of a reply). Nothing when nothing is gathered.
 */
void dcon_lines_flush(dcon_lines_t *g, bool end_line, dcon_emit_fn emit, void *ctx);

#ifdef __cplusplus
}
#endif
