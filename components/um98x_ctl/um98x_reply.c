/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "um98x_reply.h"

#include <string.h>

#include "um98x_plan.h"

#define ACK_PREFIX     "$command,"
#define CONFIG_PREFIX  "$CONFIG,"
#define BANNER_PREFIX  "$devicename,"
#define VERSION_PREFIX "#VERSIONA,"
#define MODE_PREFIX    "#MODE"
#define GROUPS1_PREFIX "#ANT1GROUPA,"
#define GROUPS2_PREFIX "#ANT2GROUPA,"
#define BASEPOS_PREFIX "#BASEPOSA,"
#define SPPNAV_PREFIX  "#SPPNAVA,"
#define RESPONSE_SEP   ",response: "

#define PLEN(s) (sizeof(s) - 1u)

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool same_ci(const char *a, size_t alen, const char *b, size_t blen)
{
    if (alen != blen) {
        return false;
    }
    for (size_t i = 0; i < alen; i++) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

static bool prefix_ci(const char *s, size_t len, const char *prefix)
{
    size_t n = strlen(prefix);
    return len >= n && same_ci(s, n, prefix, n);
}

static bool is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void trim(const char **p, size_t *len)
{
    while (*len > 0 && is_blank((*p)[0])) {
        (*p)++;
        (*len)--;
    }
    while (*len > 0 && is_blank((*p)[*len - 1])) {
        (*len)--;
    }
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = lower(c);
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static bool parse_hex(const char *s, size_t digits, uint32_t *out)
{
    uint32_t v = 0;
    for (size_t i = 0; i < digits; i++) {
        int d = hex_value(s[i]);
        if (d < 0) {
            return false;
        }
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return true;
}

/* Receiver ASCII log CRC (reflected 0xEDB88320, initial 0, no final XOR). */
static uint32_t log_crc32(const char *s, size_t n)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint8_t)s[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
    }
    return crc;
}

uint8_t um98x_xor8(const char *s, size_t n)
{
    uint8_t x = 0;
    for (size_t i = 0; i < n; i++) {
        x ^= (uint8_t)s[i];
    }
    return x;
}

/* '$' line: "*hh" at the end, XOR from '$' inclusive. Returns the position
 * of '*' or 0 if the checksum is missing or wrong. */
static size_t dollar_checked(const char *line, size_t len)
{
    if (len < 4) {
        return 0;
    }
    size_t star = len - 3;
    uint32_t want;
    if (line[star] != '*' || !parse_hex(line + star + 1, 2, &want) || um98x_xor8(line, star) != want) {
        return 0;
    }
    return star;
}

/* '#' log with either checksum: 2-hex XOR including '#', or 8-hex CRC32
 * over the bytes after '#'. Returns the position of '*' or 0. */
static size_t hash_checked(const char *line, size_t len)
{
    uint32_t want;
    if (len >= 4 && line[len - 3] == '*' && parse_hex(line + len - 2, 2, &want) &&
        um98x_xor8(line, len - 3) == want) {
        return len - 3;
    }
    if (len >= 10 && line[len - 9] == '*' && parse_hex(line + len - 8, 8, &want) &&
        log_crc32(line + 1, len - 10) == want) {
        return len - 9;
    }
    return 0;
}

/* Last occurrence of sep in [p, p+len). */
static const char *find_last(const char *p, size_t len, const char *sep)
{
    size_t n = strlen(sep);
    if (len < n) {
        return NULL;
    }
    for (size_t i = len - n + 1; i-- > 0;) {
        if (memcmp(p + i, sep, n) == 0) {
            return p + i;
        }
    }
    return NULL;
}

static void classify_ack(const char *line, size_t star, um98x_line_t *out)
{
    const char *content = line + PLEN(ACK_PREFIX);
    size_t clen = star - PLEN(ACK_PREFIX);
    const char *sep = find_last(content, clen, RESPONSE_SEP);
    if (sep == NULL) {
        out->kind = UM98X_LINE_BAD;
        return;
    }
    out->kind = UM98X_LINE_ACK;
    out->echo = content;
    out->echo_len = (size_t)(sep - content);
    out->text = sep + PLEN(RESPONSE_SEP);
    out->text_len = (size_t)(line + star - out->text);
    out->ok = out->text_len >= 2 && out->text[0] == 'O' && out->text[1] == 'K';
}

static void classify_config(const char *line, size_t star, um98x_line_t *out)
{
    const char *content = line + PLEN(CONFIG_PREFIX);
    size_t clen = star - PLEN(CONFIG_PREFIX);
    const char *comma = memchr(content, ',', clen);
    if (comma == NULL || comma == content) {
        out->kind = UM98X_LINE_BAD;
        return;
    }
    out->kind = UM98X_LINE_CONFIG;
    out->item = content;
    out->item_len = (size_t)(comma - content);
    out->text = comma + 1;
    out->text_len = (size_t)(line + star - out->text);
}

static void classify_mode(const char *line, size_t len, um98x_line_t *out)
{
    /* Log name "MODE" or "MODEA", then the header. */
    size_t name_end = PLEN(MODE_PREFIX);
    if (len > name_end && line[name_end] == 'A') {
        name_end++;
    }
    if (len <= name_end || line[name_end] != ',') {
        out->kind = UM98X_LINE_OTHER;
        return;
    }
    size_t star = hash_checked(line, len);
    if (star == 0) {
        out->kind = UM98X_LINE_BAD;
        return;
    }
    out->kind = UM98X_LINE_MODE;
    const char *semi = memchr(line, ';', star);
    if (semi != NULL) {
        out->text = semi + 1;
        out->text_len = (size_t)(line + star - out->text);
    } else {
        out->text = line + star;   /* no body: compares as "different" */
        out->text_len = 0;
    }
}

/* The signal-group log: the body after the first ';' (build 25164's form),
 * else everything after the log name. */
static void classify_groups(const char *line, size_t len, uint8_t ant, um98x_line_t *out)
{
    size_t star = hash_checked(line, len);
    if (star == 0) {
        out->kind = UM98X_LINE_BAD;
        return;
    }
    const size_t name = PLEN(GROUPS1_PREFIX);
    const char *semi = memchr(line + name, ';', star - name);
    out->kind = UM98X_LINE_GROUPS;
    out->ant = ant;
    out->text = semi != NULL ? semi + 1 : line + name;
    out->text_len = (size_t)(line + star - out->text);
}

/* An own-position log: the body after the first ';', else after the log name (as the signal-group log). */
static void classify_ownpos(const char *line, size_t len, size_t name, uint8_t which, um98x_line_t *out)
{
    size_t star = hash_checked(line, len);
    if (star == 0 || star < name) {
        out->kind = UM98X_LINE_BAD;
        return;
    }
    const char *semi = memchr(line + name, ';', star - name);
    out->kind = UM98X_LINE_OWNPOS;
    out->own_log = which;
    out->text = semi != NULL ? semi + 1 : line + name;
    out->text_len = (size_t)(line + star - out->text);
}

/* "The Board is resetting,...*hh": the XOR of everything before '*'. */
static void classify_resetting(const char *line, size_t len, um98x_line_t *out)
{
    uint32_t want;
    if (len < 4 || line[len - 3] != '*' || !parse_hex(line + len - 2, 2, &want) ||
        um98x_xor8(line, len - 3) != want) {
        out->kind = UM98X_LINE_BAD;
        return;
    }
    out->kind = UM98X_LINE_RESETTING;
    out->text = line;
    out->text_len = len - 3;
}

void um98x_line_classify(const char *line, size_t len, um98x_line_t *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = UM98X_LINE_OTHER;
    if (line == NULL) {
        return;
    }
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        len--;
    }
    if (len == 0 || memchr(line, '\0', len) != NULL) {
        return;
    }
    if (line[0] == '$') {
        bool ack = prefix_ci(line, len, ACK_PREFIX);
        bool cfg = !ack && prefix_ci(line, len, CONFIG_PREFIX);
        bool banner = !ack && !cfg && prefix_ci(line, len, BANNER_PREFIX);
        if (!ack && !cfg && !banner) {
            return;   /* NMEA */
        }
        size_t star = dollar_checked(line, len);
        if (star == 0) {
            out->kind = UM98X_LINE_BAD;
            return;
        }
        if (ack) {
            classify_ack(line, star, out);
        } else if (cfg) {
            classify_config(line, star, out);
        } else {
            out->kind = UM98X_LINE_BANNER;
            out->text = line + PLEN(BANNER_PREFIX);
            out->text_len = star - PLEN(BANNER_PREFIX);
        }
        return;
    }
    if (line[0] == '#') {
        if (prefix_ci(line, len, VERSION_PREFIX)) {
            out->kind = gnss_versiona_parse(line, len, &out->version) ? UM98X_LINE_VERSION : UM98X_LINE_BAD;
        } else if (prefix_ci(line, len, MODE_PREFIX)) {
            classify_mode(line, len, out);
        } else if (prefix_ci(line, len, GROUPS1_PREFIX)) {
            classify_groups(line, len, 1, out);
        } else if (prefix_ci(line, len, GROUPS2_PREFIX)) {
            classify_groups(line, len, 2, out);
        } else if (prefix_ci(line, len, BASEPOS_PREFIX)) {
            classify_ownpos(line, len, PLEN(BASEPOS_PREFIX), UM98X_OWN_LOG_BASEPOS, out);
        } else if (prefix_ci(line, len, SPPNAV_PREFIX)) {
            classify_ownpos(line, len, PLEN(SPPNAV_PREFIX), UM98X_OWN_LOG_SPPNAV, out);
        }
        return;
    }
    if (len >= PLEN(GNSS_NOTICE_PREFIX) && memcmp(line, GNSS_NOTICE_PREFIX, PLEN(GNSS_NOTICE_PREFIX)) == 0) {
        classify_resetting(line, len, out);
    }
}

