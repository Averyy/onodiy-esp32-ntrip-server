/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "gnss_framing.h"

#include <string.h>

#include "gnss_rtcm3.h"

enum {
    ST_SYNC = 0,     /* between units: look for a unit start */
    ST_RTCM,         /* RTCM3 candidate in buf[0 .. fill) */
    ST_TEXT,         /* text line in buf[0 .. fill) */
    ST_TEXT_SKIP,    /* overlong line: consume up to its terminator */
    ST_BIN_HDR,      /* binary log header in buf[0 .. fill) */
    ST_BIN_SKIP,     /* binary log body and CRC: `need` bytes left */
    ST_NOTICE,       /* the receiver's restarting answer: its prefix in buf[0 .. fill) */
};

/* What the CR that ended the previous line means for a LF right after it. */
enum {
    CR_NONE = 0,
    CR_EMITTED,      /* the LF belongs to an emitted line */
    CR_DROPPED,      /* the LF belongs to a dropped line: discarded */
};

static const uint8_t k_prompt[] = { 'b', 'o', 'o', 't', '>' };
/* The matcher below restarts with "is this byte the first prompt byte?",
 * which is exact only because no proper prefix of the prompt is also a
 * suffix of it. Keep that true if the prompt text ever changes. */

static const uint8_t k_bin_sync[] = { 0xAA, 0x44, 0xB5 };

/* The one line without a lead character (gnss_framing.h). */
static const char k_notice[] = GNSS_NOTICE_PREFIX;
#define NOTICE_LEN (sizeof(k_notice) - 1u)

static bool is_text_byte(uint8_t c)
{
    return (c >= 0x20 && c <= 0x7E) || c == '\t';
}

/* ---- checksums ---------------------------------------------------------- */

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static bool parse_hex(const char *s, size_t n, uint32_t *out)
{
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) {
        int d = hex_digit(s[i]);
        if (d < 0) {
            return false;
        }
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return true;
}

/* Reflected CRC-32, polynomial 0xEDB88320, initial value 0, no final XOR. */
static uint32_t crc32_log(const char *p, size_t n)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint8_t)p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return crc;
}

bool gnss_text_line_valid(const char *line, size_t len)
{
    if (line == NULL || len < 4 || (line[0] != '$' && line[0] != '#')) {
        return false;
    }
    const char lead = line[0];
    uint32_t want;

    /* "*hh": XOR of the bytes between the lead character and '*'. */
    if (line[len - 3] == '*' && parse_hex(line + len - 2, 2, &want)) {
        uint8_t x = 0;
        for (size_t i = 1; i < len - 3; i++) {
            x ^= (uint8_t)line[i];
        }
        if ((uint8_t)(x ^ (uint8_t)lead) == want) {
            return true;                    /* lead character included */
        }
        if (lead == '$' && x == want) {
            return true;                    /* NMEA rule */
        }
    }
    /* "*hhhhhhhh": CRC32 of the bytes between '#' and '*'. */
    if (lead == '#' && len >= 10 && line[len - 9] == '*' && parse_hex(line + len - 8, 8, &want)) {
        return crc32_log(line + 1, len - 10) == want;
    }
    return false;
}

/* The restarting answer: its prefix, then "*hh", the XOR of every byte
 * before the '*' (there is no lead character to leave out or include). */
static bool notice_valid(const char *line, size_t len)
{
    uint32_t want;
    if (len < NOTICE_LEN + 3u || memcmp(line, k_notice, NOTICE_LEN) != 0 || line[len - 3] != '*' ||
        !parse_hex(line + len - 2, 2, &want)) {
        return false;
    }
    uint8_t x = 0;
    for (size_t i = 0; i < len - 3; i++) {
        x ^= (uint8_t)line[i];
    }
    return x == want;
}

/* ---- framer ------------------------------------------------------------- */

static void discard(gnss_framer_t *f, size_t n)
{
    if (n > 0) {
        f->stats.bytes_discarded += n;
        f->sync_lost = true;
    }
}

static void found_unit(gnss_framer_t *f)
{
    if (f->sync_lost) {
        f->stats.resyncs++;
        f->sync_lost = false;
    }
}

/* Stream offset of the byte being processed. Bytes still waiting to be
 * scanned again are always the most recent ones fed. */
