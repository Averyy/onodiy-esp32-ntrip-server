/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The fixed-position check's rule (um98x_fixcheck.h): the receiver's own
 * position logs as the command layer reads them, the solutions that count,
 * the distances, and the verdict over three solutions.
 *
 * SYNTHETIC: every log line here was built from the documented field layout
 * with made-up positions; the checksums are computed by the test. One test
 * reads a real receiver's binary logs (test/fixtures/unicore).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture_util.h"
#include "um98x_fixcheck.h"
#include "um98x_profile.h"
#include "um98x_reply.h"
#include "um98x_txn.h"
#include "unity.h"

/* The fixed position the tests check against (ellipsoidal height). */
#define FIX_LAT 45.4000000
#define FIX_LON -75.7000000
#define FIX_H   100.0
#define GEOID   -34.0   /* geoid height there: 34 m below the ellipsoid */

/* Metres per degree of latitude near 45 degrees: about 111 132 m. */
#define M_PER_DEG_LAT 111132.0

static uint32_t crc32_log(const char *s, size_t n)
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

/* "#<name>,<header>;<body>*crc32" */
static void make_log(char *out, size_t cap, const char *name, const char *body)
{
    char tmp[400];
    int n = snprintf(tmp, sizeof(tmp), "%s,COM1,0,0.0,FINE,2380,0.000,0,0,18;%s", name, body);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(tmp));
    snprintf(out, cap, "#%s*%08x", tmp, (unsigned)crc32_log(tmp, (size_t)n));
}

/* A body as the receiver lays it out: 15 fields we read, then the rest of the record. */
static void make_body(char *out, size_t cap, const char *status, const char *type, double lat, double lon, double msl,
                      double sep, double sd_lat, double sd_lon, double sd_hgt, int tracked, int used)
{
    snprintf(out, cap,
             "%s,%s,%.11f,%.11f,%.4f,%.4f,WGS84,%.4f,%.4f,%.4f,\"0\",0.000,0.000,%d,%d,0,0,0,00,00,00,"
             "SOL_COMPUTED,DOPPLER_VELOCITY,0.000,0.000,0.0040,12.500000,0.0050,0.0200,0.0100",
             status, type, lat, lon, msl, sep, sd_lat, sd_lon, sd_hgt, tracked, used);
}

/* A usable solution north/east of the fixed position and up_m above it (sep: the separation printed). */
static um98x_own_fix_t own_at(double north_m, double east_m, double up_m, double sep)
{
    um98x_own_fix_t f;
    memset(&f, 0, sizeof(f));
    f.computed = true;
    f.pos_type = UM98X_OWN_POS_SINGLE;
    f.lat_deg = FIX_LAT + north_m / M_PER_DEG_LAT;
    f.lon_deg = FIX_LON + east_m / (M_PER_DEG_LAT * cos(FIX_LAT * 3.14159265358979323846 / 180.0));
    f.msl_height_m = FIX_H + up_m - sep;   /* height above the geoid */
    f.separation_m = sep;
    f.sd_lat_m = 1.1;
    f.sd_lon_m = 0.9;
    f.sd_hgt_m = 2.2;
    f.sats_tracked = 40;
    f.sats_used = 30;
    return f;
}

static um98x_fix_offset_t offset_of(const um98x_own_fix_t *f)
{
    um98x_fix_offset_t o;
    TEST_ASSERT_TRUE(um98x_fix_offset(FIX_LAT, FIX_LON, FIX_H, f, &o));
    return o;
}

/* ---- the logs ----------------------------------------------------------------- */