bool um98x_echo_matches(const char *echo, size_t echo_len, const char *cmd)
{
    if (echo == NULL || cmd == NULL) {
        return false;
    }
    size_t cmd_len = strlen(cmd);
    trim(&echo, &echo_len);
    trim(&cmd, &cmd_len);
    return echo_len > 0 && same_ci(echo, echo_len, cmd, cmd_len);
}

/* Next space-separated token of [*p, end). */
static bool next_token(const char **p, const char *end, const char **tok, size_t *tok_len)
{
    while (*p < end && is_blank(**p)) {
        (*p)++;
    }
    if (*p >= end) {
        return false;
    }
    *tok = *p;
    while (*p < end && !is_blank(**p)) {
        (*p)++;
    }
    *tok_len = (size_t)(*p - *tok);
    return true;
}

static bool parse_uint(const char *s, size_t len, uint32_t max, uint32_t *out)
{
    if (len == 0 || len > 10) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10u + (uint64_t)(s[i] - '0');
    }
    if (v > max) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static uint8_t on_off(const char *tok, size_t len)
{
    if (same_ci(tok, len, "ENABLE", 6)) {
        return UM98X_SETTING_ON;
    }
    if (same_ci(tok, len, "DISABLE", 7)) {
        return UM98X_SETTING_OFF;
    }
    return UM98X_SETTING_UNKNOWN;
}

