/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Real-world RTCM3 streams (test/fixtures) against independent decodes
 * (pyrtcm, cross-checked against go-gnss for the MSM content): framing,
 * CRC-24Q and resync over mixed and noisy streams, every MSM header field,
 * mask and per-cell C/N0, zero-satellite MSMs, a signal announced without
 * cells, epoch grouping by epoch time as well as the multiple-message bit,
 * the 1033 descriptor strings, binary receiver logs, an HTTP chunked body
 * and our own chunk encoder.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture_util.h"
#include "gnss_framing.h"
#include "gnss_obs_parse.h"
#include "gnss_rtcm3.h"
#include "ntrip_proto.h"
#include "rtcm_epoch.h"
#include "unity.h"

#define WEEK_MS 604800000u

static bool is_msm_number(uint16_t t)
{
    unsigned d = t % 10u;
    return t >= 1071 && t <= 1137 && d >= 1 && d <= 7;
}

static bool is_sbas_number(uint16_t t)
{
    return t >= 1101 && t <= 1107;
}

/* Fixtures with RTCM content, in index order. */
static size_t rtcm_fixtures(const char **names, size_t cap)
{
    size_t n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, fx_index())
    {
        if (cJSON_GetObjectItemCaseSensitive(e, "frames_ok") != NULL && n < cap) {
            names[n++] = e->string;
        }
    }
    return n;
}

static bool load_oracle(const char *name, fx_jsonl_t *j)
{
    char rel[160];
    snprintf(rel, sizeof(rel), "oracle/%s.msm.jsonl", name);
    return fx_jsonl_load(rel, j);
}

static const cJSON *item(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    TEST_ASSERT_NOT_NULL_MESSAGE(v, k);
    return v;
}

static long num(const cJSON *o, const char *k)
{
    return fx_int(o, k);
}

/* Terminator bytes right after a line: CR LF, CR or LF. */
static size_t terminator_len(const uint8_t *buf, size_t len, const fx_unit_t *u)
{
    size_t k = u->off + u->len;
    if (k >= len || (buf[k] != '\r' && buf[k] != '\n')) {
        return 0;
    }
    return (buf[k] == '\r' && k + 1 < len && buf[k + 1] == '\n') ? 2 : 1;
}

TEST_CASE("fixtures: the framer emits exactly the oracle's frames and lines, fed in any piece size", "[fixtures]")
{
    const char *names[64];
    size_t nf = rtcm_fixtures(names, 64);
    TEST_ASSERT_GREATER_OR_EQUAL(25, nf);
    static const size_t k_pieces[] = { 0, 1, 7, 100, 1029 };

    for (size_t f = 0; f < nf; f++) {
        const cJSON *e = fx_entry(names[f]);
        size_t len;
        uint8_t *buf = fx_stream(names[f], &len);
        for (size_t p = 0; p < sizeof(k_pieces) / sizeof(k_pieces[0]); p++) {
            char msg[200];
            snprintf(msg, sizeof(msg), "%s, pieces of %zu", names[f], k_pieces[p]);
            fx_units_t u;
            fx_frame(buf, len, k_pieces[p], &u);

            /* Frames: total and per message number. */
            size_t frames = 0;
            size_t frame_bytes = 0;
            size_t lines = 0;
            size_t line_bytes = 0;
            for (size_t i = 0; i < u.n; i++) {
                if (u.u[i].kind == GNSS_FRAME_RTCM3) {
                    frames++;
                    frame_bytes += u.u[i].len;
                    TEST_ASSERT_TRUE_MESSAGE(gnss_rtcm3_frame_ok(u.u[i].data, u.u[i].len), msg);
                } else if (u.u[i].kind == GNSS_FRAME_TEXT) {
                    lines++;
                    line_bytes += u.u[i].len + terminator_len(buf, len, &u.u[i]);
                }
            }
            TEST_ASSERT_EQUAL_size_t_MESSAGE((size_t)num(e, "frames_ok"), frames, msg);
            TEST_ASSERT_EQUAL_UINT64_MESSAGE(frames, u.stats.rtcm_frames, msg);
            const cJSON *mc;
            size_t counted = 0;
            cJSON_ArrayForEach(mc, item(e, "message_counts"))
            {
                unsigned type = (unsigned)atoi(mc->string);
                size_t ours = 0;
                for (size_t i = 0; i < u.n; i++) {
                    ours += u.u[i].kind == GNSS_FRAME_RTCM3 && u.u[i].type == type;
                }
                char m2[240];
                snprintf(m2, sizeof(m2), "%s: message %u", msg, type);
                TEST_ASSERT_EQUAL_size_t_MESSAGE((size_t)mc->valuedouble, ours, m2);
                counted += ours;
            }
            TEST_ASSERT_EQUAL_size_t_MESSAGE(frames, counted, msg);
            TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)num(e, "crc_fail_candidates"), u.stats.rtcm_crc_errors, msg);

            /* Checksum-valid text lines (NMEA, receiver logs). */
            TEST_ASSERT_EQUAL_size_t_MESSAGE((size_t)num(e, "text_lines"), lines, msg);

            /* Every byte is in a frame, a line, the discarded count, or the
             * tail that is still being collected at the end. */
            size_t skipped = (size_t)num(e, "skipped_bytes");
            size_t tail = (size_t)num(e, "truncated_tail_bytes");
            TEST_ASSERT_EQUAL_UINT64_MESSAGE(len, u.stats.bytes_in, msg);
            TEST_ASSERT_EQUAL_UINT64_MESSAGE(skipped, u.stats.bytes_discarded + line_bytes, msg);
            TEST_ASSERT_EQUAL_size_t_MESSAGE(len, frame_bytes + skipped + tail, msg);
            fx_units_free(&u);
        }
        free(buf);
    }
}