TEST_CASE("fixcheck: a position log's body reads into a solution", "[um98x_fixcheck]")
{
    char body[300];
    make_body(body, sizeof(body), "SOL_COMPUTED", "SINGLE", 45.40012345678, -75.69987654321, 134.5678, -34.1234, 1.2345,
              1.1, 2.5, 41, 33);
    um98x_own_fix_t f;
    TEST_ASSERT_TRUE(um98x_own_fix_parse(body, strlen(body), &f));
    TEST_ASSERT_TRUE(f.computed);
    TEST_ASSERT_EQUAL_UINT8(UM98X_OWN_POS_SINGLE, f.pos_type);
    TEST_ASSERT_DOUBLE_WITHIN(1e-11, 45.40012345678, f.lat_deg);
    TEST_ASSERT_DOUBLE_WITHIN(1e-11, -75.69987654321, f.lon_deg);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 134.5678, f.msl_height_m);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, -34.1234, f.separation_m);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1.2345, f.sd_lat_m);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 2.5, f.sd_hgt_m);
    TEST_ASSERT_EQUAL_UINT8(41, f.sats_tracked);
    TEST_ASSERT_EQUAL_UINT8(33, f.sats_used);

    /* Only the first 15 fields are needed. */
    const char *short15 = "SOL_COMPUTED,PSRDIFF,1,2,3,4,WGS84,5,6,7,\"12\",0.5,1.0,20,18";
    TEST_ASSERT_TRUE(um98x_own_fix_parse(short15, strlen(short15), &f));
    TEST_ASSERT_EQUAL_UINT8(UM98X_OWN_POS_PSRDIFF, f.pos_type);
    TEST_ASSERT_EQUAL_UINT8(18, f.sats_used);

    /* Words that are not the computed status or a known type never fail the parse. */
    make_body(body, sizeof(body), "INSUFFICIENT_OBS", "NONE", 0, 0, 0, 0, 0, 0, 0, 3, 0);
    TEST_ASSERT_TRUE(um98x_own_fix_parse(body, strlen(body), &f));
    TEST_ASSERT_FALSE(f.computed);
    TEST_ASSERT_EQUAL_UINT8(UM98X_OWN_POS_NONE, f.pos_type);
    make_body(body, sizeof(body), "sol_computed", "fixedpos", 45, -75, 100, -34, 0, 0, 0, 30, 25);
    TEST_ASSERT_TRUE(um98x_own_fix_parse(body, strlen(body), &f));
    TEST_ASSERT_TRUE(f.computed);
    TEST_ASSERT_EQUAL_UINT8(UM98X_OWN_POS_FIXED, f.pos_type);
    make_body(body, sizeof(body), "SOL_COMPUTED", "NARROW_INT", 45, -75, 100, -34, 0, 0, 0, 30, 25);
    TEST_ASSERT_TRUE(um98x_own_fix_parse(body, strlen(body), &f));
    TEST_ASSERT_EQUAL_UINT8(UM98X_OWN_POS_OTHER, f.pos_type);
}

TEST_CASE("fixcheck: a body with a field missing or not a number is not read", "[um98x_fixcheck]")
{
    um98x_own_fix_t f;
    static const char *const bad[] = {
        "",
        "SOL_COMPUTED,SINGLE,45.4,-75.7,134.5,-34.1,WGS84,1.2,1.1,2.5,\"0\",0.000,0.000,41",   /* 14 fields */
        "SOL_COMPUTED,SINGLE,,-75.7,134.5,-34.1,WGS84,1.2,1.1,2.5,\"0\",0.000,0.000,41,33",   /* no latitude */
        "SOL_COMPUTED,SINGLE,45.4N,-75.7,134.5,-34.1,WGS84,1.2,1.1,2.5,\"0\",0.000,0.000,41,33",
        "SOL_COMPUTED,SINGLE,45.4,-75.7,134.5,-34.1,WGS84,1.2,1.1,nan,\"0\",0.000,0.000,41,33",
        "SOL_COMPUTED,SINGLE,45.4,-75.7,134.5,-34.1,WGS84,1.2,1.1,2.5,\"0\",0.000,0.000,41,3.5",   /* a count */
        "SOL_COMPUTED,SINGLE,45.4,-75.7,134.5,-34.1,WGS84,1.2,1.1,2.5,\"0\",0.000,0.000,41,300",
        "SOL_COMPUTED,SINGLE,45.4,-75.7,134.5,-34.1,WGS84,1.2,1.1,2.5,\"0\",0.000,0.000,41,-1",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        f.computed = true;
        TEST_ASSERT_FALSE_MESSAGE(um98x_own_fix_parse(bad[i], strlen(bad[i]), &f), bad[i]);
        TEST_ASSERT_FALSE(f.computed);   /* cleared */
    }
    TEST_ASSERT_FALSE(um98x_own_fix_parse(NULL, 10, &f));
    TEST_ASSERT_FALSE(um98x_own_fix_parse("x", 1, NULL));
}