/* Degrees with an optional sign and fraction, in 0.1 degree (rounded half
 * away from zero), -90..90. */
static bool parse_ddeg(const char *s, size_t len, int16_t *out)
{
    size_t i = 0;
    bool neg = false;
    if (i < len && (s[i] == '-' || s[i] == '+')) {
        neg = (s[i] == '-');
        i++;
    }
    uint32_t whole = 0;
    size_t digits = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        if (++digits > 3) {
            return false;
        }
        whole = whole * 10u + (uint32_t)(s[i] - '0');
        i++;
    }
    uint32_t tenths = 0;
    bool round_up = false;
    if (i < len && s[i] == '.') {
        i++;
        size_t frac = 0;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            if (frac == 0) {
                tenths = (uint32_t)(s[i] - '0');
            } else if (frac == 1) {
                round_up = s[i] >= '5';
            }
            frac++;
            digits++;
            i++;
        }
    }
    if (i != len || digits == 0) {
        return false;
    }
    uint32_t v = whole * 10u + tenths + (round_up ? 1u : 0u);
    if (v > 900u) {
        return false;
    }
    *out = (int16_t)(neg ? -(int32_t)v : (int32_t)v);
    return true;
}

static void skip_blanks(const char **p, const char *end)
{
    while (*p < end && is_blank(**p)) {
        (*p)++;
    }
}

/* One quoted string of at most UM98X_ANTENNA_STR_MAX printable characters. */
static bool quoted(const char **p, const char *end, char *out)
{
    skip_blanks(p, end);
    if (*p >= end || **p != '"') {
        return false;
    }
    (*p)++;
    size_t n = 0;
    while (*p < end && **p != '"') {
        unsigned char c = (unsigned char)**p;
        if (c < 0x20 || c > 0x7E || n >= UM98X_ANTENNA_STR_MAX) {
            return false;
        }
        out[n++] = (char)c;
        (*p)++;
    }
    if (*p >= end) {
        return false;   /* no closing quote */
    }
    (*p)++;
    out[n] = '\0';
    return true;
}