/* Compare one decoded MSM with its oracle record. */
static void check_msm(const char *name, const fx_unit_t *fr, const cJSON *rec)
{
    char msg[200];
    snprintf(msg, sizeof(msg), "%s @%zu type %u", name, fr->off, fr->type);
    TEST_ASSERT_EQUAL_size_t_MESSAGE((size_t)num(rec, "off"), fr->off, msg);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE((uint16_t)num(rec, "type"), fr->type, msg);

    gnss_rtcm3_msm_t h;
    if (is_sbas_number(fr->type)) {
        /* Not decoded as an MSM by design (the receiver may reuse 1101..1107);
         * the frame itself is CRC-valid and counted. */
        TEST_ASSERT_FALSE_MESSAGE(gnss_rtcm3_msm_decode(fr->data, fr->len, &h), msg);
        return;
    }
    TEST_ASSERT_TRUE_MESSAGE(gnss_rtcm3_msm_decode(fr->data, fr->len, &h), msg);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(item(rec, "sys")->valuestring, fx_sys_name(h.sys), msg);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)num(rec, "msm"), h.msm, msg);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE((uint16_t)num(rec, "station"), h.station_id, msg);
    uint32_t epoch = (uint32_t)num(rec, "epoch");
    const cJSON *dow = cJSON_GetObjectItemCaseSensitive(rec, "dow");
    if (h.sys == GNSS_SYS_GLONASS) {
        /* The oracle splits the GLONASS epoch into day of week and time of day. */
        TEST_ASSERT_NOT_NULL_MESSAGE(dow, msg);
        epoch |= (uint32_t)dow->valuedouble << 27;
    } else {
        TEST_ASSERT_NULL_MESSAGE(dow, msg);
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(epoch, h.epoch_time, msg);
    TEST_ASSERT_EQUAL_MESSAGE(num(rec, "mmb") != 0, h.multiple_message, msg);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)num(rec, "iods"), h.iods, msg);

    /* Satellite and signal masks, in ascending ID order. */
    const cJSON *sats = item(rec, "sats");
    const cJSON *sigs = item(rec, "sigs");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)cJSON_GetArraySize(sats), h.num_sats, msg);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)cJSON_GetArraySize(sigs), h.num_sigs, msg);
    unsigned sat_id[64];
    unsigned sig_id[32];
    unsigned ns = 0;
    unsigned ng = 0;
    for (unsigned i = 0; i < 64; i++) {
        if (h.sat_mask & ((uint64_t)1 << i)) {
            sat_id[ns] = i + 1;
            TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)cJSON_GetArrayItem(sats, (int)ns)->valuedouble, i + 1, msg);
            ns++;
        }
    }
    for (unsigned i = 0; i < 32; i++) {
        if (h.sig_mask & ((uint32_t)1 << i)) {
            sig_id[ng] = i + 1;
            TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)cJSON_GetArrayItem(sigs, (int)ng)->valuedouble, i + 1, msg);
            ng++;
        }
    }

    /* Cells: the cell mask is Nsat x Nsig bits, satellite-major; every set
     * bit is one cell in sent order. */
    const cJSON *cells = item(rec, "cells");
    const unsigned ncell = (unsigned)cJSON_GetArraySize(cells);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(ncell, h.num_cells, msg);
    uint16_t q4[GNSS_RTCM3_MSM_MAX_CELLS];
    obs_cnr_t cnr;
    bool have_cnr = obs_msm_cnr(fr->data, fr->len, &h, &cnr, q4);
    TEST_ASSERT_EQUAL_MESSAGE(h.msm >= 4, have_cnr, msg);
    unsigned k = 0;
    for (unsigned x = 0; x < ns * ng; x++) {
        if (!(h.cell_mask & ((uint64_t)1 << x))) {
            continue;
        }
        TEST_ASSERT_LESS_THAN_UINT_MESSAGE(ncell, k, msg);
        const cJSON *c = cJSON_GetArrayItem(cells, (int)k);
        TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)cJSON_GetArrayItem(c, 0)->valuedouble, sat_id[x / ng], msg);
        TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)cJSON_GetArrayItem(c, 1)->valuedouble, sig_id[x % ng], msg);
        const cJSON *raw = cJSON_GetArrayItem(c, 2);
        if (h.msm >= 4) {
            /* DF403 (whole dB-Hz) for MSM4/5, DF408 (1/16 dB-Hz) for MSM6/7. */
            unsigned want = (unsigned)raw->valuedouble * ((h.msm <= 5) ? 16u : 1u);
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(want, q4[k], msg);
        } else {
            TEST_ASSERT_TRUE_MESSAGE(cJSON_IsNull(raw), msg);
        }
        k++;
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(ncell, k, msg);
}