TEST_CASE("fixcheck: the command layer reads both position logs, either checksum", "[um98x_fixcheck]")
{
    char body[300];
    char line[512];
    make_body(body, sizeof(body), "SOL_COMPUTED", "SINGLE", 45.4, -75.7, 134.0, -34.0, 1, 1, 2, 40, 30);
    um98x_line_t l;

    make_log(line, sizeof(line), "BASEPOSA", body);
    um98x_line_classify(line, strlen(line), &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_OWNPOS, l.kind);
    TEST_ASSERT_EQUAL_UINT8(UM98X_OWN_LOG_BASEPOS, l.own_log);
    TEST_ASSERT_EQUAL_size_t(strlen(body), l.text_len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(l.text, body, l.text_len));

    make_log(line, sizeof(line), "SPPNAVA", body);
    um98x_line_classify(line, strlen(line), &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_OWNPOS, l.kind);
    TEST_ASSERT_EQUAL_UINT8(UM98X_OWN_LOG_SPPNAV, l.own_log);

    /* A damaged checksum is a bad line, not a solution. */
    line[strlen(line) - 1] = line[strlen(line) - 1] == '0' ? '1' : '0';
    um98x_line_classify(line, strlen(line), &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_BAD, l.kind);

    /* The 2-hex XOR including '#', as the base-mode log may carry it. */
    char tmp[400];
    int n = snprintf(tmp, sizeof(tmp), "#BASEPOSA,COM1,0,0.0,FINE,0,0.000,0,0,0;%s", body);
    snprintf(line, sizeof(line), "%s*%02X", tmp, um98x_xor8(tmp, (size_t)n));
    um98x_line_classify(line, strlen(line), &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_OWNPOS, l.kind);

    /* The best-solution log is not one of them. */
    make_log(line, sizeof(line), "BESTNAVA", body);
    um98x_line_classify(line, strlen(line), &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_OTHER, l.kind);
}

TEST_CASE("fixcheck: the position queries go out without a lease while streaming, nothing else new", "[um98x_fixcheck]")
{
    TEST_ASSERT_TRUE(um98x_stream_query_allowed("BASEPOSA"));
    TEST_ASSERT_TRUE(um98x_stream_query_allowed(" sppnava\r\n"));
    TEST_ASSERT_TRUE(um98x_cmd_allowed(UM98X_LINK_STREAM, false, UM98X_CMD_BASEPOS));
    TEST_ASSERT_TRUE(um98x_cmd_allowed(UM98X_LINK_STREAM, false, UM98X_CMD_SPPNAV));
    /* With a rate they would start a periodic log: never without a lease. */
    TEST_ASSERT_FALSE(um98x_stream_query_allowed("BASEPOSA 1"));
    TEST_ASSERT_FALSE(um98x_stream_query_allowed("SPPNAVA COM1 1"));
    TEST_ASSERT_FALSE(um98x_stream_query_allowed("BESTNAVA"));
    TEST_ASSERT_FALSE(um98x_cmd_allowed(UM98X_LINK_RAW, false, UM98X_CMD_BASEPOS));
}

/* ---- the query in flight ---------------------------------------------------------- */

static bool offer(um98x_txn_t *t, const char *line, int64_t now)
{
    um98x_line_t l;
    um98x_line_classify(line, strlen(line), &l);
    return um98x_txn_on_line(t, line, strlen(line), &l, now);
}

static void ack_line(char *out, size_t cap, const char *cmd, const char *reply)
{
    char tmp[200];
    int n = snprintf(tmp, sizeof(tmp), "$command,%s,response: %s", cmd, reply);
    snprintf(out, cap, "%s*%02X", tmp, um98x_xor8(tmp, (size_t)n));
}

