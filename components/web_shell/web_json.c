/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "web_json.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void reset(wj_t *w, char *buf, size_t cap, wj_flush_fn flush, void *ctx)
{
    memset(w, 0, sizeof(*w));
    w->buf = buf;
    w->cap = cap;
    w->flush = flush;
    w->flush_ctx = ctx;
    if (buf == NULL || cap == 0) {
        w->error = true;
    } else {
        buf[0] = '\0';
    }
}

void wj_init(wj_t *w, char *buf, size_t cap, wj_flush_fn flush, void *ctx)
{
    reset(w, buf, cap, flush, ctx);
}

void wj_init_members(wj_t *w, char *buf, size_t cap)
{
    reset(w, buf, cap, NULL, NULL);
    w->depth = 1;
    w->is_array[1] = false;
}

void wj_init_elements(wj_t *w, char *buf, size_t cap)
{
    reset(w, buf, cap, NULL, NULL);
    w->depth = 1;
    w->is_array[1] = true;
}

/* ---- raw output ---------------------------------------------------------------- */

static bool do_flush(wj_t *w)
{
    if (w->len == 0) {
        return true;
    }
    if (w->flush == NULL || !w->flush(w->flush_ctx, w->buf, w->len)) {
        w->error = true;
        return false;
    }
    w->flushed = true;
    w->len = 0;
    return true;
}

static void put(wj_t *w, const char *s, size_t n)
{
    if (w->error) {
        return;
    }
    while (n > 0) {
        /* Without a flush callback one byte stays free for the terminator. */
        size_t room = w->cap - w->len - (w->flush == NULL ? 1u : 0u);
        if (w->cap <= w->len + (w->flush == NULL ? 1u : 0u)) {
            room = 0;
        }
        if (room == 0) {
            if (w->flush == NULL || !do_flush(w)) {
                w->error = true;
                return;
            }
            continue;
        }
        size_t take = n < room ? n : room;
        memcpy(w->buf + w->len, s, take);
        w->len += take;
        w->total += take;
        s += take;
        n -= take;
    }
    if (w->flush == NULL) {
        w->buf[w->len] = '\0';
    }
}

static void putc_(wj_t *w, char c)
{
    put(w, &c, 1);
}

static void puts_(wj_t *w, const char *s)
{
    put(w, s, strlen(s));
}

/* ---- escaping ------------------------------------------------------------------- */

size_t wj_utf8_seq_len(const uint8_t *s, size_t avail)
{
    if (avail == 0) {
        return 0;
    }
    uint8_t c = s[0];
    if (c < 0x80) {
        return 1;
    }
    size_t n;
    uint32_t min;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) {
        n = 2;
        min = 0x80;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3;
        min = 0x800;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        n = 4;
        min = 0x10000;
        cp = c & 0x07;
    } else {
        return 0;
    }
    if (avail < n) {
        return 0;
    }
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    return n;
}

static void put_escaped(wj_t *w, const char *s, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    const uint8_t *p = (const uint8_t *)s;
    size_t i = 0;
    while (i < len && !w->error) {
        /* Copy a run of bytes that need no escaping in one go. */
        size_t run = i;
        while (run < len && p[run] >= 0x20 && p[run] < 0x7F && p[run] != '"' && p[run] != '\\') {
            run++;
        }
        if (run > i) {
            put(w, s + i, run - i);
            i = run;
            continue;
        }
        uint8_t c = p[i];
        if (c >= 0x80) {
            size_t n = wj_utf8_seq_len(p + i, len - i);
            if (n > 0) {
                put(w, s + i, n);
                i += n;
                continue;
            }
        }
        char esc[7];
        switch (c) {
        case '"':
            put(w, "\\\"", 2);
            break;
        case '\\':
            put(w, "\\\\", 2);
            break;
        case '\n':
            put(w, "\\n", 2);
            break;
        case '\r':
            put(w, "\\r", 2);
            break;
        case '\t':
            put(w, "\\t", 2);
            break;
        default:
            esc[0] = '\\';
            esc[1] = 'u';
            esc[2] = '0';
            esc[3] = '0';
            esc[4] = hex[c >> 4];
            esc[5] = hex[c & 0x0F];
            put(w, esc, 6);
            break;
        }
        i++;
    }
}

/* ---- structure ------------------------------------------------------------------ */

/* Comma and key before a value. A key inside an array (or a missing key
 * inside an object) is a caller error. */
static void prefix(wj_t *w, const char *key)
{
    if (w->error) {
        return;
    }
    bool in_array = w->depth == 0 || w->is_array[w->depth];
    if (in_array != (key == NULL)) {
        w->error = true;
        return;
    }
    if (w->need_comma[w->depth]) {
        putc_(w, ',');
    }
    w->need_comma[w->depth] = true;
    if (key != NULL) {
        putc_(w, '"');
        put_escaped(w, key, strlen(key));
        put(w, "\":", 2);
    }
}

