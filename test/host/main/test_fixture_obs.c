/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The observation core fed with real streams (test/fixtures): the summary
 * (satellites, signals, bands, per-band C/N0, epochs), the message table and
 * the live per-satellite list with its totals, against values computed here
 * from the pyrtcm oracle; and the satellite-in-view parser and sky view over
 * a real Unicore NMEA capture, against pynmeagps.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture_util.h"
#include "gnss_obs_core.h"
#include "gnss_obs_parse.h"
#include "gnss_rtcm3.h"
#include "unity.h"

static obs_core_t s_core;

static long num(const cJSON *o, const char *k)
{
    return fx_int(o, k);
}

static int sys_of(const char *name)
{
    for (unsigned s = 0; s < GNSS_SYS_COUNT; s++) {
        if (strcmp(name, fx_sys_name(s)) == 0) {
            return (int)s;
        }
    }
    return -1;
}

static uint8_t q4_dbhz(uint32_t q4)
{
    uint32_t v = (q4 + 8u) / 16u;
    return (uint8_t)(v == 0 ? 1 : (v > 255 ? 255 : v));
}

static unsigned popcount(uint64_t v)
{
    unsigned n = 0;
    for (; v; v &= v - 1) {
        n++;
    }
    return n;
}

/* One oracle MSM record with its arrival time. */
typedef struct {
    const cJSON *rec;
    int sys;
    uint32_t epoch;
    uint64_t t;
    unsigned group;
} rec_t;

/* Per-satellite expectation (latest epoch that listed the satellite). */
typedef struct {
    bool valid;
    uint64_t t;
    uint32_t epoch;
    uint32_t sig_mask;
    uint8_t band_mask;
    uint8_t cnr[GNSS_BAND_COUNT];
} want_sat_t;

static void feed_stream(const char *name, rec_t *recs, size_t *nrec, uint64_t *t_end)
{
    fx_jsonl_t j;
    char rel[160];
    snprintf(rel, sizeof(rel), "oracle/%s.msm.jsonl", name);
    TEST_ASSERT_TRUE(fx_jsonl_load(rel, &j));
    size_t len;
    uint8_t *buf = fx_stream(name, &len);
    fx_units_t u;
    fx_frame(buf, len, 0, &u);

    /* Arrival: one epoch per second (a new one after a closing MSM or on a
     * new epoch time of a constellation already seen), 5 ms per frame. */
    obs_core_init(&s_core);
    uint64_t t = 1000000;
    size_t r = 0;
    unsigned group = 0;
    bool closed = false;
    uint8_t seen = 0;
    uint32_t last_epoch[GNSS_SYS_COUNT] = { 0 };
    for (size_t i = 0; i < u.n; i++) {
        const fx_unit_t *fr = &u.u[i];
        if (fr->kind != GNSS_FRAME_RTCM3) {
            continue;
        }
        t += 5;
        gnss_rtcm3_msm_t h;
        bool msm = gnss_rtcm3_msm_type(fr->type, NULL, NULL) && gnss_rtcm3_msm_decode(fr->data, fr->len, &h);
        if (msm) {
            if (closed || ((seen & (1u << h.sys)) && last_epoch[h.sys] != h.epoch_time)) {
                group++;
                seen = 0;
                t = 1000000 + (uint64_t)group * 1000u;
            }
            closed = !h.multiple_message;
            seen |= (uint8_t)(1u << h.sys);
            last_epoch[h.sys] = h.epoch_time;
            /* The oracle record of this frame. */
            while (r < j.n && (size_t)num(j.rec[r], "off") != fr->off) {
                r++;
            }
            TEST_ASSERT_LESS_THAN_size_t(j.n, r);
            recs[*nrec] = (rec_t) {
                .rec = j.rec[r],
                .sys = (int)h.sys,
                .epoch = h.epoch_time,
                .t = t,
                .group = group,
            };
            (*nrec)++;
            j.rec[r++] = NULL;   /* kept by recs[] */
        }
        obs_cnr_t cnr;
        uint16_t q4[GNSS_RTCM3_MSM_MAX_CELLS];
        bool has_cnr = msm && obs_msm_cnr(fr->data, fr->len, &h, &cnr, q4);
        obs_core_feed_msg(&s_core, fr->type, msm, msm ? h.epoch_time : 0, t);
        if (msm) {
            obs_core_feed_msm(&s_core, &h, has_cnr ? &cnr : NULL, t);
            obs_core_feed_sats(&s_core, &h, has_cnr ? q4 : NULL, t);
        }
    }
    *t_end = t;
    for (size_t i = 0; i < j.n; i++) {
        cJSON_Delete(j.rec[i]);   /* NULL for the kept ones */
    }
    free(j.rec);
    fx_units_free(&u);
    free(buf);
}