TEST_CASE("fixcheck: a position query is done on its own log, once, and a refusal is final", "[um98x_fixcheck]")
{
    char body[300];
    char base[512];
    char spp[512];
    char ack[200];
    make_body(body, sizeof(body), "SOL_COMPUTED", "SINGLE", 45.4, -75.7, 134.0, -34.0, 1, 1, 2, 40, 30);
    make_log(base, sizeof(base), "BASEPOSA", body);
    make_log(spp, sizeof(spp), "SPPNAVA", body);

    um98x_txn_t t;
    int64_t wake;
    um98x_txn_begin(&t, UM98X_TXN_OWNPOS, UM98X_CMD_SPPNAV, 0, NULL, 0);
    TEST_ASSERT_EQUAL(UM98X_TXN_ACT_SEND, um98x_txn_poll(&t, 0, &wake));
    um98x_txn_sent(&t, 10, true);
    ack_line(ack, sizeof(ack), "SPPNAVA", "OK");
    TEST_ASSERT_TRUE(offer(&t, ack, 40));
    TEST_ASSERT_EQUAL(UM98X_TXN_PENDING, t.status);
    TEST_ASSERT_FALSE(offer(&t, base, 60));   /* the other log is not its answer */
    TEST_ASSERT_TRUE(offer(&t, spp, 80));
    TEST_ASSERT_EQUAL(UM98X_TXN_OK, t.status);
    TEST_ASSERT_EQUAL_STRING(body, t.own_body);

    /* Acknowledged and no log within UM98X_GROUPS_LOG_MS: done, not resent. */
    um98x_txn_begin(&t, UM98X_TXN_OWNPOS, UM98X_CMD_BASEPOS, 0, NULL, 0);
    TEST_ASSERT_EQUAL(UM98X_TXN_ACT_SEND, um98x_txn_poll(&t, 0, &wake));
    um98x_txn_sent(&t, 10, true);
    ack_line(ack, sizeof(ack), "BASEPOSA", "OK");
    TEST_ASSERT_TRUE(offer(&t, ack, 40));
    TEST_ASSERT_EQUAL(UM98X_TXN_ACT_WAIT, um98x_txn_poll(&t, 500, &wake));
    TEST_ASSERT_EQUAL(UM98X_TXN_ACT_WAIT, um98x_txn_poll(&t, 40 + UM98X_GROUPS_LOG_MS - 1, &wake));
    TEST_ASSERT_EQUAL(UM98X_TXN_ACT_DONE, um98x_txn_poll(&t, 40 + UM98X_GROUPS_LOG_MS, &wake));
    TEST_ASSERT_EQUAL(UM98X_TXN_TIMEOUT, t.status);
    TEST_ASSERT_EQUAL_UINT8(1, t.sends);

    /* Its log 1.5 s after the acknowledgement: still its answer. */
    um98x_txn_begin(&t, UM98X_TXN_OWNPOS, UM98X_CMD_BASEPOS, 0, NULL, 0);
    TEST_ASSERT_EQUAL(UM98X_TXN_ACT_SEND, um98x_txn_poll(&t, 0, &wake));
    um98x_txn_sent(&t, 10, true);
    TEST_ASSERT_TRUE(offer(&t, ack, 40));
    TEST_ASSERT_EQUAL(UM98X_TXN_ACT_WAIT, um98x_txn_poll(&t, 1540, &wake));
    TEST_ASSERT_TRUE(offer(&t, base, 1540));
    TEST_ASSERT_EQUAL(UM98X_TXN_OK, t.status);

    /* A firmware without the log refuses it: final at once. */
    um98x_txn_begin(&t, UM98X_TXN_OWNPOS, UM98X_CMD_BASEPOS, 0, NULL, 0);
    TEST_ASSERT_EQUAL(UM98X_TXN_ACT_SEND, um98x_txn_poll(&t, 0, &wake));
    um98x_txn_sent(&t, 10, true);
    ack_line(ack, sizeof(ack), "BASEPOSA", "PARSING FAILED NO MATCHING FUNC  BASEPOSA");
    TEST_ASSERT_TRUE(offer(&t, ack, 40));
    TEST_ASSERT_EQUAL(UM98X_TXN_REJECTED, t.status);
    TEST_ASSERT_EQUAL_UINT8(1, t.sends);

    /* No answer at all: sent twice, then a timeout. */
    um98x_txn_begin(&t, UM98X_TXN_OWNPOS, UM98X_CMD_BASEPOS, 0, NULL, 0);
    int64_t now = 0;
    for (int i = 0; i < 20; i++) {
        um98x_txn_action_t act = um98x_txn_poll(&t, now, &wake);
        if (act == UM98X_TXN_ACT_DONE) {
            break;
        }
        if (act == UM98X_TXN_ACT_SEND) {
            um98x_txn_sent(&t, now, true);
        } else {
            now = wake;
        }
    }
    TEST_ASSERT_EQUAL(UM98X_TXN_TIMEOUT, t.status);
    TEST_ASSERT_EQUAL_UINT8(2, t.sends);
}