static uint64_t cur_offset(const gnss_framer_t *f)
{
    return f->stats.bytes_in - 1u - (uint64_t)(f->rescan_end - f->rescan_pos);
}

static void start_unit(gnss_framer_t *f, uint8_t state, uint8_t c)
{
    f->buf[0] = c;
    f->fill = 1;
    f->need = 0;
    f->state = state;
}

/*
 * Reject the current candidate: its first byte is discarded and the rest is
 * scanned again, followed by whatever was still waiting from an earlier
 * rescan. Everything stays in buf: the bytes being rebuilt into a new unit
 * always sit below the bytes not yet scanned (each byte is read before at
 * most one byte is stored), so buf[1 .. fill) and the waiting tail can be
 * joined in place.
 */
static void rescan(gnss_framer_t *f)
{
    size_t waiting = f->rescan_end - f->rescan_pos;
    if (waiting > 0) {
        memmove(f->buf + f->fill, f->buf + f->rescan_pos, waiting);
    }
    f->rescan_pos = 1;
    f->rescan_end = f->fill + waiting;
    discard(f, 1);
    f->fill = 0;
    f->need = 0;
    f->state = ST_SYNC;
}

static void report_prompt(gnss_framer_t *f)
{
    f->prompt_pending = false;
    f->stats.prompts++;
    const gnss_frame_t fr = {
        .kind = GNSS_FRAME_PROMPT,
        .data = k_prompt,
        .len = sizeof(k_prompt),
        .rtcm_msg_type = 0,
        .at = f->prompt_at,
    };
    f->cb(&fr, f->ctx);
}

static void emit_rtcm(gnss_framer_t *f)
{
    size_t len = f->need;
    uint64_t end = cur_offset(f);
    uint64_t start = end + 1u - len;

    f->rtcm_seen = true;
    f->rtcm_end_at = end;
    if (f->prompt_pending && end + GNSS_PROMPT_WINDOW >= f->prompt_at &&
        start <= f->prompt_at + GNSS_PROMPT_WINDOW) {
        f->prompt_pending = false;           /* RTCM around it: not the loader */
        f->stats.prompts_ignored++;
    }
    found_unit(f);
    f->stats.rtcm_frames++;
    f->fill = 0;
    f->need = 0;
    f->state = ST_SYNC;

    const gnss_frame_t fr = {
        .kind = GNSS_FRAME_RTCM3,
        .data = f->buf,
        .len = len,
        .rtcm_msg_type = (uint16_t)(((unsigned)f->buf[3] << 4) | (f->buf[4] >> 4)),
    };
    f->cb(&fr, f->ctx);
}

static bool has_lead(const uint8_t *p, size_t n)
{
    return memchr(p, '$', n) != NULL || memchr(p, '#', n) != NULL;
}

static void end_line(gnss_framer_t *f, uint8_t term)
{
    size_t len = f->fill;
    bool valid = (f->buf[0] == '$' || f->buf[0] == '#') ? gnss_text_line_valid((const char *)f->buf, len)
                                                         : notice_valid((const char *)f->buf, len);
    if (!valid) {
        f->stats.text_rejected++;
        if (len > 1 && has_lead(f->buf + 1, len - 1)) {
            /* Junk that began with a lead byte may hold a real line after
             * it: scan the rest again, the terminator included (it stays
             * below the bytes still waiting, see rescan()). */
            f->buf[len] = term;
            f->fill = len + 1;
            rescan(f);
            f->after_cr = CR_NONE;
            return;
        }
        f->fill = 0;
        f->state = ST_SYNC;
        discard(f, len + 1);
        f->after_cr = (term == '\r') ? CR_DROPPED : CR_NONE;
        return;
    }
    f->fill = 0;
    f->state = ST_SYNC;
    f->after_cr = (term == '\r') ? CR_EMITTED : CR_NONE;
    f->buf[len] = 0;    /* below any byte still waiting to be rescanned */
    found_unit(f);
    f->stats.text_lines++;

    const gnss_frame_t fr = {
        .kind = GNSS_FRAME_TEXT,
        .data = f->buf,
        .len = len,
        .rtcm_msg_type = 0,
    };
    f->cb(&fr, f->ctx);
}