static void antenna_item(um98x_rx_settings_t *rx, const char *p, const char *end)
{
    um98x_rx_settings_t a;
    memset(&a, 0, sizeof(a));
    const char *tok;
    size_t tok_len;
    uint32_t id;
    bool ok = quoted(&p, end, a.antenna_model) && quoted(&p, end, a.antenna_serial) &&
              next_token(&p, end, &tok, &tok_len) && parse_uint(tok, tok_len, UINT8_MAX, &id);
    rx->antenna_known = ok;
    memset(rx->antenna_model, 0, sizeof(rx->antenna_model));
    memset(rx->antenna_serial, 0, sizeof(rx->antenna_serial));
    rx->antenna_setup_id = 0;
    if (ok) {
        memcpy(rx->antenna_model, a.antenna_model, sizeof(rx->antenna_model));
        memcpy(rx->antenna_serial, a.antenna_serial, sizeof(rx->antenna_serial));
        rx->antenna_setup_id = (uint8_t)id;
    }
}

void um98x_config_item(um98x_config_readback_t *rb, const char *item, size_t item_len, const char *text,
                       size_t text_len)
{
    if (rb->items < UINT16_MAX) {
        rb->items++;
    }
    bool sg = same_ci(item, item_len, "SIGNALGROUP", 11);
    bool com1 = same_ci(item, item_len, um98x_link_port_name(), 4);   /* the link's port (COM1 on the dev board) */
    bool b1c = same_ci(item, item_len, "RTCMB1CB2A", 10);
    bool mmp = same_ci(item, item_len, "MMP", 3);
    bool mask = same_ci(item, item_len, "MASK", 4);
    bool ant = same_ci(item, item_len, "BASEANTENNAMODEL", 16);
    if (!sg && !com1 && !b1c && !mmp && !mask && !ant) {
        return;
    }
    /* The values follow the item's own name in the text, e.g.
     * "CONFIG SIGNALGROUP 3 6" or "CONFIG COM1 115200". */
    const char *p = text;
    const char *end = text + text_len;
    const char *tok;
    size_t tok_len;
    bool found = false;
    while (next_token(&p, end, &tok, &tok_len)) {
        if (same_ci(tok, tok_len, item, item_len)) {
            found = true;
            break;
        }
    }
    if (ant) {
        if (found) {
            antenna_item(&rb->rx, p, end);
        } else {
            antenna_item(&rb->rx, end, end);   /* unknown */
        }
        return;
    }
    bool have = found && next_token(&p, end, &tok, &tok_len);
    if (b1c || mmp) {
        uint8_t v = have ? on_off(tok, tok_len) : (uint8_t)UM98X_SETTING_UNKNOWN;
        if (b1c) {
            rb->rx.b1cb2a = v;
        } else {
            rb->rx.mmp = v;
        }
        return;
    }
    if (!have) {
        return;
    }
    if (mask) {
        int16_t ddeg;
        if (parse_ddeg(tok, tok_len, &ddeg)) {
            rb->rx.mask_known = true;
            rb->rx.mask_ddeg = ddeg;
        }
        return;
    }
    uint32_t a;
    uint32_t b = 0;
    if (sg) {
        if (!parse_uint(tok, tok_len, UINT8_MAX, &a)) {
            return;
        }
        if (next_token(&p, end, &tok, &tok_len) && !parse_uint(tok, tok_len, UINT8_MAX, &b)) {
            return;
        }
        rb->signal_group_known = true;
        rb->signal_group_master = (uint8_t)a;
        rb->signal_group_slave = (uint8_t)b;
    } else if (parse_uint(tok, tok_len, UINT32_MAX, &a)) {
        rb->com1_baud_known = true;
        rb->com1_baud = a;
    }
}

bool um98x_stream_query_allowed(const char *cmd)
{
    if (cmd == NULL) {
        return false;
    }
    size_t n = strlen(cmd);
    trim(&cmd, &n);
    return same_ci(cmd, n, "VERSIONA", 8) || same_ci(cmd, n, "MODE", 4) || same_ci(cmd, n, "BASEPOSA", 8) ||
           same_ci(cmd, n, "SPPNAVA", 7);
}

bool um98x_cmd_allowed(um98x_link_mode_t mode, bool have_lease, const char *cmd)
{
    switch (mode) {
    case UM98X_LINK_STREAM:
        return !have_lease && cmd != NULL && um98x_stream_query_allowed(cmd);
    case UM98X_LINK_COMMAND:
        return have_lease;
    default:
        return false;
    }
}

bool um98x_cmd_normalise(const char *cmd, char *out, size_t cap)
{
    if (cmd == NULL || out == NULL || cap == 0) {
        return false;
    }
    out[0] = '\0';
    size_t n = strlen(cmd);
    trim(&cmd, &n);
    if (n == 0 || n >= UM98X_CMD_MAX || n >= cap) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)cmd[i];
        if (c < 0x20 || c >= 0x7F) {
            return false;
        }
    }
    memcpy(out, cmd, n);
    out[n] = '\0';
    return true;
}