/* ---- the solutions that count ------------------------------------------------------- */

TEST_CASE("fixcheck: only a computed single-point solution with enough satellites and small deviations counts",
          "[um98x_fixcheck]")
{
    um98x_own_fix_t good = own_at(0, 0, 0, GEOID);
    TEST_ASSERT_TRUE(um98x_own_fix_usable(&good));

    um98x_own_fix_t f = good;
    f.computed = false;
    TEST_ASSERT_FALSE(um98x_own_fix_usable(&f));
    f = good;
    f.pos_type = UM98X_OWN_POS_FIXED;   /* an echo of the set position */
    TEST_ASSERT_FALSE(um98x_own_fix_usable(&f));
    f.pos_type = UM98X_OWN_POS_NONE;
    TEST_ASSERT_FALSE(um98x_own_fix_usable(&f));
    f.pos_type = UM98X_OWN_POS_OTHER;
    TEST_ASSERT_FALSE(um98x_own_fix_usable(&f));
    f.pos_type = UM98X_OWN_POS_SBAS;
    TEST_ASSERT_TRUE(um98x_own_fix_usable(&f));
    f.pos_type = UM98X_OWN_POS_PSRDIFF;
    TEST_ASSERT_TRUE(um98x_own_fix_usable(&f));

    f = good;
    f.sats_used = UM98X_FIX_MIN_SATS - 1;
    TEST_ASSERT_FALSE(um98x_own_fix_usable(&f));
    f.sats_used = UM98X_FIX_MIN_SATS;
    TEST_ASSERT_TRUE(um98x_own_fix_usable(&f));

    f = good;
    f.sd_lat_m = 4.0;
    f.sd_lon_m = 3.0;   /* 5.0 together: at the cap */
    TEST_ASSERT_TRUE(um98x_own_fix_usable(&f));
    f.sd_lon_m = 3.1;
    TEST_ASSERT_FALSE(um98x_own_fix_usable(&f));
    f = good;
    f.sd_hgt_m = UM98X_FIX_MAX_SD_V_M + 0.01;
    TEST_ASSERT_FALSE(um98x_own_fix_usable(&f));
    f.sd_hgt_m = -1.0;
    TEST_ASSERT_FALSE(um98x_own_fix_usable(&f));

    f = good;
    f.lat_deg = NAN;
    TEST_ASSERT_FALSE(um98x_own_fix_usable(&f));
    f = good;
    f.lon_deg = 181.0;
    TEST_ASSERT_FALSE(um98x_own_fix_usable(&f));
    f = good;
    f.separation_m = 160.0;   /* no geoid is that far from the ellipsoid */
    TEST_ASSERT_FALSE(um98x_own_fix_usable(&f));
    f = good;
    f.msl_height_m = 20000.0;
    TEST_ASSERT_FALSE(um98x_own_fix_usable(&f));
    TEST_ASSERT_FALSE(um98x_own_fix_usable(NULL));

    /* An unusable solution gives no distance at all. */
    um98x_fix_offset_t o;
    f = good;
    f.computed = false;
    TEST_ASSERT_FALSE(um98x_fix_offset(FIX_LAT, FIX_LON, FIX_H, &f, &o));
    TEST_ASSERT_FALSE(um98x_fix_offset(NAN, FIX_LON, FIX_H, &good, &o));
    TEST_ASSERT_FALSE(um98x_fix_offset(FIX_LAT, FIX_LON, 20000.0, &good, &o));
}

/* ---- the distances ------------------------------------------------------------------ */