TEST_CASE("fixtures: every MSM header, mask and C/N0 cell matches the oracle", "[fixtures]")
{
    const char *names[64];
    size_t nf = rtcm_fixtures(names, 64);
    size_t compared = 0;
    size_t want = 0;
    for (size_t f = 0; f < nf; f++) {
        want += (size_t)num(fx_entry(names[f]), "msm_messages");
        fx_jsonl_t j;
        if (!load_oracle(names[f], &j)) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, num(fx_entry(names[f]), "msm_messages"), names[f]);
            continue;
        }
        size_t len;
        uint8_t *buf = fx_stream(names[f], &len);
        fx_units_t u;
        fx_frame(buf, len, 0, &u);
        size_t r = 0;
        for (size_t i = 0; i < u.n; i++) {
            if (u.u[i].kind != GNSS_FRAME_RTCM3 || !is_msm_number(u.u[i].type)) {
                continue;
            }
            TEST_ASSERT_LESS_THAN_size_t_MESSAGE(j.n, r, names[f]);
            check_msm(names[f], &u.u[i], j.rec[r]);
            r++;
        }
        TEST_ASSERT_EQUAL_size_t_MESSAGE(j.n, r, names[f]);
        compared += r;
        fx_units_free(&u);
        free(buf);
        fx_jsonl_free(&j);
    }
    TEST_ASSERT_EQUAL_size_t(want, compared);
    TEST_ASSERT_GREATER_OR_EQUAL(800, compared);
}

/*
 * All MSM frames of one fixture with their decoded headers (SBAS numbers
 * left out), and the epoch each belongs to by the oracle: a new epoch after
 * a message with the multiple-message bit clear (SBAS ones included), or
 * when a constellation already in the epoch comes with another epoch time
 * (a closing message that never came).
 *
 * The three per-MSM arrays are on the heap: together about 30 KB, which does
 * not fit the stack of the FreeRTOS task the tests run in (on Linux that
 * task's stack is the real thread stack).
 */
#define MSM_LIST_MAX 512

typedef struct {
    fx_units_t u;
    uint8_t *buf;
    size_t len;
    gnss_rtcm3_msm_t *h;     /* MSM_LIST_MAX each */
    const fx_unit_t **fr;
    unsigned *grp;
    unsigned groups;
    size_t n;
} msm_list_t;