static void free_recs(rec_t *recs, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        cJSON_Delete((cJSON *)recs[i].rec);
    }
}

static void check_stream(const char *name, unsigned want_groups)
{
    static rec_t recs[600];
    size_t n = 0;
    uint64_t t_end;
    feed_stream(name, recs, &n, &t_end);
    TEST_ASSERT_GREATER_THAN(0, n);
    const uint64_t now = t_end + 100;

    /* Expected per constellation: signals with cells in the window, the
     * satellites of its latest epoch, and the C/N0 per band of that epoch. */
    uint32_t want_sigs[GNSS_SYS_COUNT] = { 0 };
    uint64_t want_sats[GNSS_SYS_COUNT] = { 0 };
    uint32_t last_epoch[GNSS_SYS_COUNT] = { 0 };
    bool present[GNSS_SYS_COUNT] = { false };
    uint32_t cnr_sum[GNSS_SYS_COUNT][GNSS_BAND_COUNT];
    uint32_t cnr_n[GNSS_SYS_COUNT][GNSS_BAND_COUNT];
    memset(cnr_sum, 0, sizeof(cnr_sum));
    memset(cnr_n, 0, sizeof(cnr_n));
    static want_sat_t sat[GNSS_SYS_COUNT][64];
    memset(sat, 0, sizeof(sat));
    for (size_t i = 0; i < n; i++) {
        const rec_t *r = &recs[i];
        if (r->sys < 0 || r->sys == GNSS_SYS_SBAS) {
            continue;
        }
        const unsigned s = (unsigned)r->sys;
        const unsigned msm = (unsigned)num(r->rec, "msm");
        if (!present[s] || last_epoch[s] != r->epoch) {
            want_sats[s] = 0;
            memset(cnr_sum[s], 0, sizeof(cnr_sum[s]));
            memset(cnr_n[s], 0, sizeof(cnr_n[s]));
        }
        present[s] = true;
        last_epoch[s] = r->epoch;
        const cJSON *c;
        cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(r->rec, "cells"))
        {
            unsigned sv = (unsigned)cJSON_GetArrayItem(c, 0)->valuedouble;
            unsigned sg = (unsigned)cJSON_GetArrayItem(c, 1)->valuedouble;
            const cJSON *raw = cJSON_GetArrayItem(c, 2);
            uint32_t q4 = cJSON_IsNull(raw) ? 0 : (uint32_t)raw->valuedouble * (msm <= 5 ? 16u : 1u);
            want_sats[s] |= (uint64_t)1 << (sv - 1);
            if (now - r->t < OBS_SIGNAL_WINDOW_MS) {
                want_sigs[s] |= (uint32_t)1 << (sg - 1);
            }
            gnss_band_t b = gnss_rtcm3_msm_band((gnss_sys_t)s, sg);
            if (b < GNSS_BAND_COUNT && q4 != 0) {
                cnr_sum[s][b] += q4;
                cnr_n[s][b]++;
            }
            want_sat_t *w = &sat[s][sv - 1];
            if (!w->valid || w->epoch != r->epoch) {
                memset(w, 0, sizeof(*w));
                w->valid = true;
                w->epoch = r->epoch;
            }
            w->t = r->t;
            w->sig_mask |= (uint32_t)1 << (sg - 1);
            if (b < GNSS_BAND_COUNT) {
                w->band_mask |= (uint8_t)(1u << b);
                if (q4 != 0 && q4_dbhz(q4) > w->cnr[b]) {
                    w->cnr[b] = q4_dbhz(q4);
                }
            }
        }
    }

    obs_core_view_t v;
    obs_core_view(&s_core, now, &v);
    char msg[160];
    for (unsigned s = 0; s < GNSS_SYS_COUNT; s++) {
        if (s == GNSS_SYS_SBAS) {
            continue;
        }
        snprintf(msg, sizeof(msg), "%s: %s", name, fx_sys_name(s));
        TEST_ASSERT_EQUAL_MESSAGE(present[s], v.sys[s].present, msg);
        if (!present[s]) {
            continue;
        }
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(popcount(want_sats[s]), v.sys[s].satellites, msg);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(want_sigs[s], v.sys[s].signal_mask, msg);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(gnss_rtcm3_msm_band_mask((gnss_sys_t)s, want_sigs[s]), v.sys[s].band_mask,
                                       msg);
        for (unsigned b = 0; b < GNSS_BAND_COUNT; b++) {
            uint8_t want = 0;
            if (cnr_n[s][b] != 0 && (v.sys[s].band_mask & (1u << b))) {
                want = q4_dbhz((cnr_sum[s][b] + cnr_n[s][b] / 2u) / cnr_n[s][b]);
            }
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(want, v.sys[s].cnr_dbhz[b], msg);
        }
    }
    snprintf(msg, sizeof(msg), "%s: epochs", name);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(want_groups, v.epochs, msg);

    /* Live list: every satellite of an MSM epoch within the window, with the
     * signals and the best C/N0 per band of its latest epoch; and the totals. */
    static obs_live_sat_t live[OBS_LIVE_MAX];
    obs_live_totals_t tot;
    size_t nl = obs_core_live(&s_core, now, live, OBS_LIVE_MAX, &tot);
    size_t k = 0;
    unsigned tracked = 0;
    unsigned signals = 0;
    uint32_t band_sum[GNSS_BAND_COUNT] = { 0 };
    uint32_t band_n[GNSS_BAND_COUNT] = { 0 };
    uint16_t band_sats[GNSS_BAND_COUNT] = { 0 };
    uint32_t all_sum = 0;
    uint32_t all_n = 0;
    for (unsigned s = 0; s < GNSS_SYS_COUNT; s++) {
        unsigned sys_sats = 0;
        for (unsigned i = 0; i < 64; i++) {
            const want_sat_t *w = &sat[s][i];
            if (!w->valid || now - w->t >= OBS_SIGNAL_WINDOW_MS) {
                continue;
            }
            snprintf(msg, sizeof(msg), "%s: %s %u", name, fx_sys_name(s), i + 1);
            TEST_ASSERT_LESS_THAN_size_t_MESSAGE(nl, k, msg);
            const obs_live_sat_t *l = &live[k++];
            TEST_ASSERT_EQUAL_MESSAGE(s, l->sys, msg);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(i + 1, l->sat, msg);
            TEST_ASSERT_TRUE_MESSAGE(l->in_msm, msg);
            TEST_ASSERT_EQUAL_HEX32_MESSAGE(w->sig_mask, l->sig_mask, msg);
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(w->band_mask, l->band_mask, msg);
            TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(w->cnr, l->cnr_dbhz, GNSS_BAND_COUNT, msg);
            TEST_ASSERT_FALSE_MESSAGE(l->has_pos, msg);
            tracked++;
            sys_sats++;
            signals += popcount(w->sig_mask);
            for (unsigned b = 0; b < GNSS_BAND_COUNT; b++) {
                if (!(w->band_mask & (1u << b))) {
                    continue;
                }
                band_sats[b]++;
                if (w->cnr[b] != 0) {
                    band_sum[b] += w->cnr[b];
                    band_n[b]++;
                    all_sum += w->cnr[b];
                    all_n++;
                }
            }
        }
        snprintf(msg, sizeof(msg), "%s: %s satellites", name, fx_sys_name(s));
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(sys_sats, tot.sys_sats[s], msg);
    }
    snprintf(msg, sizeof(msg), "%s: live totals", name);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(k, nl, msg);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(tracked, tot.tracked, msg);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(tracked, tot.satellites, msg);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(signals, tot.signals, msg);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, tot.dropped, msg);
    for (unsigned b = 0; b < GNSS_BAND_COUNT; b++) {
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(band_sats[b], tot.band_sats[b], msg);
        uint8_t want = band_n[b] ? (uint8_t)((band_sum[b] + band_n[b] / 2u) / band_n[b]) : 0;
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(want, tot.band_cnr_dbhz[b], msg);
    }
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(all_n ? (uint8_t)((all_sum + all_n / 2u) / all_n) : 0, tot.cnr_dbhz, msg);
    free_recs(recs, n);
}

