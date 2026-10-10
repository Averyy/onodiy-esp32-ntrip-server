/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "fixture_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#ifndef FIXTURE_DIR
#error "FIXTURE_DIR must point at test/fixtures"
#endif

static void path_of(char *out, size_t cap, const char *rel)
{
    int n = snprintf(out, cap, "%s/%s", FIXTURE_DIR, rel);
    TEST_ASSERT_TRUE_MESSAGE(n > 0 && (size_t)n < cap, "fixture path too long");
}

static uint8_t *read_file(const char *rel, size_t *len, bool required)
{
    char path[512];
    path_of(path, sizeof(path), rel);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        if (required) {
            TEST_FAIL_MESSAGE(path);
        }
        return NULL;
    }
    TEST_ASSERT_EQUAL_INT(0, fseek(f, 0, SEEK_END));
    long n = ftell(f);
    TEST_ASSERT_TRUE(n >= 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(f, 0, SEEK_SET));
    uint8_t *buf = malloc((size_t)n + 1);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_size_t((size_t)n, fread(buf, 1, (size_t)n, f));
    fclose(f);
    buf[n] = 0;
    *len = (size_t)n;
    return buf;
}

uint8_t *fx_read(const char *rel, size_t *len)
{
    return read_file(rel, len, true);
}

static cJSON *s_index;

const cJSON *fx_index(void)
{
    if (s_index == NULL) {
        size_t n;
        char *text = (char *)fx_read("oracle/index.json", &n);
        s_index = cJSON_Parse(text);
        free(text);
        TEST_ASSERT_NOT_NULL_MESSAGE(s_index, "oracle/index.json does not parse");
    }
    return s_index;
}

const cJSON *fx_entry(const char *name)
{
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(fx_index(), name);
    TEST_ASSERT_NOT_NULL_MESSAGE(e, name);
    return e;
}

long fx_int(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    TEST_ASSERT_TRUE_MESSAGE(cJSON_IsNumber(v), key);
    return (long)v->valuedouble;
}

bool fx_jsonl_load(const char *rel, fx_jsonl_t *out)
{
    memset(out, 0, sizeof(*out));
    size_t len;
    char *text = (char *)read_file(rel, &len, false);
    if (text == NULL) {
        return false;
    }
    size_t lines = 0;
    for (size_t i = 0; i < len; i++) {
        lines += text[i] == '\n';
    }
    out->rec = calloc(lines + 1, sizeof(cJSON *));
    TEST_ASSERT_NOT_NULL(out->rec);
    char *p = text;
    while (*p != '\0') {
        char *nl = strchr(p, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }
        if (*p != '\0') {
            cJSON *j = cJSON_Parse(p);
            TEST_ASSERT_NOT_NULL_MESSAGE(j, rel);
            out->rec[out->n++] = j;
        }
        if (nl == NULL) {
            break;
        }
        p = nl + 1;
    }
    free(text);
    return true;
}

void fx_jsonl_free(fx_jsonl_t *j)
{
    for (size_t i = 0; i < j->n; i++) {
        cJSON_Delete(j->rec[i]);
    }
    free(j->rec);
    memset(j, 0, sizeof(*j));
}

size_t fx_dechunk(const uint8_t *in, size_t len, uint8_t *out, size_t *chunks, bool *complete)
{
    size_t i = 0;
    size_t o = 0;
    *chunks = 0;
    *complete = false;
    while (i < len) {
        size_t size = 0;
        size_t j = i;
        unsigned digits = 0;
        for (; j < len; j++) {
            uint8_t c = in[j];
            unsigned d;
            if (c >= '0' && c <= '9') {
                d = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                d = c - 'a' + 10u;
            } else if (c >= 'A' && c <= 'F') {
                d = c - 'A' + 10u;
            } else {
                break;
            }
            size = size * 16u + d;
            digits++;
        }
        if (digits == 0 || digits > 6) {
            return o;   /* not a size line */
        }
        if (j < len && in[j] == ';') {
            while (j < len && in[j] != '\r') {
                j++;   /* chunk extension */
            }
        }
        if (j + 1 >= len || in[j] != '\r' || in[j + 1] != '\n') {
            return o;   /* cut, or garbage after the size */
        }
        j += 2;
        if (size == 0) {
            *complete = true;
            return o;
        }
        if (j + size + 2 > len || in[j + size] != '\r' || in[j + size + 1] != '\n') {
            return o;   /* cut or malformed */
        }
        memcpy(out + o, in + j, size);
        o += size;
        (*chunks)++;
        i = j + size + 2;
    }
    return o;
}

uint8_t *fx_stream(const char *name, size_t *len)
{
    const cJSON *e = fx_entry(name);
    const cJSON *path = cJSON_GetObjectItemCaseSensitive(e, "path");
    TEST_ASSERT_TRUE(cJSON_IsString(path));
    size_t n;
    uint8_t *raw = fx_read(path->valuestring, &n);
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "dechunk"))) {
        *len = n;
        return raw;
    }
    uint8_t *body = malloc(n + 1);
    TEST_ASSERT_NOT_NULL(body);
    size_t chunks;
    bool complete;
    *len = fx_dechunk(raw, n, body, &chunks, &complete);
    free(raw);
    return body;
}

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t pos;    /* end of the previous unit */
    fx_units_t *out;
} frame_ctx_t;

static void on_unit(const gnss_frame_t *fr, void *arg)
{
    frame_ctx_t *c = arg;
    if (fr->kind == GNSS_FRAME_PROMPT) {
        return;
    }
    size_t at = c->pos;
    while (at + fr->len <= c->len && memcmp(c->buf + at, fr->data, fr->len) != 0) {
        at++;
    }
    TEST_ASSERT_TRUE_MESSAGE(at + fr->len <= c->len, "emitted unit not found in the input");
    fx_units_t *o = c->out;
    if (o->n == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 256;
        o->u = realloc(o->u, o->cap * sizeof(*o->u));
        TEST_ASSERT_NOT_NULL(o->u);
    }
    o->u[o->n++] = (fx_unit_t) {
        .kind = fr->kind,
        .type = fr->rtcm_msg_type,
        .off = at,
        .len = fr->len,
        .data = c->buf + at,
    };
    c->pos = at + fr->len;
}

void fx_frame(const uint8_t *buf, size_t len, size_t piece, fx_units_t *out)
{
    memset(out, 0, sizeof(*out));
    static gnss_framer_t f;
    frame_ctx_t c = { .buf = buf, .len = len, .pos = 0, .out = out };
    gnss_framer_init(&f, on_unit, &c);
    if (piece == 0) {
        piece = len ? len : 1;
    }
    for (size_t i = 0; i < len; i += piece) {
        gnss_framer_feed(&f, buf + i, (len - i < piece) ? len - i : piece);
    }
    gnss_framer_get_stats(&f, &out->stats);
}

void fx_units_free(fx_units_t *u)
{
    free(u->u);
    memset(u, 0, sizeof(*u));
}

const char *fx_sys_name(unsigned sys)
{
    static const char *const k[] = { "GPS", "GLONASS", "Galileo", "SBAS", "QZSS", "BeiDou", "NavIC" };
    return sys < sizeof(k) / sizeof(k[0]) ? k[sys] : "?";
}