static void msm_list(const char *name, msm_list_t *m)
{
    memset(m, 0, sizeof(*m));
    m->h = calloc(MSM_LIST_MAX, sizeof(m->h[0]));
    m->fr = calloc(MSM_LIST_MAX, sizeof(m->fr[0]));
    m->grp = calloc(MSM_LIST_MAX, sizeof(m->grp[0]));
    TEST_ASSERT_TRUE(m->h != NULL && m->fr != NULL && m->grp != NULL);
    fx_jsonl_t j;
    TEST_ASSERT_TRUE_MESSAGE(load_oracle(name, &j), name);
    static unsigned rec_grp[1024];
    TEST_ASSERT_LESS_OR_EQUAL_size_t(1024, j.n);
    unsigned g = 0;
    bool closed = false;
    char seen_sys[8][12];
    long seen_epoch[8];
    unsigned nseen = 0;
    for (size_t r = 0; r < j.n; r++) {
        const char *sys = item(j.rec[r], "sys")->valuestring;
        long epoch = num(j.rec[r], "epoch");
        bool clash = false;
        for (unsigned k = 0; k < nseen; k++) {
            clash |= strcmp(seen_sys[k], sys) == 0 && seen_epoch[k] != epoch;
        }
        if (r > 0 && (closed || clash)) {
            g++;
            nseen = 0;
        }
        unsigned k = 0;
        while (k < nseen && strcmp(seen_sys[k], sys) != 0) {
            k++;
        }
        if (k == nseen) {
            TEST_ASSERT_LESS_THAN_UINT(8, nseen);
            snprintf(seen_sys[nseen++], sizeof(seen_sys[0]), "%s", sys);
        }
        seen_epoch[k] = epoch;
        closed = num(j.rec[r], "mmb") == 0;
        rec_grp[r] = g;
    }
    m->groups = j.n ? g + 1 : 0;

    m->buf = fx_stream(name, &m->len);
    fx_frame(m->buf, m->len, 0, &m->u);
    size_t r = 0;
    for (size_t i = 0; i < m->u.n; i++) {
        const fx_unit_t *fr = &m->u.u[i];
        if (fr->kind != GNSS_FRAME_RTCM3 || !is_msm_number(fr->type)) {
            continue;
        }
        while (r < j.n && (size_t)num(j.rec[r], "off") != fr->off) {
            r++;
        }
        TEST_ASSERT_LESS_THAN_size_t(j.n, r);
        if (!is_sbas_number(fr->type)) {
            TEST_ASSERT_LESS_THAN_size_t(MSM_LIST_MAX, m->n);
            TEST_ASSERT_TRUE(gnss_rtcm3_msm_decode(fr->data, fr->len, &m->h[m->n]));
            m->grp[m->n] = rec_grp[r];
            m->fr[m->n++] = fr;
        }
        r++;
    }
    fx_jsonl_free(&j);
}

static void msm_list_free(msm_list_t *m)
{
    fx_units_free(&m->u);
    free(m->buf);
    free(m->h);
    free(m->fr);
    free(m->grp);
}

TEST_CASE("fixtures: zero-satellite MSMs decode, SBAS numbers are framed but not decoded", "[fixtures]")
{
    /* The native receiver sends QZSS 1117 with no satellite every epoch. */
    msm_list_t m;
    msm_list("centipede_LAX_30ep.rtcm3", &m);
    unsigned empty = 0;
    for (size_t i = 0; i < m.n; i++) {
        if (m.h[i].msg_type != 1117) {
            continue;
        }
        empty++;
        TEST_ASSERT_EQUAL_UINT8(0, m.h[i].num_sats);
        TEST_ASSERT_EQUAL_UINT8(0, m.h[i].num_sigs);
        TEST_ASSERT_EQUAL_UINT8(0, m.h[i].num_cells);
        TEST_ASSERT_EQUAL_UINT64(0, m.h[i].cell_mask);
        obs_cnr_t cnr;
        uint16_t q4[GNSS_RTCM3_MSM_MAX_CELLS];
        TEST_ASSERT_TRUE(obs_msm_cnr(m.fr[i]->data, m.fr[i]->len, &m.h[i], &cnr, q4));
        for (unsigned b = 0; b < GNSS_BAND_COUNT; b++) {
            TEST_ASSERT_EQUAL_UINT16(0, cnr.cells[b]);
        }
    }
    TEST_ASSERT_EQUAL_UINT(30, empty);
    /* 1107 (SBAS, also empty) is framed every epoch but kept out of MSM decoding. */
    unsigned sbas = 0;
    for (size_t i = 0; i < m.u.n; i++) {
        sbas += m.u.u[i].type == 1107;
    }
    TEST_ASSERT_EQUAL_UINT(30, sbas);
    msm_list_free(&m);

    /* The published 1104 with no satellite: a 22-byte payload. */
    size_t len;
    uint8_t *f = fx_read("rtcm/go-gnss_1104_frame.bin", &len);
    TEST_ASSERT_EQUAL_size_t(28, len);
    TEST_ASSERT_TRUE(gnss_rtcm3_frame_ok(f, len));
    TEST_ASSERT_EQUAL_UINT16(1104, gnss_rtcm3_msg_type(f, len));
    gnss_rtcm3_msm_t h;
    TEST_ASSERT_FALSE(gnss_rtcm3_msm_decode(f, len, &h));
    rtcm_msm_head_t head;
    TEST_ASSERT_FALSE(rtcm_msm_head(f, len, 1104, &head));
    free(f);
}