TEST_CASE("fixtures: observation summary and live list over a native MSM7 stream", "[fixtures]")
{
    check_stream("centipede_LAX_30ep.rtcm3", 30);
}

TEST_CASE("fixtures: observation summary with a signal announced without cells and a lost closing MSM", "[fixtures]")
{
    /* BeiDou signal 14 is in every 1127's signal mask with no cell: it is not
     * a signal the receiver delivers. One epoch never gets its closing MSM:
     * the next epoch time ends it, and it is still counted. */
    check_stream("centipede_ROBU1_35ep.rtcm3", 35);
}

TEST_CASE("fixtures: observation summary over MSM4, a split constellation and other encoders", "[fixtures]")
{
    check_stream("centipede_BGDD_30ep.rtcm3", 30);
    check_stream("rtklib_GMSD7_12ep.rtcm3", 12);
    check_stream("pyrtcm_NTRIP-USCL00CHL0.log", 1);
}

TEST_CASE("fixtures: message statistics count a split type once per epoch", "[fixtures]")
{
    static rec_t recs[600];
    size_t n = 0;
    uint64_t t_end;
    feed_stream("pygnssutils_ntrip_chunked.bin", recs, &n, &t_end);
    /* Frames and distinct epoch times of 1127 in the oracle. */
    unsigned frames = 0;
    unsigned epochs = 0;
    uint32_t last = 0;
    for (size_t i = 0; i < n; i++) {
        if (num(recs[i].rec, "type") != 1127) {
            continue;
        }
        frames++;
        if (epochs == 0 || recs[i].epoch != last) {
            epochs++;
        }
        last = recs[i].epoch;
    }
    TEST_ASSERT_GREATER_THAN(epochs, frames);
    obs_msg_stat_t st[OBS_MSG_MAX];
    size_t ns = obs_core_msgs(&s_core, t_end, st, OBS_MSG_MAX);
    bool found = false;
    for (size_t i = 0; i < ns; i++) {
        if (st[i].msg == 1127) {
            found = true;
            TEST_ASSERT_EQUAL_UINT64(frames, st[i].frames);
            TEST_ASSERT_EQUAL_UINT64(epochs, st[i].count);
            TEST_ASSERT_EQUAL_UINT32(1000, st[i].period_ms);
        }
    }
    TEST_ASSERT_TRUE(found);
    free_recs(recs, n);
}