static void sync_byte(gnss_framer_t *f, uint8_t c)
{
    uint8_t after_cr = f->after_cr;
    f->after_cr = CR_NONE;
    if (c == '\n' && after_cr != CR_NONE) {
        if (after_cr == CR_DROPPED) {
            discard(f, 1);
        }
        return;             /* the LF of a CR LF terminator */
    }
    switch (c) {
    case GNSS_RTCM3_PREAMBLE:
        start_unit(f, ST_RTCM, c);
        break;
    case '$':
    case '#':
        start_unit(f, ST_TEXT, c);
        break;
    case 0xAA:
        start_unit(f, ST_BIN_HDR, c);
        break;
    case 'T':   /* the first byte of GNSS_NOTICE_PREFIX */
        start_unit(f, ST_NOTICE, c);
        break;
    default:
        discard(f, 1);
        break;
    }
}

static void step(gnss_framer_t *f, uint8_t c)
{
    for (;;) {
        switch (f->state) {
        case ST_SYNC:
            sync_byte(f, c);
            return;

        case ST_RTCM:
            f->buf[f->fill++] = c;
            if (f->fill == 2) {
                if ((c >> 2) != 0) {
                    f->stats.rtcm_reserved_nonzero++;
                    rescan(f);
                }
            } else if (f->fill == 3) {
                size_t n = ((size_t)(f->buf[1] & 0x03u) << 8) | c;
                if (n < 2) {
                    f->stats.rtcm_bad_length++;
                    rescan(f);
                } else {
                    f->need = n + GNSS_RTCM3_HEADER_LEN + GNSS_RTCM3_CRC_LEN;
                }
            } else if (f->fill == f->need) {
                if (gnss_rtcm3_crc24q(f->buf, f->need) == 0) {
                    emit_rtcm(f);
                } else {
                    f->stats.rtcm_crc_errors++;
                    rescan(f);
                }
            }
            return;

        case ST_TEXT:
            if (c == '\r' || c == '\n') {
                end_line(f, c);
                return;
            }
            if (f->fill == 1 && (c == '$' || c == '#')) {
                /* No line starts with two lead bytes: the first was noise
                 * (else "$$..." could pass the '$'-inclusive rule). */
                f->stats.text_aborts++;
                discard(f, 1);
                start_unit(f, ST_TEXT, c);
                return;
            }
            if (is_text_byte(c)) {
                if (f->fill > GNSS_TEXT_MAX_LINE - 1) {
                    f->stats.text_overflows++;
                    discard(f, f->fill + 1);
                    f->fill = 0;
                    f->state = ST_TEXT_SKIP;
                } else {
                    f->buf[f->fill++] = c;
                }
                return;
            }
            /* Not text: drop the partial line, look at the byte again. */
            f->stats.text_aborts++;
            discard(f, f->fill);
            f->fill = 0;
            f->state = ST_SYNC;
            continue;

        case ST_TEXT_SKIP:
            if (c == '\r' || c == '\n') {
                discard(f, 1);
                f->after_cr = (c == '\r') ? CR_DROPPED : CR_NONE;
                f->state = ST_SYNC;
                return;
            }
            if (is_text_byte(c)) {
                discard(f, 1);
                return;
            }
            f->state = ST_SYNC;
            continue;

        case ST_BIN_HDR:
            f->buf[f->fill++] = c;
            if (f->fill <= sizeof(k_bin_sync)) {
                if (c != k_bin_sync[f->fill - 1]) {
                    rescan(f);
                }
            } else if (f->fill == GNSS_BIN_HEADER_LEN) {
                size_t msg = (size_t)f->buf[6] | ((size_t)f->buf[7] << 8);
                if (msg > GNSS_BIN_MAX_MSG) {
                    rescan(f);           /* implausible length: a false sync */
                } else {
                    f->stats.bytes_discarded += GNSS_BIN_HEADER_LEN;
                    f->need = msg + 4u;  /* body + CRC32 */
                    f->fill = 0;
                    f->state = ST_BIN_SKIP;
                }
            }
            return;

        case ST_NOTICE:
            /* Only the exact prefix continues; any other byte ends the
             * candidate, and the bytes after its first are scanned again
             * (that byte included), so nothing behind it is lost. */
            f->buf[f->fill++] = c;
            if (c != (uint8_t)k_notice[f->fill - 1]) {
                rescan(f);
            } else if (f->fill == NOTICE_LEN) {
                f->state = ST_TEXT;   /* the rest is an ordinary line */
            }
            return;

        case ST_BIN_SKIP:
            f->stats.bytes_discarded++;
            if (--f->need == 0) {
                f->stats.bin_skipped++;
                f->state = ST_SYNC;
            }
            return;

        default:
            f->state = ST_SYNC;
            continue;
        }
    }
}

