/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "settings_codec.h"

#include <string.h>

void blob_writer_init(blob_writer_t *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->error = (buf == NULL && cap != 0);
}

static void put_le(blob_writer_t *w, uint64_t v, size_t n)
{
    if (w->error || n > w->cap - w->len) {
        w->error = true;
        return;
    }
    for (size_t i = 0; i < n; i++) {
        w->buf[w->len++] = (uint8_t)(v >> (8u * i));
    }
}

void blob_put_u8(blob_writer_t *w, uint8_t v)   { put_le(w, v, 1); }
void blob_put_u16(blob_writer_t *w, uint16_t v) { put_le(w, v, 2); }
void blob_put_u32(blob_writer_t *w, uint32_t v) { put_le(w, v, 4); }
void blob_put_u64(blob_writer_t *w, uint64_t v) { put_le(w, v, 8); }
void blob_put_i64(blob_writer_t *w, int64_t v)  { put_le(w, (uint64_t)v, 8); }
void blob_put_bool(blob_writer_t *w, bool v)    { put_le(w, v ? 1u : 0u, 1); }

void blob_put_f32(blob_writer_t *w, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    put_le(w, bits, 4);
}

void blob_put_f64(blob_writer_t *w, double v)
{
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    put_le(w, bits, 8);
}

void blob_put_str(blob_writer_t *w, const char *s)
{
    size_t n = (s != NULL) ? strlen(s) : 0;
    if (n > 255u) {
        w->error = true;
        return;
    }
    put_le(w, n, 1);
    if (w->error || n > w->cap - w->len) {
        w->error = true;
        return;
    }
    if (n > 0) {
        memcpy(&w->buf[w->len], s, n);
        w->len += n;
    }
}

bool blob_writer_ok(const blob_writer_t *w)
{
    return !w->error;
}

void blob_reader_init(blob_reader_t *r, const uint8_t *buf, size_t len)
{
    r->buf = buf;
    r->len = (buf != NULL) ? len : 0;
    r->pos = 0;
    r->error = false;
}

static uint64_t get_le(blob_reader_t *r, size_t n)
{
    if (r->error || n > r->len - r->pos) {
        r->error = true;
        return 0;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        v |= (uint64_t)r->buf[r->pos++] << (8u * i);
    }
    return v;
}

uint8_t blob_get_u8(blob_reader_t *r)   { return (uint8_t)get_le(r, 1); }
uint16_t blob_get_u16(blob_reader_t *r) { return (uint16_t)get_le(r, 2); }
uint32_t blob_get_u32(blob_reader_t *r) { return (uint32_t)get_le(r, 4); }
uint64_t blob_get_u64(blob_reader_t *r) { return get_le(r, 8); }
int64_t blob_get_i64(blob_reader_t *r)  { return (int64_t)get_le(r, 8); }
bool blob_get_bool(blob_reader_t *r)    { return get_le(r, 1) != 0; }

float blob_get_f32(blob_reader_t *r)
{
    uint32_t bits = (uint32_t)get_le(r, 4);
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

double blob_get_f64(blob_reader_t *r)
{
    uint64_t bits = get_le(r, 8);
    double v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

void blob_get_str(blob_reader_t *r, char *out, size_t out_size)
{
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    size_t n = (size_t)get_le(r, 1);
    if (r->error) {
        return;
    }
    if (out == NULL || n >= out_size || n > r->len - r->pos) {
        r->error = true;
        return;
    }
    memcpy(out, &r->buf[r->pos], n);
    out[n] = '\0';
    r->pos += n;
}

bool blob_reader_ok(const blob_reader_t *r)
{
    return !r->error;
}

size_t blob_reader_remaining(const blob_reader_t *r)
{
    return r->error ? 0 : r->len - r->pos;
}

uint32_t blob_hash32(const uint8_t *data, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}
