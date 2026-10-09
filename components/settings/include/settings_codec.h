/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Explicit little-endian encoding for settings blobs (pure logic, no IDF
 * includes, host-testable).
 *
 * Multi-field records are stored as one blob: a version byte followed by
 * fields written with these helpers. The layout is defined by the order of
 * the put/get calls, not by any C struct, so it does not change with the
 * compiler, padding or enum sizes.
 *
 * Writers and readers are sticky on error: after the first overflow or short
 * read every later call is a no-op and *_ok() returns false, so callers check
 * once at the end.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool error;
} blob_writer_t;

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t pos;
    bool error;
} blob_reader_t;

void blob_writer_init(blob_writer_t *w, uint8_t *buf, size_t cap);
void blob_put_u8(blob_writer_t *w, uint8_t v);
void blob_put_u16(blob_writer_t *w, uint16_t v);
void blob_put_u32(blob_writer_t *w, uint32_t v);
void blob_put_u64(blob_writer_t *w, uint64_t v);
void blob_put_i64(blob_writer_t *w, int64_t v);
void blob_put_f32(blob_writer_t *w, float v);
void blob_put_f64(blob_writer_t *w, double v);
void blob_put_bool(blob_writer_t *w, bool v);
/** u8 length + bytes (no terminator). Strings longer than 255 bytes are an error. */
void blob_put_str(blob_writer_t *w, const char *s);
/** True if every put fitted. */
bool blob_writer_ok(const blob_writer_t *w);

void blob_reader_init(blob_reader_t *r, const uint8_t *buf, size_t len);
uint8_t blob_get_u8(blob_reader_t *r);
uint16_t blob_get_u16(blob_reader_t *r);
uint32_t blob_get_u32(blob_reader_t *r);
uint64_t blob_get_u64(blob_reader_t *r);
int64_t blob_get_i64(blob_reader_t *r);
float blob_get_f32(blob_reader_t *r);
double blob_get_f64(blob_reader_t *r);
bool blob_get_bool(blob_reader_t *r);
/** Copy a string into out (always terminated). Too long for out = error. */
void blob_get_str(blob_reader_t *r, char *out, size_t out_size);
/** True if every get had enough input. */
bool blob_reader_ok(const blob_reader_t *r);
/** Bytes not consumed yet (newer writers may append fields; older readers ignore them). */
size_t blob_reader_remaining(const blob_reader_t *r);

/** FNV-1a 32-bit hash; stable across builds, used for "already applied" checks. */
uint32_t blob_hash32(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