static void prompt_step(gnss_framer_t *f, uint8_t c)
{
    if (c != k_prompt[f->prompt_match]) {
        f->prompt_match = (c == k_prompt[0]) ? 1 : 0;
        return;
    }
    if (++f->prompt_match < sizeof(k_prompt)) {
        return;
    }
    f->prompt_match = 0;
    uint64_t at = f->stats.bytes_in - 1u;
    if (f->rtcm_seen && f->rtcm_end_at + GNSS_PROMPT_WINDOW >= at) {
        f->stats.prompts_ignored++;          /* RTCM just before it */
        return;
    }
    if (!f->prompt_pending) {
        f->prompt_pending = true;
        f->prompt_at = at;
    }
}

/* End of a feed: report a pending hit unless an open RTCM candidate that
 * started at or before it could still turn out valid. */
static void prompt_settle(gnss_framer_t *f)
{
    if (!f->prompt_pending) {
        return;
    }
    if (f->state == ST_RTCM && f->stats.bytes_in - f->fill <= f->prompt_at) {
        return;
    }
    report_prompt(f);
}

void gnss_framer_init(gnss_framer_t *f, gnss_frame_cb_t cb, void *ctx)
{
    memset(f, 0, sizeof(*f));
    f->cb = cb;
    f->ctx = ctx;
    f->state = ST_SYNC;
}

void gnss_framer_reset(gnss_framer_t *f)
{
    if (f->state == ST_RTCM || f->state == ST_TEXT || f->state == ST_BIN_HDR || f->state == ST_NOTICE) {
        discard(f, f->fill);
    }
    f->fill = 0;
    f->need = 0;
    f->rescan_pos = 0;
    f->rescan_end = 0;
    f->after_cr = CR_NONE;
    f->state = ST_SYNC;
    /* The prompt matcher survives: a loader prompt may straddle a baud
     * switch. A pending hit can no longer be inside a frame. */
    if (f->prompt_armed && f->prompt_pending) {
        report_prompt(f);
    }
}

void gnss_framer_feed(gnss_framer_t *f, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        const uint8_t c = data[i];
        f->stats.bytes_in++;
        step(f, c);
        while (f->rescan_pos < f->rescan_end) {
            step(f, f->buf[f->rescan_pos++]);
        }
        if (f->prompt_armed) {
            prompt_step(f, c);
        }
    }
    if (f->prompt_armed) {
        prompt_settle(f);
    }
}

void gnss_framer_get_stats(const gnss_framer_t *f, gnss_framing_stats_t *out)
{
    *out = f->stats;
}

void gnss_framer_set_prompt_detect(gnss_framer_t *f, bool armed)
{
    if (!armed || !f->prompt_armed) {
        f->prompt_match = 0;         /* a fresh arm starts clean; re-arming keeps the state */
        f->prompt_pending = false;
    }
    f->prompt_armed = armed;
}

/* ---- version log -------------------------------------------------------- */

static void copy_cut(char *dst, size_t size, const char *src, size_t n)
{
    if (n > size - 1) {
        n = size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Model and firmware decide what gets configured or flashed: they must be
 * non-empty and fit whole (a cut model could read as a supported one). */
static bool copy_whole(char *dst, size_t size, const char *src, size_t n)
{
    if (n == 0 || n > size - 1) {
        return false;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    return true;
}

static uint32_t build_of(const char *firmware)
{
    const char *p = strstr(firmware, "Build");
    if (p == NULL) {
        return 0;
    }
    p += 5;
    uint64_t v = 0;
    bool any = false;
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (uint64_t)(*p - '0');
        if (v > UINT32_MAX) {
            return 0;                    /* not a build number we can compare */
        }
        any = true;
        p++;
    }
    return any ? (uint32_t)v : 0;
}

static bool store_field(gnss_versiona_t *v, unsigned idx, const char *s, size_t n)
{
    switch (idx) {
    case 0:
        return copy_whole(v->model, sizeof(v->model), s, n);
    case 1:
        return copy_whole(v->firmware, sizeof(v->firmware), s, n);
    case 3: {
        const char *dash = memchr(s, '-', n);
        if (dash == NULL) {
            copy_cut(v->serial, sizeof(v->serial), s, n);
        } else {
            size_t pn_len = (size_t)(dash - s);
            copy_cut(v->pn, sizeof(v->pn), s, pn_len);
            copy_cut(v->serial, sizeof(v->serial), dash + 1, n - pn_len - 1);
        }
        return true;
    }
    case 4:
        copy_cut(v->efuse_id, sizeof(v->efuse_id), s, n);
        return true;
    case 5:
        copy_cut(v->compile_date, sizeof(v->compile_date), s, n);
        return true;
    default:
        return true;                     /* authorisation and later fields: not kept */
    }
}

static bool all_digits(const char *s, size_t n)
{
    if (n == 0 || n > 9) {
        return false;   /* 9 digits fit in a uint32_t */
    }
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

static bool all_letters(const char *s, size_t n)
{
    if (n == 0) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) {
            return false;
        }
    }
    return true;
}

static uint32_t digits_value(const char *s, size_t n)
{
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) {
        v = v * 10u + (uint32_t)(s[i] - '0');
    }
    return v;
}

