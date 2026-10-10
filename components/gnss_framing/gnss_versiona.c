/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Version log parser (pure logic, no IDF includes): the one parser for the
 * receiver's "#VERSIONA" log, shared by the command layer and the firmware
 * flasher. Rules in gnss_framing.h.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "gnss_framing.h"

#define VERSIONA_PREFIX     "#VERSIONA,"
#define VERSIONA_PREFIX_LEN (sizeof(VERSIONA_PREFIX) - 1u)
#define CRC_HEX_DIGITS      8u

/* Receiver ASCII log CRC: reflected CRC-32 (polynomial 0xEDB88320), initial
 * value 0, no final XOR, over the bytes between '#' and '*'. Bitwise: the
 * version log is parsed rarely and a table would cost 1 KB. */
static uint32_t log_crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
    }
    return crc;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Exactly CRC_HEX_DIGITS hex digits (the receiver writes lower case; either
 * case is accepted). */
static bool parse_crc(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    for (size_t i = 0; i < CRC_HEX_DIGITS; i++) {
        int d = hex_value(s[i]);
        if (d < 0) {
            return false;
        }
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return true;
}

/* One body field with one pair of surrounding quotes removed. */
typedef struct {
    const char *p;
    size_t len;
} field_t;

static field_t unquote(const char *p, size_t len)
{
    if (len >= 2 && p[0] == '"' && p[len - 1] == '"') {
        p++;
        len -= 2;
    }
    return (field_t){ p, len };
}

/* Copy a field that must fit whole (model, firmware): false if it does not. */
static bool copy_exact(char *dst, size_t cap, field_t f)
{
    if (f.len >= cap) {
        return false;
    }
    memcpy(dst, f.p, f.len);
    dst[f.len] = '\0';
    return true;
}

/* Copy a display-only field, cut to the buffer. */
static void copy_cut(char *dst, size_t cap, const char *p, size_t len)
{
    if (len >= cap) {
        len = cap - 1;
    }
    memcpy(dst, p, len);
    dst[len] = '\0';
}

/* Integer after "Build" in the firmware string; 0 if absent, without digits
 * or larger than 32 bits. */
static uint32_t build_number(const char *fw)
{
    const char *b = strstr(fw, "Build");
    if (b == NULL) {
        return 0;
    }
    b += 5;
    uint64_t v = 0;
    size_t digits = 0;
    while (*b >= '0' && *b <= '9') {
        v = v * 10u + (uint64_t)(*b - '0');
        if (v > UINT32_MAX) {
            return 0;
        }
        b++;
        digits++;
    }
    return digits > 0 ? (uint32_t)v : 0;
}

bool gnss_versiona_parse(const char *line, size_t len, gnss_versiona_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (line == NULL) {
        return false;
    }
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        len--;
    }
    if (len < VERSIONA_PREFIX_LEN + 1u + CRC_HEX_DIGITS || memcmp(line, VERSIONA_PREFIX, VERSIONA_PREFIX_LEN) != 0) {
        return false;
    }
    /* A NUL anywhere means this is not one text line. */
    if (memchr(line, '\0', len) != NULL) {
        return false;
    }
    const size_t star = len - 1u - CRC_HEX_DIGITS;
    uint32_t want;
    if (line[star] != '*' || !parse_crc(line + star + 1, &want) ||
        log_crc32((const uint8_t *)line + 1, star - 1u) != want) {
        return false;
    }

    /* Header and body split at the first ';'; only the log name is taken
     * from the header. */
    const char *semi = memchr(line, ';', star);
    if (semi == NULL) {
        return false;
    }
    const char *p = semi + 1;
    const char *end = line + star;

    field_t f[GNSS_VERSIONA_MIN_FIELDS];
    size_t n = 0;
    while (n < GNSS_VERSIONA_MIN_FIELDS) {
        const char *comma = memchr(p, ',', (size_t)(end - p));
        const char *stop = (comma != NULL) ? comma : end;
        f[n++] = unquote(p, (size_t)(stop - p));
        if (comma == NULL) {
            break;
        }
        p = comma + 1;
    }
    if (n < GNSS_VERSIONA_MIN_FIELDS) {
        return false;
    }

    /* The model and the firmware decide what gets configured or flashed:
     * they must fit whole and be non-empty. The other fields are shown only. */
    if (f[0].len == 0 || f[1].len == 0 || !copy_exact(out->model, sizeof(out->model), f[0]) ||
        !copy_exact(out->firmware, sizeof(out->firmware), f[1])) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    const char *dash = memchr(f[3].p, '-', f[3].len);
    if (dash != NULL) {
        copy_cut(out->pn, sizeof(out->pn), f[3].p, (size_t)(dash - f[3].p));
        copy_cut(out->serial, sizeof(out->serial), dash + 1, f[3].len - (size_t)(dash - f[3].p) - 1u);
    } else {
        copy_cut(out->pn, sizeof(out->pn), f[3].p, f[3].len);
    }
    copy_cut(out->efuse_id, sizeof(out->efuse_id), f[4].p, f[4].len);
    copy_cut(out->compile_date, sizeof(out->compile_date), f[5].p, f[5].len);
    out->build = build_number(out->firmware);
    return true;
}