/* ---- satellite-in-view sentences ------------------------------------------ */

/* NMEA 4.11 satellite numbering per talker -> MSM numbering (expected values). */
static bool want_map(const char *talker, int n, int *sys, int *sat)
{
    if (strcmp(talker, "GP") == 0 || strcmp(talker, "GN") == 0) {
        if (n >= 1 && n <= 32) {
            *sys = GNSS_SYS_GPS, *sat = n;
        } else if (n >= 33 && n <= 64) {
            *sys = GNSS_SYS_SBAS, *sat = n - 32;   /* PRN 120.. = MSM satellite 1.. */
        } else if (n >= 65 && n <= 96) {
            *sys = GNSS_SYS_GLONASS, *sat = n - 64;
        } else if (n >= 193 && n <= 202) {
            *sys = GNSS_SYS_QZSS, *sat = n - 192;
        } else {
            return false;
        }
        return true;
    }
    if (strcmp(talker, "GL") == 0) {
        *sys = GNSS_SYS_GLONASS, *sat = (n >= 65 && n <= 96) ? n - 64 : n;
        return *sat >= 1 && *sat <= 32;
    }
    if (strcmp(talker, "GA") == 0) {
        *sys = GNSS_SYS_GALILEO, *sat = n;
        return n >= 1 && n <= 64;
    }
    if (strcmp(talker, "GB") == 0) {
        *sys = GNSS_SYS_BEIDOU, *sat = n;
        return n >= 1 && n <= 63;
    }
    if (strcmp(talker, "GQ") == 0) {
        *sys = GNSS_SYS_QZSS, *sat = (n >= 193 && n <= 202) ? n - 192 : n;
        return *sat >= 1 && *sat <= 10;
    }
    return false;
}

