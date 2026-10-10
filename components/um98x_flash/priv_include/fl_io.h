/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Byte-stream I/O of the receiver flasher's pure cores (no IDF or RTOS
 * includes, host-testable). The firmware wraps the GNSS link's RAW lease in
 * it; the host tests wrap a simulated receiver loader with a virtual clock.
 *
 * Rules every core follows:
 *  - every read waits at most FL_SLICE_MS, so the caller's task watchdog is
 *    fed inside every wait, however long the wait is in total;
 *  - times are 32-bit millisecond values compared only as differences
 *    ((uint32_t)(now - then)), so a wrapping clock is harmless;
 *  - write() returns once the bytes have left the UART (drained), so a time
 *    taken right after it is the end of the write.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Longest single read wait (task watchdog slices). */
#define FL_SLICE_MS 1000u

typedef struct {
    void *ctx;
    /** Write all bytes and wait until they have left; 0 = ok, anything else = failed (nothing more is sent). */
    int (*write)(void *ctx, const uint8_t *data, size_t len);
    /** Read up to cap bytes, waiting up to timeout_ms (<= FL_SLICE_MS) for the first. Returns the count, 0 on timeout. */
    size_t (*read)(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms);
    /** Discard everything buffered on input. */
    void (*flush)(void *ctx);
    /** Switch the host baud (after pending output, input discarded briefly). 0 = ok. */
    int (*set_baud)(void *ctx, uint32_t baud);
    /** Monotonic milliseconds (may wrap). */
    uint32_t (*now_ms)(void *ctx);
    /** Idle for ms (<= FL_SLICE_MS; feeds the watchdog). */
    void (*sleep_ms)(void *ctx, uint32_t ms);
    /** UART framing-error events counted so far (cumulative), or NULL when unknown. */
    uint32_t (*line_errors)(void *ctx);
    /** Optional: one line of plain text for the session log (no newline). */
    void (*note)(void *ctx, const char *text);
} fl_io_t;

static inline uint32_t fl_now(const fl_io_t *io)
{
    return io->now_ms(io->ctx);
}

/** Milliseconds since t (wrap-safe). */
static inline uint32_t fl_since(const fl_io_t *io, uint32_t t)
{
    return (uint32_t)(io->now_ms(io->ctx) - t);
}

/** Milliseconds left until deadline (0 when it has passed; wrap-safe within 2^31 ms). */
static inline uint32_t fl_left(const fl_io_t *io, uint32_t deadline)
{
    int32_t d = (int32_t)(deadline - io->now_ms(io->ctx));
    return d > 0 ? (uint32_t)d : 0u;
}

static inline int fl_write(const fl_io_t *io, const void *data, size_t len)
{
    return io->write(io->ctx, (const uint8_t *)data, len);
}

/** Write a string literal or C string. */
int fl_write_str(const fl_io_t *io, const char *s);

/** Idle for ms in watchdog slices. */
void fl_sleep(const fl_io_t *io, uint32_t ms);

/** printf-style note into the session log (truncated to 160 characters). */
void fl_notef(const fl_io_t *io, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/** NUL-safe substring search. */
const uint8_t *fl_memmem(const uint8_t *hay, size_t hay_len, const char *needle);

#ifdef __cplusplus
}
#endif
