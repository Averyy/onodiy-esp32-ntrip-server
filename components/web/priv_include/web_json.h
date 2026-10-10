/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Streaming JSON writer (pure logic, no IDF or RTOS includes, host-testable).
 *
 * Output goes into a caller-supplied buffer. With a flush callback the
 * buffer is handed over whenever it fills (an HTTP response sent in chunks,
 * so no response needs a large contiguous block); without one, running out
 * of room is an error and the text stays complete up to the last fitting
 * byte (callers check wj_ok()). Errors are sticky: after the first one every
 * call is a no-op.
 *
 * Members and elements: every value call takes a key. Inside an object the
 * key names the member; inside an array (or at the top level) it must be
 * NULL. Commas are inserted automatically.
 *
 * Strings are escaped for JSON: quote, backslash and control bytes
 * (\u00XX, the usual short forms for \n \r \t); valid UTF-8 is copied as
 * is; a byte that is not part of a valid UTF-8 sequence is written as
 * \u00XX (read as Latin-1), so the output is always valid UTF-8 JSON.
 *
 * Fragments: wj_init_members() starts inside an implicit object (members are
 * written without the surrounding braces), wj_init_elements() inside an
 * implicit array. The status push builds each section this way, compares
 * the texts and joins the changed ones with commas.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Deepest nesting the writer tracks (objects and arrays together). */
#define WJ_MAX_DEPTH 12

/** Hands `len` bytes on; false = the receiver failed (the writer stops). */
typedef bool (*wj_flush_fn)(void *ctx, const char *data, size_t len);

typedef struct {
    char *buf;
    size_t cap;
    size_t len;          /**< bytes in buf not flushed yet */
    size_t total;        /**< bytes produced so far (flushed + buffered) */
    wj_flush_fn flush;
    void *flush_ctx;
    bool flushed;        /**< flush was called at least once */
    bool error;
    uint8_t depth;
    bool need_comma[WJ_MAX_DEPTH + 1];
    bool is_array[WJ_MAX_DEPTH + 1];
} wj_t;

/** Plain writer at the top level (one value). `flush` may be NULL. */
void wj_init(wj_t *w, char *buf, size_t cap, wj_flush_fn flush, void *ctx);

/** Fragment writer inside an implicit object: `"a":1,"b":2`. */
void wj_init_members(wj_t *w, char *buf, size_t cap);

/** Fragment writer inside an implicit array: `{...},{...}`. */
void wj_init_elements(wj_t *w, char *buf, size_t cap);

void wj_obj_open(wj_t *w, const char *key);
void wj_obj_close(wj_t *w);
void wj_arr_open(wj_t *w, const char *key);
void wj_arr_close(wj_t *w);

/** String value; NULL writes null. */
void wj_str(wj_t *w, const char *key, const char *s);
/** String of exactly `len` bytes (may contain NUL, written as \u0000). */
void wj_strn(wj_t *w, const char *key, const char *s, size_t len);
/** String, null when empty or NULL. */
void wj_str_or_null(wj_t *w, const char *key, const char *s);
void wj_int(wj_t *w, const char *key, int64_t v);
void wj_uint(wj_t *w, const char *key, uint64_t v);
/** Unsigned value; UINT32_MAX ("never", "unknown") writes null. */
void wj_u32_or_null(wj_t *w, const char *key, uint32_t v);
/** Unix time; 0 or less writes null. */
void wj_unix(wj_t *w, const char *key, int64_t v);
void wj_bool(wj_t *w, const char *key, bool v);
void wj_null(wj_t *w, const char *key);
/** Fixed decimals (0-9); NaN or infinity writes null. Never exponent notation. */
void wj_double(wj_t *w, const char *key, double v, unsigned decimals);
/** Lowercase hex string of `len` bytes. */
void wj_hex(wj_t *w, const char *key, const uint8_t *data, size_t len);
/** "0x%08x" string. */
void wj_addr(wj_t *w, const char *key, uint32_t v);
/** A JSON value that is already text (another writer's fragment); copied verbatim. */
void wj_raw(wj_t *w, const char *key, const char *json, size_t len);

/** Flush what is buffered (with a flush callback). @return !error. */
bool wj_finish(wj_t *w);

/** True while no error happened. */
bool wj_ok(const wj_t *w);

/** Text written so far when there is no flush callback (NUL-terminated if room). */
const char *wj_text(wj_t *w);

/* ---- small helpers shared with the parsers ---------------------------------- */

/**
 * Length of the valid UTF-8 sequence starting at s[0] (1-4), 0 if s[0]
 * does not start one within `avail` bytes (overlong forms, surrogates and
 * code points above U+10FFFF are invalid).
 */
size_t wj_utf8_seq_len(const uint8_t *s, size_t avail);

#ifdef __cplusplus
}
#endif