TEST_CASE("fixtures: every satellite-in-view sentence of a real Unicore capture parses like pynmeagps", "[fixtures]")
{
    size_t len;
    uint8_t *buf = fx_read("nmea/gpsd_unicore-um621_excerpt.log", &len);
    fx_units_t u;
    fx_frame(buf, len, 0, &u);
    TEST_ASSERT_EQUAL_size_t((size_t)num(fx_entry("gpsd_unicore-um621_excerpt.log"), "text_lines"), u.n);
    fx_jsonl_t j;
    TEST_ASSERT_TRUE(fx_jsonl_load("oracle/gpsd_unicore-um621_excerpt.log.gsv.jsonl", &j));

    obs_core_init(&s_core);
    /* Expected sky: per satellite the last reported position and the best C/N0. */
    static struct {
        bool pos;
        int el, az;
        int cnr;
        bool any;
    } sky[GNSS_SYS_COUNT][64];
    memset(sky, 0, sizeof(sky));
    unsigned signals_seen[GNSS_SYS_COUNT] = { 0 };
    size_t r = 0;
    for (size_t i = 0; i < u.n; i++) {
        const char *line = (const char *)u.u[i].data;
        obs_gsv_t g;
        bool gsv = obs_gsv_parse(line, u.u[i].len, &g);
        bool is_gsv = u.u[i].len > 7 && memcmp(line + 3, "GSV,", 4) == 0;
        TEST_ASSERT_EQUAL(is_gsv, gsv);
        if (!gsv) {
            continue;
        }
        TEST_ASSERT_LESS_THAN_size_t(j.n, r);
        const cJSON *rec = j.rec[r++];
        char msg[120];
        snprintf(msg, sizeof(msg), "line %ld: %.60s", num(rec, "n"), line);
        const char *talker = cJSON_GetObjectItemCaseSensitive(rec, "talker")->valuestring;
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(talker, line + 1, 2, msg);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(num(rec, "total"), g.total, msg);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(num(rec, "number"), g.number, msg);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(num(rec, "in_view"), g.in_view, msg);
        const cJSON *sid = cJSON_GetObjectItemCaseSensitive(rec, "signal_id");
        unsigned want_sid = cJSON_IsNull(sid) ? 0 : (unsigned)strtoul(sid->valuestring, NULL, 16);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(want_sid, g.signal_id, msg);
        unsigned k = 0;
        const cJSON *s;
        cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(rec, "sats"))
        {
            const cJSON *sv = cJSON_GetArrayItem(s, 0);
            const cJSON *el = cJSON_GetArrayItem(s, 1);
            const cJSON *az = cJSON_GetArrayItem(s, 2);
            const cJSON *cn = cJSON_GetArrayItem(s, 3);
            int sys;
            int sat;
            if (cJSON_IsNull(sv) || !want_map(talker, (int)sv->valuedouble, &sys, &sat)) {
                continue;
            }
            TEST_ASSERT_LESS_THAN_UINT_MESSAGE(g.count, k, msg);
            const obs_gsv_sat_t *o = &g.sat[k++];
            TEST_ASSERT_EQUAL_MESSAGE(sys, o->sys, msg);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(sat, o->sat, msg);
            bool pos = !cJSON_IsNull(el) && !cJSON_IsNull(az);
            TEST_ASSERT_EQUAL_MESSAGE(pos, o->has_pos, msg);
            if (pos) {
                TEST_ASSERT_EQUAL_INT8_MESSAGE((int)el->valuedouble, o->elevation_deg, msg);
                TEST_ASSERT_EQUAL_UINT16_MESSAGE((int)az->valuedouble, o->azimuth_deg, msg);
                sky[sys][sat - 1].pos = true;
                sky[sys][sat - 1].el = (int)el->valuedouble;
                sky[sys][sat - 1].az = (int)az->valuedouble;
            }
            int c = cJSON_IsNull(cn) ? 0 : (int)cn->valuedouble;
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(c, o->cnr_dbhz, msg);
            if (c > sky[sys][sat - 1].cnr) {
                sky[sys][sat - 1].cnr = c;
            }
            sky[sys][sat - 1].any = true;
            signals_seen[sys] |= 1u << (want_sid & 31u);
        }
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(k, g.count, msg);
        obs_core_feed_gsv(&s_core, &g, 5000);   /* one burst */
    }
    TEST_ASSERT_EQUAL_size_t(j.n, r);
    /* This capture: GPS on signals 1 and 8, GLONASS 1, Galileo 1 and 7,
     * BeiDou 1 and 5, plus SBAS under the GPS talker. */
    TEST_ASSERT_EQUAL_HEX32((1u << 1) | (1u << 8), signals_seen[GNSS_SYS_GPS]);
    TEST_ASSERT_EQUAL_HEX32(1u << 1, signals_seen[GNSS_SYS_GLONASS]);
    TEST_ASSERT_EQUAL_HEX32((1u << 1) | (1u << 7), signals_seen[GNSS_SYS_GALILEO]);
    TEST_ASSERT_EQUAL_HEX32((1u << 1) | (1u << 5), signals_seen[GNSS_SYS_BEIDOU]);
    TEST_ASSERT_NOT_EQUAL(0, signals_seen[GNSS_SYS_SBAS]);

    /* The sky view holds every satellite with a position, sorted. */
    static obs_sat_t view[OBS_SKY_MAX];
    size_t nv = obs_core_sky(&s_core, 6000, view, OBS_SKY_MAX);
    size_t k = 0;
    for (unsigned s = 0; s < GNSS_SYS_COUNT; s++) {
        for (unsigned i = 0; i < 64; i++) {
            if (!sky[s][i].pos) {
                continue;
            }
            TEST_ASSERT_LESS_THAN_size_t(nv, k);
            const obs_sat_t *o = &view[k++];
            TEST_ASSERT_EQUAL(s, o->sys);
            TEST_ASSERT_EQUAL_UINT8(i + 1, o->sat);
            TEST_ASSERT_EQUAL_INT8(sky[s][i].el, o->elevation_deg);
            TEST_ASSERT_EQUAL_UINT16(sky[s][i].az, o->azimuth_deg);
            TEST_ASSERT_EQUAL_UINT8(sky[s][i].cnr, o->cnr_dbhz);
        }
    }
    TEST_ASSERT_EQUAL_size_t(k, nv);
    TEST_ASSERT_GREATER_OR_EQUAL(30, nv);

    fx_jsonl_free(&j);
    fx_units_free(&u);
    free(buf);
}