TEST_CASE("fixtures: the cell mask is sized by satellites x signals, also for a signal without cells", "[fixtures]")
{
    /* Every native 1127 here announces BeiDou signal 14 (B2I) in its signal
     * mask without a single cell for it. */
    msm_list_t m;
    msm_list("centipede_ROBU1_35ep.rtcm3", &m);
    unsigned n1127 = 0;
    for (size_t i = 0; i < m.n; i++) {
        const gnss_rtcm3_msm_t *h = &m.h[i];
        if (h->msg_type != 1127) {
            continue;
        }
        n1127++;
        TEST_ASSERT_TRUE(h->sig_mask & (1u << 13));
        unsigned grid = (unsigned)h->num_sats * h->num_sigs;
        TEST_ASSERT_LESS_OR_EQUAL_UINT(64, grid);
        TEST_ASSERT_LESS_THAN_UINT(grid, h->num_cells);
        /* Position of signal 14 among the sent signals. */
        unsigned pos = 0;
        for (unsigned id = 1; id < 14; id++) {
            pos += (h->sig_mask >> (id - 1)) & 1u;
        }
        for (unsigned s = 0; s < h->num_sats; s++) {
            TEST_ASSERT_FALSE(h->cell_mask & ((uint64_t)1 << (s * h->num_sigs + pos)));
        }
        TEST_ASSERT_EQUAL_UINT32(h->sig_mask & ~(1u << 13), gnss_rtcm3_msm_sigs_with_cells(h));
        TEST_ASSERT_EQUAL_UINT64(h->sat_mask, gnss_rtcm3_msm_sats_with_cells(h));
    }
    TEST_ASSERT_EQUAL_UINT(34, n1127);
    msm_list_free(&m);
}

static void expected_epochs(const msm_list_t *m, unsigned *grp)
{
    memcpy(grp, m->grp, m->n * sizeof(grp[0]));
}

TEST_CASE("fixtures: epochs group by epoch time as well as the multiple-message bit", "[fixtures]")
{
    static const struct {
        const char *name;
        unsigned groups;
        int by_time;          /* epochs started on the epoch time (closing MSM never came or not
                               * an MSM to us); -1: every epoch after the first */
        uint32_t bds_lag_ms;  /* BeiDou epoch time behind the GPS one */
    } k[] = {
        { "centipede_LAX_30ep.rtcm3", 30, 0, 14000 },
        { "centipede_ROBU1_35ep.rtcm3", 35, 1, 14000 },
        { "centipede_BGDD_30ep.rtcm3", 30, 0, 14000 },
        { "pygnssutils_ntrip_chunked.bin", 0, 0, 14000 },
        { "pygnssutils_nmea_rtcm3_excerpt.log", 0, 0, 14000 },
        /* The last MSM of every epoch here is SBAS 1107, which the epoch
         * rules (and the station, before the hub) leave out: each epoch can
         * only start on its epoch time, and its first MSM (GPS) is of a
         * constellation the open epoch does not hold yet. */
        { "gpsd_rtcm3.2.log", 0, -1, 14000 },
        { "gpsd_ublox-zed-f9t-rtcm3.log", 0, 0, 14000 },
        /* A 2012 encoder that put GPS time into its BeiDou MSMs: the epoch
         * rules compare times within one constellation only, so it groups
         * the same. */
        { "rtklib_GMSD7_12ep.rtcm3", 12, 0, 0 },
        { "pyrtcm_NTRIP-USCL00CHL0.log", 1, 0, 14000 },
    };
    static unsigned grp[MSM_LIST_MAX];
    static uint32_t got[MSM_LIST_MAX];
    for (size_t f = 0; f < sizeof(k) / sizeof(k[0]); f++) {
        msm_list_t m;
        msm_list(k[f].name, &m);
        expected_epochs(&m, grp);

        /* The hub's epoch tracker over every frame (no arrival gaps). */
        rtcm_epoch_t e;
        rtcm_epoch_init(&e, 1000, 1);
        size_t mi = 0;
        for (size_t i = 0; i < m.u.n; i++) {
            const fx_unit_t *fr = &m.u.u[i];
            if (fr->kind != GNSS_FRAME_RTCM3) {
                continue;
            }
            uint32_t ep = rtcm_epoch_assign(&e, fr->data, fr->len, fr->type, 0, true, NULL, NULL);
            if (mi < m.n && m.fr[mi] == fr) {
                got[mi++] = ep;
            }
        }
        TEST_ASSERT_EQUAL_size_t(m.n, mi);
        /* Same partition: epoch numbers advance exactly where the groups do. */
        for (size_t i = 1; i < m.n; i++) {
            char msg[160];
            snprintf(msg, sizeof(msg), "%s MSM %zu (type %u)", k[f].name, i, m.h[i].msg_type);
            TEST_ASSERT_EQUAL_MESSAGE(grp[i] != grp[i - 1], got[i] != got[i - 1], msg);
        }
        uint64_t by_time = k[f].by_time >= 0 ? (uint64_t)k[f].by_time : (uint64_t)(m.groups - 1u);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(by_time, e.by_time, k[f].name);
        if (k[f].groups != 0) {
            TEST_ASSERT_EQUAL_UINT_MESSAGE(k[f].groups, grp[m.n - 1] + 1, k[f].name);
        }

        /* BeiDou time runs 14 s behind GPS time: within one epoch the BDS
         * epoch time is the GPS one minus 14 000 ms (modulo the week). */
        unsigned pairs = 0;
        for (size_t a = 0; a < m.n; a++) {
            if (m.h[a].sys != GNSS_SYS_GPS) {
                continue;
            }
            for (size_t b = 0; b < m.n; b++) {
                if (m.h[b].sys == GNSS_SYS_BEIDOU && grp[b] == grp[a]) {
                    TEST_ASSERT_EQUAL_UINT32_MESSAGE((m.h[a].epoch_time + WEEK_MS - k[f].bds_lag_ms) % WEEK_MS,
                                                     m.h[b].epoch_time, k[f].name);
                    pairs++;
                }
            }
        }
        if (strncmp(k[f].name, "centipede_", 10) == 0) {
            TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(30, pairs, k[f].name);
        }
        msm_list_free(&m);
    }
}