TEST_CASE("fixcheck: horizontal and vertical distances from the receiver's own solution", "[um98x_fixcheck]")
{
    /* Where the fixed position says: about nothing. */
    um98x_own_fix_t f = own_at(0, 0, 0, GEOID);
    um98x_fix_offset_t o = offset_of(&f);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 0.0, o.horizontal_m);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 0.0, o.up_m);

    /* The receiver 120 m north and 35 m east of it. */
    f = own_at(120.0, 35.0, 0, GEOID);
    o = offset_of(&f);
    TEST_ASSERT_DOUBLE_WITHIN(0.5, sqrt(120.0 * 120.0 + 35.0 * 35.0), o.horizontal_m);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 0.0, o.up_m);
    TEST_ASSERT_DOUBLE_WITHIN(0.5, o.horizontal_m, o.distance_m);
    TEST_ASSERT_TRUE(um98x_fix_beyond(&o, UM98X_FIX_LIMIT_M));
    TEST_ASSERT_FALSE(um98x_fix_height_only(&o));

    /* A slip in the third decimal of the latitude (0.0009 degrees): about 100 m. */
    f = own_at(-0.0009 * M_PER_DEG_LAT, 0, 0, GEOID);
    o = offset_of(&f);
    TEST_ASSERT_DOUBLE_WITHIN(1.0, 100.0, o.horizontal_m);

    /* Another site about 12 km away (the test's degrees-to-metres is approximate east-west). */
    f = own_at(8000.0, -9000.0, 15.0, GEOID);
    o = offset_of(&f);
    TEST_ASSERT_DOUBLE_WITHIN(60.0, 12042.0, o.horizontal_m);
    TEST_ASSERT_DOUBLE_WITHIN(0.1, -15.0, o.up_m);

    /* Inside the receiver's own accuracy (1.5 m / 2.5 m RMS, a few metres at worst): well within the limits. */
    f = own_at(4.0, -3.0, 6.0, GEOID);
    o = offset_of(&f);
    TEST_ASSERT_FALSE(um98x_fix_beyond(&o, UM98X_FIX_LIMIT_M));
    TEST_ASSERT_DOUBLE_WITHIN(0.1, -6.0, o.up_m);   /* the fixed position is 6 m below the receiver's */

    /* Height alone: the fixed height 80 m above the receiver's own. */
    f = own_at(0, 0, -80.0, GEOID);
    o = offset_of(&f);
    TEST_ASSERT_DOUBLE_WITHIN(0.1, 80.0, o.up_m);
    TEST_ASSERT_TRUE(um98x_fix_beyond(&o, UM98X_FIX_LIMIT_M));
    TEST_ASSERT_TRUE(um98x_fix_height_only(&o));
}

TEST_CASE("fixcheck: the ellipsoidal height is the height above sea level plus the separation", "[um98x_fixcheck]")
{
    /* The receiver where the fixed position says. */
    um98x_own_fix_t f = own_at(0, 0, 0, GEOID);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 0.0, offset_of(&f).up_m);

    /* The fixed height entered as height above sea level: off by the separation. 34 m stays inside the limit... */
    double msl = FIX_H - GEOID;
    um98x_fix_offset_t o;
    TEST_ASSERT_TRUE(um98x_fix_offset(FIX_LAT, FIX_LON, msl, &f, &o));
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 34.0, o.up_m);
    TEST_ASSERT_FALSE(um98x_fix_beyond(&o, UM98X_FIX_LIMIT_M));

    /* ... where the geoid is 62 m above the ellipsoid, the same mistake is beyond it, by the height alone. */
    um98x_own_fix_t g = f;
    g.separation_m = 62.0;
    g.msl_height_m = FIX_H - 62.0;
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 0.0, offset_of(&g).up_m);
    TEST_ASSERT_TRUE(um98x_fix_offset(FIX_LAT, FIX_LON, FIX_H - 62.0, &g, &o));
    TEST_ASSERT_DOUBLE_WITHIN(0.01, -62.0, o.up_m);   /* the fixed height is below the receiver's */
    TEST_ASSERT_TRUE(um98x_fix_height_only(&o));
}