/* The header's clock: "<cpu idle>,<time reference>,<time status>,<week>,<ms>,..." */
static void header_clock(const char *h, size_t n, gnss_versiona_t *v)
{
    const char *f[5];
    size_t fl[5];
    size_t pos = 0;
    for (unsigned i = 0; i < 5; i++) {
        if (pos > n) {
            return;
        }
        const char *comma = memchr(h + pos, ',', n - pos);
        size_t end = comma != NULL ? (size_t)(comma - h) : n;
        f[i] = h + pos;
        fl[i] = end - pos;
        if (comma == NULL && i < 4) {
            return;   /* fewer than five fields */
        }
        pos = end + 1u;
    }
    if (!all_digits(f[0], fl[0]) || !all_letters(f[1], fl[1]) || !all_letters(f[2], fl[2]) ||
        !all_digits(f[3], fl[3]) || !all_digits(f[4], fl[4])) {
        return;
    }
    v->hdr_clock = true;
    v->hdr_time_unknown = fl[2] == 7 && strncmp(f[2], "UNKNOWN", 7) == 0;
    v->hdr_week = digits_value(f[3], fl[3]);
    v->hdr_ms = digits_value(f[4], fl[4]);
}

bool gnss_versiona_uptime_ms(const gnss_versiona_t *v, uint32_t *ms)
{
    if (v == NULL || !v->hdr_clock || !v->hdr_time_unknown || v->hdr_week > 1u) {
        return false;
    }
    if (ms != NULL) {
        *ms = v->hdr_ms;
    }
    return true;
}

bool gnss_versiona_parse(const char *line, size_t len, gnss_versiona_t *out)
{
    static const char k_prefix[] = "#VERSIONA,";
    const size_t prefix_len = sizeof(k_prefix) - 1;

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
    if (len < prefix_len + 9 || memcmp(line, k_prefix, prefix_len) != 0 || line[len - 9] != '*') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!is_text_byte((uint8_t)line[i])) {
            return false;                /* e.g. a NUL in text cut from raw bytes */
        }
    }
    uint32_t want;
    if (!parse_hex(line + len - 8, 8, &want) || crc32_log(line + 1, len - 10) != want) {
        return false;
    }

    const size_t body_end = len - 9;     /* index of '*' */
    const char *semi = memchr(line + prefix_len, ';', body_end - prefix_len);
    if (semi == NULL) {
        return false;
    }
    gnss_versiona_t v;
    memset(&v, 0, sizeof(v));
    size_t pos = (size_t)(semi - line) + 1;
    unsigned fields = 0;
    for (;;) {
        const char *s = line + pos;
        const char *comma = memchr(s, ',', body_end - pos);
        size_t n = (comma != NULL) ? (size_t)(comma - s) : body_end - pos;
        if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
            s++;
            n -= 2;
        }
        if (!store_field(&v, fields, s, n)) {
            return false;
        }
        fields++;
        if (comma == NULL) {
            break;
        }
        pos = (size_t)(comma - line) + 1;
    }
    if (fields < GNSS_VERSIONA_MIN_FIELDS) {
        return false;
    }
    v.build = build_of(v.firmware);
    header_clock(line + prefix_len, (size_t)(semi - line) - prefix_len, &v);
    *out = v;
    return true;
}