TEST_CASE("fixtures: a constellation split over two MSMs of one type stays one epoch", "[fixtures]")
{
    /* This caster stream sends BeiDou as two 1127 per epoch (the first with
     * the multiple-message bit set) ahead of the other constellations. */
    msm_list_t m;
    msm_list("pygnssutils_ntrip_chunked.bin", &m);
    unsigned pairs = 0;
    for (size_t i = 1; i < m.n; i++) {
        if (m.h[i].msg_type == 1127 && m.h[i - 1].msg_type == 1127) {
            pairs++;
            TEST_ASSERT_EQUAL_UINT32(m.h[i - 1].epoch_time, m.h[i].epoch_time);
            TEST_ASSERT_TRUE(m.h[i - 1].multiple_message);
            TEST_ASSERT_EQUAL_UINT64(0, m.h[i - 1].sat_mask & m.h[i].sat_mask);   /* disjoint satellites */
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL(10, pairs);

    rtcm_epoch_t e;
    rtcm_epoch_init(&e, 1000, 1);
    uint32_t prev = 0;
    for (size_t i = 0; i < m.n; i++) {
        uint32_t ep = rtcm_epoch_assign(&e, m.fr[i]->data, m.fr[i]->len, m.fr[i]->type, 0, true, NULL, NULL);
        if (i > 0 && m.h[i].msg_type == 1127 && m.h[i - 1].msg_type == 1127) {
            TEST_ASSERT_EQUAL_UINT32(prev, ep);
        }
        prev = ep;
    }
    TEST_ASSERT_EQUAL_UINT64(0, e.by_time);
    msm_list_free(&m);
}

TEST_CASE("fixtures: 1033 descriptor strings match the oracle", "[fixtures]")
{
    const char *names[64];
    size_t nf = rtcm_fixtures(names, 64);
    unsigned checked = 0;
    for (size_t f = 0; f < nf; f++) {
        const cJSON *d1033 = item(fx_entry(names[f]), "d1033");
        if (cJSON_GetArraySize(d1033) == 0) {
            continue;
        }
        size_t len;
        uint8_t *buf = fx_stream(names[f], &len);
        const cJSON *d;
        cJSON_ArrayForEach(d, d1033)
        {
            size_t off = (size_t)num(d, "offset");
            TEST_ASSERT_LESS_THAN_size_t(len, off + 6);
            size_t flen = (((size_t)buf[off + 1] & 3u) << 8 | buf[off + 2]) + 6u;
            TEST_ASSERT_TRUE(gnss_rtcm3_frame_ok(buf + off, flen));
            obs_1033_t o;
            TEST_ASSERT_TRUE_MESSAGE(obs_1033_parse(buf + off, flen, &o), names[f]);
            TEST_ASSERT_EQUAL_UINT16(num(d, "station"), o.station_id);
            TEST_ASSERT_EQUAL_STRING(item(d, "antenna")->valuestring, o.antenna);
            TEST_ASSERT_EQUAL_UINT8(num(d, "setup_id"), o.setup_id);
            TEST_ASSERT_EQUAL_STRING(item(d, "antenna_serial")->valuestring, o.antenna_serial);
            TEST_ASSERT_EQUAL_STRING(item(d, "receiver_type")->valuestring, o.receiver_type);
            TEST_ASSERT_EQUAL_STRING(item(d, "receiver_fw")->valuestring, o.receiver_fw);
            TEST_ASSERT_EQUAL_STRING(item(d, "receiver_serial")->valuestring, o.receiver_serial);
            checked++;
        }
        free(buf);
    }
    TEST_ASSERT_GREATER_OR_EQUAL(4, checked);
}

TEST_CASE("fixtures: binary logs from a real receiver are skipped whole", "[fixtures]")
{
    size_t len;
    uint8_t *buf = fx_read("unicore/pyunigps_um981.log", &len);
    fx_units_t u;
    fx_frame(buf, len, 0, &u);
    TEST_ASSERT_EQUAL_size_t(0, u.n);
    TEST_ASSERT_EQUAL_UINT64(39, u.stats.bin_skipped);
    TEST_ASSERT_EQUAL_UINT64(len, u.stats.bytes_discarded);
    TEST_ASSERT_EQUAL_UINT64(0, u.stats.rtcm_crc_errors);
    TEST_ASSERT_EQUAL_UINT64(0, u.stats.text_aborts + u.stats.text_rejected);
    fx_units_free(&u);
    free(buf);

    /* The same logs between NMEA sentences and RTCM frames. */
    buf = fx_stream("pyunigps_mixed.log", &len);
    fx_frame(buf, len, 1, &u);
    TEST_ASSERT_EQUAL_UINT64(39, u.stats.bin_skipped);
    TEST_ASSERT_EQUAL_UINT64(num(fx_entry("pyunigps_mixed.log"), "frames_ok"), u.stats.rtcm_frames);
    TEST_ASSERT_EQUAL_UINT64(2, u.stats.text_lines);
    fx_units_free(&u);
    free(buf);
}

TEST_CASE("fixtures: epochs sent to a caster count an epoch whose closing MSM never came", "[fixtures]")
{
    /* Whole frames in reads of at most NTRIP_CHUNK_MAX_PAYLOAD bytes, as the
     * hub hands them to a caster slot; one tracker across the reads. An epoch
     * ends at its closing MSM, or (when that never came) at the next epoch
     * time of a constellation it already holds. */
    static const char *const k[] = {
        "centipede_ROBU1_35ep.rtcm3",
        "centipede_LAX_30ep.rtcm3",
        "pygnssutils_ntrip_chunked.bin",
        "rtklib_GMSD7_12ep.rtcm3",
    };
    static uint8_t rd[NTRIP_CHUNK_MAX_PAYLOAD];
    static unsigned grp[MSM_LIST_MAX];
    for (size_t f = 0; f < sizeof(k) / sizeof(k[0]); f++) {
        msm_list_t m;
        msm_list(k[f], &m);
        expected_epochs(&m, grp);
        /* Every group ended, except a last one still waiting for its closer. */
        uint32_t want = grp[m.n - 1] + (m.h[m.n - 1].multiple_message ? 0u : 1u);
        if (strcmp(k[f], "centipede_ROBU1_35ep.rtcm3") == 0) {
            TEST_ASSERT_EQUAL_UINT32(35, want);
        }

        ntrip_epoch_track_t tr;
        ntrip_epoch_track_init(&tr);
        uint32_t frames = 0;
        uint32_t epochs = 0;
        uint32_t total = 0;
        size_t fill = 0;
        for (size_t i = 0; i <= m.u.n; i++) {
            const fx_unit_t *fr = i < m.u.n ? &m.u.u[i] : NULL;
            if (fr != NULL && fr->kind != GNSS_FRAME_RTCM3) {
                continue;
            }
            if (fill > 0 && (fr == NULL || fill + fr->len > sizeof(rd))) {
                uint32_t nf = 0;
                uint32_t ne = 0;
                ntrip_count_frames(rd, fill, &tr, &nf, &ne);
                frames += nf;
                epochs += ne;
                fill = 0;
            }
            if (fr != NULL) {
                memcpy(rd + fill, fr->data, fr->len);
                fill += fr->len;
                total++;
            }
        }
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(total, frames, k[f]);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(want, epochs, k[f]);
        msm_list_free(&m);
    }
}

TEST_CASE("fixtures: a sentence right after a stray lead byte is not lost", "[fixtures]")
{
    /* In this capture a '#' (the last byte of a binary message) sits right
     * in front of "$GNRMC,084220.00,...": the sentence must still come out. */
    size_t len;
    uint8_t *buf = fx_stream("pygnssutils_nmea_rtcm3_excerpt.log", &len);
    static const char k_line[] = "$GNRMC,084220.00,A,3203.94995,N,03446.42914,E,0.000,,080222,,,D,V*12";
    const uint8_t *at = NULL;
    for (size_t i = 1; i + sizeof(k_line) - 1 <= len; i++) {
        if (memcmp(buf + i, k_line, sizeof(k_line) - 1) == 0) {
            at = buf + i;
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(at);
    TEST_ASSERT_EQUAL_CHAR('#', at[-1]);
    fx_units_t u;
    fx_frame(buf, len, 0, &u);
    bool found = false;
    for (size_t i = 0; i < u.n; i++) {
        if (u.u[i].kind == GNSS_FRAME_TEXT && u.u[i].len == sizeof(k_line) - 1 &&
            memcmp(u.u[i].data, k_line, u.u[i].len) == 0) {
            found = true;
        }
    }
    TEST_ASSERT_TRUE(found);
    fx_units_free(&u);
    free(buf);
}

TEST_CASE("fixtures: a caster's chunked body, and our chunk encoder round trip", "[fixtures]")
{
    /* The caster's body: 100-byte chunks that cut RTCM frames anywhere; the
     * capture ends inside a size line. */
    size_t raw_len;
    uint8_t *raw = fx_read("rtcm/pygnssutils_ntrip_chunked.bin", &raw_len);
    uint8_t *body = malloc(raw_len);
    TEST_ASSERT_NOT_NULL(body);
    size_t chunks;
    bool complete;
    size_t body_len = fx_dechunk(raw, raw_len, body, &chunks, &complete);
    TEST_ASSERT_FALSE(complete);
    TEST_ASSERT_EQUAL_size_t(246, chunks);

    /* Fed to the framer chunk by chunk, as a client receives it. */
    fx_units_t u;
    fx_frame(body, body_len, 100, &u);
    TEST_ASSERT_EQUAL_UINT64(num(fx_entry("pygnssutils_ntrip_chunked.bin"), "frames_ok"), u.stats.rtcm_frames);

    /* Our encoder: whole frames per hub read (at most NTRIP_CHUNK_MAX_PAYLOAD
     * bytes), one chunk each, then the terminator; the same decoder gets the
     * frames back byte for byte and the frame and epoch counts add up. */
    static uint8_t tx[NTRIP_CHUNK_HEAD_RESERVE + NTRIP_CHUNK_MAX_PAYLOAD + NTRIP_CHUNK_TAIL];
    uint8_t *wire = malloc(raw_len * 2 + 16);
    uint8_t *frames = malloc(raw_len);
    TEST_ASSERT_NOT_NULL(wire);
    TEST_ASSERT_NOT_NULL(frames);
    size_t wire_len = 0;
    size_t frames_len = 0;
    uint32_t total_frames = 0;
    uint32_t total_epochs = 0;
    unsigned closers = 0;
    unsigned reads = 0;
    ntrip_epoch_track_t track;
    ntrip_epoch_track_init(&track);
    size_t i = 0;
    while (i < u.n) {
        size_t fill = 0;
        while (i < u.n && (u.u[i].kind != GNSS_FRAME_RTCM3 || fill + u.u[i].len <= NTRIP_CHUNK_MAX_PAYLOAD)) {
            if (u.u[i].kind == GNSS_FRAME_RTCM3) {
                memcpy(tx + NTRIP_CHUNK_HEAD_RESERVE + fill, u.u[i].data, u.u[i].len);
                memcpy(frames + frames_len, u.u[i].data, u.u[i].len);
                fill += u.u[i].len;
                frames_len += u.u[i].len;
                gnss_rtcm3_msm_t h;
                closers += gnss_rtcm3_msm_decode(u.u[i].data, u.u[i].len, &h) && !h.multiple_message;
            }
            i++;
        }
        if (fill == 0) {
            continue;
        }
        uint32_t nf = 0;
        uint32_t ne = 0;
        ntrip_count_frames(tx + NTRIP_CHUNK_HEAD_RESERVE, fill, &track, &nf, &ne);
        total_frames += nf;
        total_epochs += ne;
        size_t start = 0;
        size_t n = ntrip_chunk_wrap(tx, fill, &start);
        TEST_ASSERT_EQUAL_size_t(fill + NTRIP_CHUNK_TAIL + (NTRIP_CHUNK_HEAD_RESERVE - start), n);
        memcpy(wire + wire_len, tx + start, n);
        wire_len += n;
        reads++;
    }
    memcpy(wire + wire_len, NTRIP_CHUNK_TERMINATOR, strlen(NTRIP_CHUNK_TERMINATOR));
    wire_len += strlen(NTRIP_CHUNK_TERMINATOR);
    TEST_ASSERT_EQUAL_UINT32(u.stats.rtcm_frames, total_frames);
    TEST_ASSERT_EQUAL_UINT32(closers, total_epochs);

    uint8_t *back = malloc(wire_len);
    TEST_ASSERT_NOT_NULL(back);
    size_t back_len = fx_dechunk(wire, wire_len, back, &chunks, &complete);
    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_EQUAL_size_t(reads, chunks);
    TEST_ASSERT_EQUAL_size_t(frames_len, back_len);
    TEST_ASSERT_EQUAL_MEMORY(frames, back, frames_len);

    free(back);
    free(frames);
    free(wire);
    fx_units_free(&u);
    free(body);
    free(raw);
}