/* Little-endian fields of a binary log body. */
static double le_f64(const uint8_t *p)
{
    double v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static float le_f32(const uint8_t *p)
{
    float v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t le_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The body of the first binary log with this message ID whose CRC-32 (over sync, header and body) holds. */
static const uint8_t *bin_body(const uint8_t *d, size_t len, uint16_t id, size_t *body_len)
{
    for (size_t i = 0; i + 24 <= len; i++) {
        if (d[i] != 0xAA || d[i + 1] != 0x44 || d[i + 2] != 0xB5) {
            continue;
        }
        uint16_t mid = (uint16_t)(d[i + 4] | (d[i + 5] << 8));
        size_t n = (size_t)(d[i + 6] | (d[i + 7] << 8));
        if (mid != id || i + 24 + n + 4 > len) {
            continue;
        }
        if (crc32_log((const char *)d + i, 24 + n) != le_u32(d + i + 24 + n)) {
            continue;
        }
        *body_len = n;
        return d + i + 24;
    }
    return NULL;
}

TEST_CASE("fixcheck: a real UM981's single-point solution matches its ECEF solution with the sum", "[um98x_fixcheck]")
{
    size_t len;
    uint8_t *d = fx_read("unicore/pyunigps_um981.log", &len);
    size_t n_spp = 0;
    size_t n_xyz = 0;
    const uint8_t *spp = bin_body(d, len, 46, &n_spp);    /* SPPNAV */
    const uint8_t *xyz = bin_body(d, len, 240, &n_xyz);   /* BESTNAVXYZ, the same epoch */
    TEST_ASSERT_NOT_NULL(spp);
    TEST_ASSERT_NOT_NULL(xyz);
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(66, n_spp);
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(32, n_xyz);

    /* Its single-point solution as the ASCII log would print it (the same fields). */
    um98x_own_fix_t f;
    memset(&f, 0, sizeof(f));
    f.computed = le_u32(spp) == 0;                                    /* SOL_COMPUTED */
    f.pos_type = le_u32(spp + 4) == 16 ? UM98X_OWN_POS_SINGLE : UM98X_OWN_POS_OTHER;
    f.lat_deg = le_f64(spp + 8);
    f.lon_deg = le_f64(spp + 16);
    f.msl_height_m = le_f64(spp + 24);
    f.separation_m = le_f32(spp + 32);
    f.sd_lat_m = le_f32(spp + 40);
    f.sd_lon_m = le_f32(spp + 44);
    f.sd_hgt_m = le_f32(spp + 48);
    f.sats_tracked = spp[64];
    f.sats_used = spp[65];
    TEST_ASSERT_TRUE(um98x_own_fix_usable(&f));
    TEST_ASSERT_TRUE(fabs(f.separation_m) > 25.0);   /* large enough to tell the sign */

    double ecef[3] = { le_f64(xyz + 8), le_f64(xyz + 16), le_f64(xyz + 24) };
    double lat, lon, h;
    TEST_ASSERT_TRUE(um98x_ecef_to_llh(ecef, &lat, &lon, &h));
    TEST_ASSERT_DOUBLE_WITHIN(0.005, h, f.msl_height_m + f.separation_m);
    TEST_ASSERT_TRUE(fabs(h - (f.msl_height_m - f.separation_m)) > 50.0);

    /* That ECEF position as the fixed one: no distance at all. */
    um98x_fix_offset_t o;
    TEST_ASSERT_TRUE(um98x_fix_offset(lat, lon, h, &f, &o));
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 0.0, o.horizontal_m);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 0.0, o.up_m);
    free(d);
}

/* ---- the verdict -------------------------------------------------------------------- */

static um98x_fix_offset_t off(double h, double up)
{
    um98x_fix_offset_t o = { .horizontal_m = h, .up_m = up, .distance_m = sqrt(h * h + up * up) };
    return o;
}

