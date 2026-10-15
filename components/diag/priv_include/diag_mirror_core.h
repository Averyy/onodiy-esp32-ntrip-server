/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * A copy of the console on a second port that nobody may be reading (pure
 * logic, no IDF or RTOS includes, host-tested).
 *
 * The station's ESP32S3 USB-C port reaches the UART0 console's bridge with
 * the plug one way round and the chip's built-in USB (USB Serial/JTAG) the
 * other way round. diag copies every console write (log lines, the bench
 * console's replies, its notes) to the built-in USB too, so either way round
 * shows the log. That copy must never hold up the console: with no host
 * attached, or one that does not read, the USB port's transmit buffer
 * stays full. So each write is handed to the port whole or not at all and
 * never waited for:
 *  - The port takes it (there is room in its buffer): it goes out as is.
 *  - No host is attached (no USB frames from one): nothing is handed over.
 *    That is not a loss, just nobody listening, and it is not counted.
 *  - A host is attached but the buffer has no room (it does not read, or
 *    reads too slowly): the write is lost and counted, in lines (every line
 *    any of whose bytes were lost, each once) and in text bytes.
 * The next write the port takes after lines were lost starts with one line
 * "[diag: N line(s) not sent on USB]"; after anything not sent, a write
 * that would continue a line the host never saw the end of starts on a new
 * line instead. A note and its write go to the port together, so a note
 * is never lost on its own.
 *
 * One writer: diag calls it under the console mutex, with the text before
 * the console's line-ending rule; the port's writes follow the same rule as
 * the UART's (`eol`).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Longest note: "\n[diag: 4294967295 lines not sent on USB]\n" fits. */
#define DMIR_NOTE_MAX 48u

/** Scratch size dmir_write needs for writes of up to `max_len` text bytes in one piece. */
#define DMIR_BUF_SIZE(max_len) (DMIR_NOTE_MAX + 2u * (size_t)(max_len))

/** How a '\n' of the text goes out (the console's CONFIG_LIBC_STDOUT_LINE_ENDING_*). */
typedef enum {
    DMIR_EOL_LF = 0,
    DMIR_EOL_CRLF,
    DMIR_EOL_CR,
} dmir_eol_t;

/** The second port, as the copy sees it. */
typedef struct {
    /** Is a host attached (it sends USB frames)? */
    bool (*connected)(void *ctx);
    /** Hand over `len` bytes now, all or none, never waiting: true = taken. */
    bool (*put)(void *ctx, const char *s, size_t len);
    void *ctx;
    dmir_eol_t eol;
} dmir_port_t;

typedef struct {
    char *buf;                /**< scratch: the note and the text with its line endings */
    size_t cap;
    bool sent_mid_line;       /**< the last byte the port took did not end a line */
    bool gap;                 /**< something was not sent since the port last took a write */
    bool in_line;             /**< the last byte seen (sent or not) did not end a line */
    bool line_lost;           /**< ...and that line is counted as lost already */
    uint32_t pending;         /**< lines lost since the last note */
    uint32_t lost;            /**< lines lost in all (while a host was attached) */
    uint64_t lost_bytes;      /**< their text bytes */
} dmir_t;

/** Start: `buf` of `cap` bytes, at least DMIR_BUF_SIZE(1). */
void dmir_init(dmir_t *m, char *buf, size_t cap);

/**
 * Copy `len` text bytes to the port (see above). Text longer than the
 * scratch allows in one piece goes in pieces, each whole or not at all.
 */
void dmir_write(dmir_t *m, const dmir_port_t *port, const char *s, size_t len);

#ifdef __cplusplus
}
#endif