static void open_(wj_t *w, const char *key, bool array)
{
    prefix(w, key);
    if (w->error) {
        return;
    }
    if (w->depth >= WJ_MAX_DEPTH) {
        w->error = true;
        return;
    }
    putc_(w, array ? '[' : '{');
    w->depth++;
    w->is_array[w->depth] = array;
    w->need_comma[w->depth] = false;
}

static void close_(wj_t *w, bool array)
{
    if (w->error) {
        return;
    }
    if (w->depth == 0 || w->is_array[w->depth] != array) {
        w->error = true;
        return;
    }
    w->depth--;
    putc_(w, array ? ']' : '}');
}

void wj_obj_open(wj_t *w, const char *key)
{
    open_(w, key, false);
}

void wj_obj_close(wj_t *w)
{
    close_(w, false);
}

void wj_arr_open(wj_t *w, const char *key)
{
    open_(w, key, true);
}

void wj_arr_close(wj_t *w)
{
    close_(w, true);
}

/* ---- values ---------------------------------------------------------------------- */

void wj_strn(wj_t *w, const char *key, const char *s, size_t len)
{
    prefix(w, key);
    putc_(w, '"');
    put_escaped(w, s, len);
    putc_(w, '"');
}

void wj_str(wj_t *w, const char *key, const char *s)
{
    if (s == NULL) {
        wj_null(w, key);
        return;
    }
    wj_strn(w, key, s, strlen(s));
}

void wj_str_or_null(wj_t *w, const char *key, const char *s)
{
    if (s == NULL || s[0] == '\0') {
        wj_null(w, key);
        return;
    }
    wj_str(w, key, s);
}

void wj_int(wj_t *w, const char *key, int64_t v)
{
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%" PRId64, v);
    prefix(w, key);
    put(w, tmp, (size_t)n);
}

void wj_uint(wj_t *w, const char *key, uint64_t v)
{
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%" PRIu64, v);
    prefix(w, key);
    put(w, tmp, (size_t)n);
}

void wj_u32_or_null(wj_t *w, const char *key, uint32_t v)
{
    if (v == UINT32_MAX) {
        wj_null(w, key);
    } else {
        wj_uint(w, key, v);
    }
}

void wj_unix(wj_t *w, const char *key, int64_t v)
{
    if (v <= 0) {
        wj_null(w, key);
    } else {
        wj_int(w, key, v);
    }
}

void wj_bool(wj_t *w, const char *key, bool v)
{
    prefix(w, key);
    puts_(w, v ? "true" : "false");
}

void wj_null(wj_t *w, const char *key)
{
    prefix(w, key);
    puts_(w, "null");
}

void wj_double(wj_t *w, const char *key, double v, unsigned decimals)
{
    if (!isfinite(v) || fabs(v) >= 1e15) {
        wj_null(w, key);
        return;
    }
    if (decimals > 9) {
        decimals = 9;
    }
    char tmp[40];
    int n = snprintf(tmp, sizeof(tmp), "%.*f", (int)decimals, v);
    if (n <= 0 || (size_t)n >= sizeof(tmp)) {
        wj_null(w, key);
        return;
    }
    /* "-0.000" reads as 0 anyway; keep JSON free of a negative zero. */
    bool all_zero = true;
    for (int i = 0; i < n; i++) {
        if (tmp[i] >= '1' && tmp[i] <= '9') {
            all_zero = false;
            break;
        }
    }
    const char *out = tmp;
    if (all_zero && tmp[0] == '-') {
        out = tmp + 1;
        n--;
    }
    prefix(w, key);
    put(w, out, (size_t)n);
}

void wj_hex(wj_t *w, const char *key, const uint8_t *data, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    prefix(w, key);
    putc_(w, '"');
    for (size_t i = 0; i < len && !w->error; i++) {
        char pair[2] = { hex[data[i] >> 4], hex[data[i] & 0x0F] };
        put(w, pair, 2);
    }
    putc_(w, '"');
}

void wj_addr(wj_t *w, const char *key, uint32_t v)
{
    char tmp[12];
    snprintf(tmp, sizeof(tmp), "0x%08" PRIx32, v);
    wj_str(w, key, tmp);
}

void wj_raw(wj_t *w, const char *key, const char *json, size_t len)
{
    prefix(w, key);
    put(w, json, len);
}

bool wj_finish(wj_t *w)
{
    if (w->error) {
        return false;
    }
    if (w->flush != NULL) {
        return do_flush(w);
    }
    return true;
}

bool wj_ok(const wj_t *w)
{
    return !w->error;
}

const char *wj_text(wj_t *w)
{
    if (w->buf != NULL && w->flush == NULL && w->len < w->cap) {
        w->buf[w->len] = '\0';
    }
    return w->buf;
}