TEST_CASE("fixcheck: three solutions in a row decide, the medians are shown", "[um98x_fixcheck]")
{
    um98x_fixchk_t c;
    um98x_fixchk_init(&c);
    um98x_fix_offset_t a = off(118.0, 3.0), b = off(125.0, -2.0), d = off(121.0, 1.0);
    TEST_ASSERT_EQUAL_UINT8(UM98X_FIX_NONE, um98x_fixchk_feed(&c, &a));
    TEST_ASSERT_EQUAL_UINT8(UM98X_FIX_NONE, um98x_fixchk_feed(&c, &b));
    TEST_ASSERT_EQUAL_UINT8(UM98X_FIX_NONE, c.verdict);
    TEST_ASSERT_EQUAL_UINT8(UM98X_FIX_FAR, um98x_fixchk_feed(&c, &d));
    TEST_ASSERT_EQUAL_UINT8(UM98X_FIX_FAR, c.verdict);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 121.0, c.shown.horizontal_m);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.0, c.shown.up_m);

    /* Near from the start: within both limits. */
    um98x_fixchk_init(&c);
    for (int i = 0; i < 2; i++) {
        um98x_fix_offset_t n = off(2.0 + i, -3.0);
        TEST_ASSERT_EQUAL_UINT8(UM98X_FIX_NONE, um98x_fixchk_feed(&c, &n));
    }
    um98x_fix_offset_t n = off(49.0, 49.0);   /* inside, just */
    TEST_ASSERT_EQUAL_UINT8(UM98X_FIX_NEAR, um98x_fixchk_feed(&c, &n));
}

TEST_CASE("fixcheck: solutions that disagree decide nothing; one outlier does not raise it", "[um98x_fixcheck]")
{
    um98x_fixchk_t c;
    um98x_fixchk_init(&c);
    um98x_fix_offset_t near = off(3.0, 2.0), far = off(140.0, 0.0);
    TEST_ASSERT_EQUAL_UINT8(UM98X_FIX_NONE, um98x_fixchk_feed(&c, &near));
    TEST_ASSERT_EQUAL_UINT8(UM98X_FIX_NONE, um98x_fixchk_feed(&c, &far));
    TEST_ASSERT_EQUAL_UINT8(UM98X_FIX_NONE, um98x_fixchk_feed(&c, &near));
    TEST_ASSERT_EQUAL_UINT8(UM98X_FIX_NONE, c.verdict);
    /* The run slides: the next two near ones and the one before them decide. */
    TEST_ASSERT_EQUAL_UINT8(UM98X_FIX_NONE, um98x_fixchk_feed(&c, &near));
    TEST_ASSERT_EQUAL_UINT8(UM98X_FIX_NEAR, um98x_fixchk_feed(&c, &near));
}

TEST_CASE("fixcheck: a far verdict clears only within 40 m, and a new run keeps it until it decides", "[um98x_fixcheck]")
{
    um98x_fixchk_t c;
    um98x_fixchk_init(&c);
    um98x_fix_offset_t far = off(0.0, -75.0);   /* the height alone */
    for (int i = 0; i < 3; i++) {
        (void)um98x_fixchk_feed(&c, &far);
    }
    TEST_ASSERT_EQUAL_UINT8(UM98X_FIX_FAR, c.verdict);

    /* A new run: the verdict and its distances stay meanwhile. */
    um98x_fixchk_restart(&c);
    TEST_ASSERT_EQUAL_UINT8(UM98X_FIX_FAR, c.verdict);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, -75.0, c.shown.up_m);
    TEST_ASSERT_EQUAL_UINT8(0, c.n);

    /* 45 m: inside the limit, but not inside the clear limit: no decision, still far. */
    um98x_fix_offset_t mid = off(45.0, 0.0);
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_UINT8(UM98X_FIX_NONE, um98x_fixchk_feed(&c, &mid));
    }
    TEST_ASSERT_EQUAL_UINT8(UM98X_FIX_FAR, c.verdict);

    /* Fixed: three solutions within 40 m clear it. */
    um98x_fix_offset_t ok = off(2.0, 1.0);
    TEST_ASSERT_EQUAL_UINT8(UM98X_FIX_NONE, um98x_fixchk_feed(&c, &ok));
    TEST_ASSERT_EQUAL_UINT8(UM98X_FIX_NONE, um98x_fixchk_feed(&c, &ok));
    TEST_ASSERT_EQUAL_UINT8(UM98X_FIX_NEAR, um98x_fixchk_feed(&c, &ok));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 2.0, c.shown.horizontal_m);

    /* From near, 45 m is near again (the clear limit only applies once far). */
    for (int i = 0; i < 3; i++) {
        (void)um98x_fixchk_feed(&c, &mid);
    }
    TEST_ASSERT_EQUAL_UINT8(UM98X_FIX_NEAR, c.verdict);
}
