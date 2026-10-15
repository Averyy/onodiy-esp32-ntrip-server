/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Framer tests: the published RTCM3 vectors (CRC-24Q check value, a 1005 and
 * a 1074 frame), garbage, flipped bytes, truncated frames, text interleaved
 * with RTCM, both '$' and both '#' checksum rules with a corrupted copy of
 * each, long lines, NUL noise, binary logs, a fuzzed stream with exact byte
 * accounting, and the loader prompt detector.
 *
 * Text vectors: the '$' acknowledgement, configuration and banner lines are
 * public examples of the receiver's replies; the failure lines and the
 * restarting answer are UM982 build 25164's own (our bench, 2026-10-14;
 * test/fixtures/unicore), with the older "FAILD" spelling of other builds
 * kept as well; the NMEA sentence is the textbook GGA example. The '#' mode and version lines are constructed
 * here in the documented layout (not captures from our receiver) and carry
 * checksums computed for this test.
 */
#include <string.h>

#include "gnss_framing.h"
#include "gnss_rtcm3.h"
#include "rtcm_vectors.h"
#include "unity.h"

/* ---- vectors (k_v1005, k_v1074: rtcm_vectors.h) ------------------------- */

#define L_ACK      "$command,unlog,response: OK*21"
#define L_ACK_VER  "$command,VERSIONA,response: OK*45"
#define L_CONFIG   "$CONFIG,SIGNALGROUP,CONFIG SIGNALGROUP 3 6*01"
#define L_FAILED   "$command,FOOBAR,response: PARSING FAILED NO MATCHING FUNC  FOOBAR*05"
#define L_SG_NO    "$command,CONFIG SIGNALGROUP 1,response: PARSING FAILED GRAMMAR ERROR,*60"
#define L_FAILD    "$command,rtcm1230 10,response: PARSING FAILD NO MATCHING FUNC  RTCM1230*61"
#define L_NOTICE   "The Board is resetting,do not response any command!*39"
#define L_BANNER   "$devicename,COM1*67"
#define L_NMEA     "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"
#define L_NMEA_IN  "$RESET*55"
#define L_MODE     "#MODEA,COM1,0,88.5,FINE,2338,412345.000,0,0,18;MODE BASE 1234 TIME 600 0*10"
#define L_VERSION  "#VERSIONA,79,GPS,FINE,2338,412345000,0,0,18,975;\"UM982\",\"R4.10Build18975\"," \
                   "\"HRPT00-S10C-P\",\"2310415000001-XY12345678901\",\"ffff48ffff0fffff\",\"2025/04/30\"*9ba66a41"

/* ---- collecting sink ---------------------------------------------------- */

#define MAX_UNITS 64

typedef struct {
    gnss_frame_kind_t kind;
    uint16_t type;
    size_t len;
    uint8_t data[GNSS_RTCM3_MAX_FRAME + 1];
} unit_t;

static unit_t s_units[MAX_UNITS];
static size_t s_count;

static void collect(const gnss_frame_t *fr, void *ctx)
{
    (void)ctx;
    TEST_ASSERT_LESS_THAN_size_t(MAX_UNITS, s_count);
    unit_t *u = &s_units[s_count++];
    u->kind = fr->kind;
    u->type = fr->rtcm_msg_type;
    u->len = fr->len;
    memcpy(u->data, fr->data, fr->len);
    if (fr->kind == GNSS_FRAME_TEXT) {
        TEST_ASSERT_EQUAL_UINT8(0, fr->data[fr->len]);   /* NUL-terminated */
    }
}

static gnss_framer_t s_framer;

static void start(void)
{
    memset(s_units, 0, sizeof(s_units));
    s_count = 0;
    memset(&s_framer, 0xA5, sizeof(s_framer));
    gnss_framer_init(&s_framer, collect, NULL);
}

static void feed(const void *data, size_t len)
{
    gnss_framer_feed(&s_framer, (const uint8_t *)data, len);
}

static void feed_str(const char *s)
{
    feed(s, strlen(s));
}

static gnss_framing_stats_t stats(void)
{
    gnss_framing_stats_t st;
    gnss_framer_get_stats(&s_framer, &st);
    return st;
}

static void expect_rtcm(size_t i, const uint8_t *frame, size_t len, uint16_t type)
{
    TEST_ASSERT_LESS_THAN_size_t(s_count, i);
    TEST_ASSERT_EQUAL(GNSS_FRAME_RTCM3, s_units[i].kind);
    TEST_ASSERT_EQUAL_UINT16(type, s_units[i].type);
    TEST_ASSERT_EQUAL_size_t(len, s_units[i].len);
    TEST_ASSERT_EQUAL_MEMORY(frame, s_units[i].data, len);
}

static void expect_line(size_t i, const char *line)
{
    TEST_ASSERT_LESS_THAN_size_t(s_count, i);
    TEST_ASSERT_EQUAL(GNSS_FRAME_TEXT, s_units[i].kind);
    TEST_ASSERT_EQUAL_UINT16(0, s_units[i].type);
    TEST_ASSERT_EQUAL_size_t(strlen(line), s_units[i].len);
    TEST_ASSERT_EQUAL_MEMORY(line, s_units[i].data, strlen(line));
}

#define make_frame rtcm_make_frame

/* ---- CRC and frame helpers ---------------------------------------------- */

TEST_CASE("maximum RTCM3 frame size follows the standard", "[gnss_framing]")
{
    TEST_ASSERT_EQUAL_UINT32(1029, GNSS_RTCM3_MAX_FRAME);
}

TEST_CASE("CRC-24Q matches the check value and the published frames", "[gnss_framing]")
{
    TEST_ASSERT_EQUAL_HEX32(0xCDE703, gnss_rtcm3_crc24q((const uint8_t *)"123456789", 9));
    TEST_ASSERT_EQUAL_HEX32(0x000000, gnss_rtcm3_crc24q(NULL, 0));

    TEST_ASSERT_EQUAL_HEX32(0x360B98, gnss_rtcm3_crc24q(k_v1005, sizeof(k_v1005) - 3));
    TEST_ASSERT_EQUAL_HEX32(0, gnss_rtcm3_crc24q(k_v1005, sizeof(k_v1005)));
    TEST_ASSERT_EQUAL_HEX32(0x71CE42, gnss_rtcm3_crc24q(k_v1074, sizeof(k_v1074) - 3));
    TEST_ASSERT_EQUAL_HEX32(0, gnss_rtcm3_crc24q(k_v1074, sizeof(k_v1074)));

    TEST_ASSERT_TRUE(gnss_rtcm3_frame_ok(k_v1005, sizeof(k_v1005)));
    TEST_ASSERT_TRUE(gnss_rtcm3_frame_ok(k_v1074, sizeof(k_v1074)));
    TEST_ASSERT_FALSE(gnss_rtcm3_frame_ok(k_v1005, sizeof(k_v1005) - 1));   /* length mismatch */

    uint8_t f[sizeof(k_v1074)];
    for (size_t i = 3; i < sizeof(f); i += 7) {                             /* any flipped bit fails */
        memcpy(f, k_v1074, sizeof(f));
        f[i] ^= 0x10;
        TEST_ASSERT_FALSE(gnss_rtcm3_frame_ok(f, sizeof(f)));
    }
    memcpy(f, k_v1005, sizeof(k_v1005));
    f[1] |= 0x04;                                                           /* a reserved bit */
    TEST_ASSERT_FALSE(gnss_rtcm3_frame_ok(f, sizeof(k_v1005)));
}

TEST_CASE("message number and station ID come from the payload", "[gnss_framing]")
{
    TEST_ASSERT_EQUAL_UINT16(1005, gnss_rtcm3_msg_type(k_v1005, sizeof(k_v1005)));
    TEST_ASSERT_EQUAL_UINT16(2003, gnss_rtcm3_station_id(k_v1005, sizeof(k_v1005)));
    TEST_ASSERT_EQUAL_UINT16(1074, gnss_rtcm3_msg_type(k_v1074, sizeof(k_v1074)));
    TEST_ASSERT_EQUAL_UINT16(0, gnss_rtcm3_station_id(k_v1074, sizeof(k_v1074)));
    TEST_ASSERT_EQUAL_UINT16(0, gnss_rtcm3_msg_type(k_v1005, 5));           /* truncated */
    TEST_ASSERT_EQUAL_UINT16(0, gnss_rtcm3_msg_type(NULL, 0));

    uint8_t f[16];
    const uint8_t one[] = { 0x3E };
    size_t n = make_frame(f, one, sizeof(one));
    TEST_ASSERT_EQUAL_UINT16(0, gnss_rtcm3_msg_type(f, n));                 /* 1-byte payload */
    TEST_ASSERT_FALSE(gnss_rtcm3_frame_ok(f, n));
}

/* ---- RTCM framing ------------------------------------------------------- */

TEST_CASE("framer init zeroes counters and reset keeps them", "[gnss_framing]")
{
    start();
    gnss_framing_stats_t st = stats();
    TEST_ASSERT_EQUAL_UINT64(0, st.bytes_in);
    TEST_ASSERT_EQUAL_UINT64(0, st.rtcm_frames);
    TEST_ASSERT_EQUAL_UINT64(0, st.text_lines);

    static const uint8_t junk[] = { 0x00, 0x01, 0x02, 0x03 };
    feed(junk, sizeof(junk));
    gnss_framer_reset(&s_framer);
    st = stats();
    TEST_ASSERT_EQUAL_UINT64(sizeof(junk), st.bytes_in);
    TEST_ASSERT_EQUAL_UINT64(sizeof(junk), st.bytes_discarded);
    TEST_ASSERT_EQUAL_size_t(0, s_count);
}

TEST_CASE("framer emits the 1005 and 1074 frames whole", "[gnss_framing]")
{
    start();
    feed(k_v1005, sizeof(k_v1005));
    feed(k_v1074, sizeof(k_v1074));
    feed(k_v1005, sizeof(k_v1005));
    TEST_ASSERT_EQUAL_size_t(3, s_count);
    expect_rtcm(0, k_v1005, sizeof(k_v1005), 1005);
    expect_rtcm(1, k_v1074, sizeof(k_v1074), 1074);
    expect_rtcm(2, k_v1005, sizeof(k_v1005), 1005);

    gnss_framing_stats_t st = stats();
    TEST_ASSERT_EQUAL_UINT64(2 * sizeof(k_v1005) + sizeof(k_v1074), st.bytes_in);
    TEST_ASSERT_EQUAL_UINT64(0, st.bytes_discarded);
    TEST_ASSERT_EQUAL_UINT64(3, st.rtcm_frames);
    TEST_ASSERT_EQUAL_UINT64(0, st.rtcm_crc_errors);
    TEST_ASSERT_EQUAL_UINT64(0, st.resyncs);
}

TEST_CASE("framer finds frames split at every byte boundary", "[gnss_framing]")
{
    uint8_t both[sizeof(k_v1005) + sizeof(k_v1074)];
    memcpy(both, k_v1005, sizeof(k_v1005));
    memcpy(both + sizeof(k_v1005), k_v1074, sizeof(k_v1074));
    for (size_t cut = 1; cut < sizeof(both); cut++) {
        start();
        feed(both, cut);
        feed(both + cut, sizeof(both) - cut);
        TEST_ASSERT_EQUAL_size_t(2, s_count);
        expect_rtcm(0, k_v1005, sizeof(k_v1005), 1005);
        expect_rtcm(1, k_v1074, sizeof(k_v1074), 1074);
    }
    start();
    for (size_t i = 0; i < sizeof(both); i++) {                    /* one byte per read */
        feed(both + i, 1);
    }
    TEST_ASSERT_EQUAL_size_t(2, s_count);
}

TEST_CASE("framer recovers both vectors from garbage, flipped bytes and truncated frames", "[gnss_framing]")
{
    static uint8_t stream[3072];
    size_t n = 0;
    size_t garbage = 0;
#define PUT(p, len) do { memcpy(stream + n, (p), (len)); n += (len); } while (0)
#define JUNK(p, len) do { PUT(p, len); garbage += (len); } while (0)

    static const uint8_t g1[] = { 0x00, 0xFF, 0xD3, 0x00, 0x40, 0x13, 0x37, 0xD3 };  /* false header swallowing what follows */
    JUNK(g1, sizeof(g1));
    PUT(k_v1005, sizeof(k_v1005));                                   /* expected 1 */
    JUNK(k_v1074, 60);                                               /* truncated 1074 */
    PUT(k_v1074, sizeof(k_v1074));                                   /* expected 2 */
    uint8_t bad[sizeof(k_v1005)];
    memcpy(bad, k_v1005, sizeof(bad));
    bad[10] ^= 0x01;                                                 /* flipped bit */
    JUNK(bad, sizeof(bad));
    static const uint8_t g2[] = { 0xD3, 0x03, 0xFF, 0x00, 0x00, 0xD3 };  /* 1023-byte false candidate */
    JUNK(g2, sizeof(g2));
    PUT(k_v1074, sizeof(k_v1074));                                   /* expected 3 */
    JUNK(k_v1005, 11);                                               /* truncated 1005 */
    PUT(k_v1005, sizeof(k_v1005));                                   /* expected 4 */
    uint8_t bad2[sizeof(k_v1074)];
    memcpy(bad2, k_v1074, sizeof(bad2));
    bad2[sizeof(bad2) - 1] ^= 0x80;                                  /* broken CRC */
    JUNK(bad2, sizeof(bad2));
    PUT(k_v1005, sizeof(k_v1005));                                   /* expected 5 */
    /* Trailing noise long enough for the longest false candidate to complete:
     * frames swallowed by one are only recovered once it fails its CRC. */
    static const uint8_t g3[GNSS_RTCM3_MAX_FRAME + 50] = { 0 };
    JUNK(g3, sizeof(g3));
#undef PUT
#undef JUNK

    for (size_t chunk = 1; chunk <= n; chunk = chunk * 3 + 1) {
        start();
        for (size_t off = 0; off < n; off += chunk) {
            feed(stream + off, (n - off < chunk) ? n - off : chunk);
        }
        gnss_framer_reset(&s_framer);
        TEST_ASSERT_EQUAL_size_t(5, s_count);
        expect_rtcm(0, k_v1005, sizeof(k_v1005), 1005);
        expect_rtcm(1, k_v1074, sizeof(k_v1074), 1074);
        expect_rtcm(2, k_v1074, sizeof(k_v1074), 1074);
        expect_rtcm(3, k_v1005, sizeof(k_v1005), 1005);
        expect_rtcm(4, k_v1005, sizeof(k_v1005), 1005);

        gnss_framing_stats_t st = stats();
        TEST_ASSERT_EQUAL_UINT64(n, st.bytes_in);
        TEST_ASSERT_EQUAL_UINT64(garbage, st.bytes_discarded);
        TEST_ASSERT_EQUAL_UINT64(5, st.rtcm_frames);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT64(4, st.rtcm_crc_errors);
        TEST_ASSERT_EQUAL_UINT64(5, st.resyncs);                     /* every frame followed junk */
    }
}

TEST_CASE("a frame inside a rejected candidate is found again", "[gnss_framing]")
{
    /* A false header announcing 80 bytes swallows the 1005 and part of the
     * 1074; when its CRC fails both are recovered from the held bytes. */
    static const uint8_t fake[] = { 0xD3, 0x00, 0x50 };
    start();
    feed(fake, sizeof(fake));
    feed(k_v1005, sizeof(k_v1005));
    TEST_ASSERT_EQUAL_size_t(0, s_count);
    feed(k_v1074, sizeof(k_v1074));
    TEST_ASSERT_EQUAL_size_t(2, s_count);
    expect_rtcm(0, k_v1005, sizeof(k_v1005), 1005);
    expect_rtcm(1, k_v1074, sizeof(k_v1074), 1074);
    gnss_framing_stats_t st = stats();
    TEST_ASSERT_EQUAL_UINT64(1, st.rtcm_crc_errors);
    TEST_ASSERT_EQUAL_UINT64(sizeof(fake), st.bytes_discarded);
    TEST_ASSERT_EQUAL_UINT64(1, st.resyncs);
}

TEST_CASE("reserved bits and short lengths reject a candidate at once", "[gnss_framing]")
{
    static const uint8_t reserved[] = { 0xD3, 0x04, 0x13 };
    static const uint8_t len0[] = { 0xD3, 0x00, 0x00 };
    static const uint8_t len1[] = { 0xD3, 0x00, 0x01 };
    start();
    feed(reserved, sizeof(reserved));
    feed(len0, sizeof(len0));
    feed(len1, sizeof(len1));
    feed(k_v1005, sizeof(k_v1005));
    TEST_ASSERT_EQUAL_size_t(1, s_count);
    expect_rtcm(0, k_v1005, sizeof(k_v1005), 1005);
    gnss_framing_stats_t st = stats();
    TEST_ASSERT_EQUAL_UINT64(1, st.rtcm_reserved_nonzero);
    TEST_ASSERT_EQUAL_UINT64(2, st.rtcm_bad_length);
    TEST_ASSERT_EQUAL_UINT64(0, st.rtcm_crc_errors);
    TEST_ASSERT_EQUAL_UINT64(9, st.bytes_discarded);
}

TEST_CASE("reset drops a partial frame and counts it", "[gnss_framing]")
{
    start();
    feed(k_v1074, 70);
    gnss_framer_reset(&s_framer);
    feed(k_v1074 + 70, sizeof(k_v1074) - 70);   /* the tail alone is junk now */
    feed(k_v1005, sizeof(k_v1005));
    TEST_ASSERT_EQUAL_size_t(1, s_count);
    expect_rtcm(0, k_v1005, sizeof(k_v1005), 1005);
    gnss_framing_stats_t st = stats();
    TEST_ASSERT_EQUAL_UINT64(sizeof(k_v1074), st.bytes_discarded);
    TEST_ASSERT_EQUAL_UINT64(1, st.resyncs);
}

/* ---- text lines --------------------------------------------------------- */

TEST_CASE("text lines interleave with RTCM frames", "[gnss_framing]")
{
    start();
    feed_str(L_ACK "\r\n");
    feed(k_v1005, sizeof(k_v1005));
    feed_str(L_NMEA "\r\n");
    feed(k_v1074, sizeof(k_v1074));
    feed_str(L_MODE "\r\n" L_ACK_VER "\r\n" L_VERSION "\r\n");
    feed(k_v1005, sizeof(k_v1005));
    feed_str(L_CONFIG "\r\n" L_FAILED "\r\n" L_BANNER "\r\n" L_NMEA_IN "\r\n");

    TEST_ASSERT_EQUAL_size_t(12, s_count);
    expect_line(0, L_ACK);
    expect_rtcm(1, k_v1005, sizeof(k_v1005), 1005);
    expect_line(2, L_NMEA);
    expect_rtcm(3, k_v1074, sizeof(k_v1074), 1074);
    expect_line(4, L_MODE);
    expect_line(5, L_ACK_VER);
    expect_line(6, L_VERSION);
    expect_rtcm(7, k_v1005, sizeof(k_v1005), 1005);
    expect_line(8, L_CONFIG);
    expect_line(9, L_FAILED);
    expect_line(10, L_BANNER);
    expect_line(11, L_NMEA_IN);

    gnss_framing_stats_t st = stats();
    TEST_ASSERT_EQUAL_UINT64(9, st.text_lines);
    TEST_ASSERT_EQUAL_UINT64(3, st.rtcm_frames);
    TEST_ASSERT_EQUAL_UINT64(0, st.bytes_discarded);
    TEST_ASSERT_EQUAL_UINT64(0, st.text_rejected);
    TEST_ASSERT_EQUAL_UINT64(0, st.resyncs);
}

TEST_CASE("both '$' rules and both '#' rules are accepted", "[gnss_framing]")
{
    static const char *const valid[] = {
        L_ACK, L_ACK_VER, L_CONFIG, L_FAILED, L_BANNER,  /* '$' included */
        L_SG_NO, L_FAILD,                                /* build 25164's refusal; the older spelling */
        L_NMEA, L_NMEA_IN,                               /* NMEA rule */
        L_MODE,                                          /* '#' XOR */
        L_VERSION,                                       /* '#' CRC32 */
        "#123456789*2dfd2d88",                           /* CRC32 check value: init 0, no final XOR */
        "#123456789*2DFD2D88",                           /* upper-case hex */
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47",
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        TEST_ASSERT_TRUE_MESSAGE(gnss_text_line_valid(valid[i], strlen(valid[i])), valid[i]);
    }
}

TEST_CASE("the receiver's restarting answer is a line without a lead character", "[gnss_framing]")
{
    /* As build 25164 sends it between other output, whole and byte by byte. */
    start();
    feed_str(L_ACK "\r\n" L_NOTICE "\r\n");
    feed(k_v1005, sizeof(k_v1005));
    feed_str(L_NOTICE "\r\n" L_BANNER "\r\n");
    TEST_ASSERT_EQUAL_size_t(5, s_count);
    expect_line(0, L_ACK);
    expect_line(1, L_NOTICE);
    expect_rtcm(2, k_v1005, sizeof(k_v1005), 1005);
    expect_line(3, L_NOTICE);
    expect_line(4, L_BANNER);
    gnss_framing_stats_t st = stats();
    TEST_ASSERT_EQUAL_UINT64(4, st.text_lines);
    TEST_ASSERT_EQUAL_UINT64(0, st.bytes_discarded);
    TEST_ASSERT_EQUAL_UINT64(0, st.text_rejected);

    start();
    static const char one[] = L_NOTICE "\n";
    for (size_t i = 0; i < sizeof(one) - 1; i++) {
        feed(one + i, 1);
    }
    TEST_ASSERT_EQUAL_size_t(1, s_count);
    expect_line(0, L_NOTICE);

    /* The other checksum forms do not apply to it. */
    TEST_ASSERT_FALSE(gnss_text_line_valid(L_NOTICE, strlen(L_NOTICE)));
}

TEST_CASE("a 'T' that does not start the restarting answer costs nothing behind it", "[gnss_framing]")
{
    start();
    feed_str("The Board is resetting,do not response any command!*38\r\n");   /* checksum off by one */
    feed_str("T" L_ACK "\r\n");                                               /* a lone 'T' before a line */
    feed_str("The Board is");                                                 /* cut short by an RTCM frame */
    feed(k_v1074, sizeof(k_v1074));
    feed_str("The Board was resetting\r\n");                                  /* not the prefix */
    feed_str("Th" L_NOTICE "\r\n");                                           /* a false start right before it */
    feed_str("The Board is resetting,do not response any command!" "\r\n");  /* no checksum */
    feed_str(L_NMEA "\r\n");

    TEST_ASSERT_EQUAL_size_t(4, s_count);
    expect_line(0, L_ACK);
    expect_rtcm(1, k_v1074, sizeof(k_v1074), 1074);
    expect_line(2, L_NOTICE);
    expect_line(3, L_NMEA);
    gnss_framing_stats_t st = stats();
    TEST_ASSERT_EQUAL_UINT64(3, st.text_lines);
    TEST_ASSERT_EQUAL_UINT64(2, st.text_rejected);   /* wrong checksum, no checksum */
    size_t kept = strlen(L_ACK "\r\n" L_NOTICE "\r\n" L_NMEA "\r\n") + sizeof(k_v1074);
    TEST_ASSERT_EQUAL_UINT64(st.bytes_in - kept, st.bytes_discarded);
}

TEST_CASE("corrupted lines are rejected and counted", "[gnss_framing]")
{
    static const char *const bad[] = {
        "$command,unlog,response: 0K*21",                /* one character changed */
        "$command,VERSIONA,response: OK*46",             /* checksum changed */
        "$CONFIG,SIGNALGROUP,CONFIG SIGNALGROUP 3 7*01",
        "$devicename,COM2*67",
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48",
        "#MODEA,COM1,0,88.5,FINE,2338,412345.000,0,0,18;MODE BASE 1234 TIME 601 0*10",
        "#MODEA,COM1,0,88.5,FINE,2338,412345.000,0,0,18;MODE BASE 1234 TIME 600 0*33",  /* XOR without '#' */
        "#VERSIONA,79,GPS,FINE,2338,412345000,0,0,18,975;\"UM982\",\"R4.10Build18976\","
        "\"HRPT00-S10C-P\",\"2310415000001-XY12345678901\",\"ffff48ffff0fffff\",\"2025/04/30\"*9ba66a41",
        "#123456789*2dfd2d89",
        "#123456789*cbf43926",                           /* the zlib CRC-32 (inverted) is not the rule */
        "$command,unlog,response: OK",                   /* no checksum */
        "$command,unlog,response: OK*2",                 /* one hex digit */
        "$command,unlog,response: OK*2G",
        "$*",
        "$",
        "XGPGGA*00",
    };
    start();
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(gnss_text_line_valid(bad[i], strlen(bad[i])), bad[i]);
        feed_str(bad[i]);
        feed_str("\r\n");
    }
    feed_str(L_ACK "\r\n");
    TEST_ASSERT_EQUAL_size_t(1, s_count);
    expect_line(0, L_ACK);
    gnss_framing_stats_t st = stats();
    TEST_ASSERT_EQUAL_UINT64(1, st.text_lines);
    TEST_ASSERT_EQUAL_UINT64(sizeof(bad) / sizeof(bad[0]) - 1, st.text_rejected);  /* "XGPGGA" never starts a line */
    TEST_ASSERT_EQUAL_UINT64(1, st.resyncs);
    TEST_ASSERT_EQUAL_UINT64(st.bytes_in - strlen(L_ACK "\r\n"), st.bytes_discarded);
}

TEST_CASE("RTCM payload bytes never produce a fake reply line", "[gnss_framing]")
{
    static const char text[] = "\r\n" L_ACK "\r\n" L_VERSION "\r\n";
    uint8_t f[GNSS_RTCM3_MAX_FRAME];
    size_t n = make_frame(f, (const uint8_t *)text, sizeof(text) - 1);
    start();
    feed(f, n);
    feed(k_v1005, sizeof(k_v1005));
    TEST_ASSERT_EQUAL_size_t(2, s_count);
    TEST_ASSERT_EQUAL(GNSS_FRAME_RTCM3, s_units[0].kind);
    TEST_ASSERT_EQUAL_size_t(n, s_units[0].len);
    TEST_ASSERT_EQUAL_UINT64(0, stats().text_lines);
}

TEST_CASE("line terminators: CR LF, LF and CR", "[gnss_framing]")
{
    start();
    feed_str(L_ACK "\r\n" L_ACK "\n" L_ACK "\r" L_ACK "\r\n");
    TEST_ASSERT_EQUAL_size_t(4, s_count);
    TEST_ASSERT_EQUAL_UINT64(0, stats().bytes_discarded);

    feed_str("\r\n\n");                                 /* blank lines are noise */
    TEST_ASSERT_EQUAL_UINT64(3, stats().bytes_discarded);
    feed_str("$bad*00\r\n");                            /* a dropped line takes its CR LF along */
    TEST_ASSERT_EQUAL_UINT64(3 + 9, stats().bytes_discarded);
}

TEST_CASE("long lines are skipped without losing what follows", "[gnss_framing]")
{
    static char line[GNSS_TEXT_MAX_LINE + 2];
    start();

    /* Exactly the maximum length, valid: emitted. */
    memset(line, 'A', sizeof(line));
    line[0] = '$';
    line[GNSS_TEXT_MAX_LINE - 3] = '*';
    uint8_t x = 0;
    for (size_t i = 1; i < GNSS_TEXT_MAX_LINE - 3; i++) {
        x ^= (uint8_t)line[i];
    }
    static const char hex[] = "0123456789ABCDEF";
    line[GNSS_TEXT_MAX_LINE - 2] = hex[x >> 4];
    line[GNSS_TEXT_MAX_LINE - 1] = hex[x & 15];
    feed(line, GNSS_TEXT_MAX_LINE);
    feed_str("\r\n");
    TEST_ASSERT_EQUAL_size_t(1, s_count);
    TEST_ASSERT_EQUAL_size_t(GNSS_TEXT_MAX_LINE, s_units[0].len);

    /* One byte longer: overflow, skipped to its terminator; the next line is fine. */
    memset(line, 'B', sizeof(line));
    line[0] = '$';
    feed(line, GNSS_TEXT_MAX_LINE + 1);
    feed_str("BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\r\n" L_ACK "\r\n");
    TEST_ASSERT_EQUAL_size_t(2, s_count);
    expect_line(1, L_ACK);
    TEST_ASSERT_EQUAL_UINT64(1, stats().text_overflows);

    /* An overlong line cut by an RTCM frame: the frame survives. */
    feed(line, GNSS_TEXT_MAX_LINE + 1);
    feed(line, 100);
    feed(k_v1005, sizeof(k_v1005));
    TEST_ASSERT_EQUAL_size_t(3, s_count);
    expect_rtcm(2, k_v1005, sizeof(k_v1005), 1005);
    gnss_framing_stats_t st = stats();
    TEST_ASSERT_EQUAL_UINT64(2, st.text_overflows);
    TEST_ASSERT_EQUAL_UINT64(0, st.text_aborts);
}

TEST_CASE("a non-text byte aborts a line and starts the next unit", "[gnss_framing]")
{
    start();
    feed_str("$GPGGA,123519,48");
    feed(k_v1005, sizeof(k_v1005));                     /* 0xD3 ends the line and starts the frame */
    feed_str("$command,unl");
    static const uint8_t nul = 0;
    feed(&nul, 1);
    feed_str(L_ACK "\r\n");
    TEST_ASSERT_EQUAL_size_t(2, s_count);
    expect_rtcm(0, k_v1005, sizeof(k_v1005), 1005);
    expect_line(1, L_ACK);
    gnss_framing_stats_t st = stats();
    TEST_ASSERT_EQUAL_UINT64(2, st.text_aborts);
    TEST_ASSERT_EQUAL_UINT64(16 + 12 + 1, st.bytes_discarded);
}

TEST_CASE("a line behind noise that ends in a lead byte is still found", "[gnss_framing]")
{
    /* '#' or '$' right in front of a line (both seen as the last byte of a
     * binary message), and a lead byte followed by text that is no line. */
    start();
    feed_str("#" L_NMEA "\r\n");
    feed_str("$" L_ACK "\r\n");              /* "$$...": would pass the '$'-inclusive rule */
    feed_str("$x!" L_BANNER "\r\n");
    feed_str("#garbage" L_VERSION "\n");
    feed_str("$" "$" "#" L_MODE "\r");
    feed(k_v1005, sizeof(k_v1005));
    TEST_ASSERT_EQUAL_size_t(6, s_count);
    expect_line(0, L_NMEA);
    expect_line(1, L_ACK);
    expect_line(2, L_BANNER);
    expect_line(3, L_VERSION);
    expect_line(4, L_MODE);
    expect_rtcm(5, k_v1005, sizeof(k_v1005), 1005);
    gnss_framing_stats_t st = stats();
    TEST_ASSERT_EQUAL_UINT64(1 + 1 + 3 + 8 + 3, st.bytes_discarded);
    TEST_ASSERT_EQUAL_UINT64(5, st.text_lines);
    TEST_ASSERT_EQUAL_UINT64(2, st.text_rejected);   /* "$x!$dev...", "#garbage#VER..." */
    TEST_ASSERT_EQUAL_UINT64(5, st.text_aborts);     /* "#$", "$$", then "$$", "$#", "##" */

    /* The same, byte by byte. */
    start();
    static const char k_all[] = "#" L_NMEA "\r\n" "$" L_ACK "\r\n" "$x!" L_BANNER "\r\n";
    for (size_t i = 0; i < sizeof(k_all) - 1; i++) {
        feed(k_all + i, 1);
    }
    TEST_ASSERT_EQUAL_size_t(3, s_count);
    expect_line(0, L_NMEA);
    expect_line(1, L_ACK);
    expect_line(2, L_BANNER);

    /* A failing line with no lead byte inside is dropped whole, as before. */
    start();
    feed_str("$GPGGA,garbage*00\r\n" L_ACK "\r\n");
    TEST_ASSERT_EQUAL_size_t(1, s_count);
    expect_line(0, L_ACK);
    st = stats();
    TEST_ASSERT_EQUAL_UINT64(strlen("$GPGGA,garbage*00\r\n"), st.bytes_discarded);
}

TEST_CASE("NUL noise between and inside units", "[gnss_framing]")
{
    static const uint8_t zeros[64] = { 0 };
    start();
    feed(zeros, 50);
    feed(k_v1005, sizeof(k_v1005));
    feed(zeros, 10);
    feed_str(L_BANNER "\r\n");
    feed(zeros, 1);
    feed_str("$devicename,");
    feed(zeros, 3);                                     /* NUL inside a line */
    feed_str("COM1*67\r\n");                            /* the rest is not a line on its own */
    feed(k_v1074, sizeof(k_v1074));
    TEST_ASSERT_EQUAL_size_t(3, s_count);
    expect_rtcm(0, k_v1005, sizeof(k_v1005), 1005);
    expect_line(1, L_BANNER);
    expect_rtcm(2, k_v1074, sizeof(k_v1074), 1074);
    gnss_framing_stats_t st = stats();
    TEST_ASSERT_EQUAL_UINT64(50 + 10 + 1 + 12 + 3 + 9, st.bytes_discarded);
    TEST_ASSERT_EQUAL_UINT64(1, st.text_aborts);
}

TEST_CASE("binary logs are skipped, false binary syncs rescanned", "[gnss_framing]")
{
    uint8_t log[GNSS_BIN_HEADER_LEN + 40 + 4];
    memset(log, 0x11, sizeof(log));
    log[0] = 0xAA;
    log[1] = 0x44;
    log[2] = 0xB5;
    log[6] = 40;                                         /* message length, little-endian */
    log[7] = 0;
    memcpy(log + GNSS_BIN_HEADER_LEN, k_v1005, sizeof(k_v1005));   /* frame-like body bytes */
    log[GNSS_BIN_HEADER_LEN + 30] = '$';

    start();
    feed(log, sizeof(log));
    feed(k_v1074, sizeof(k_v1074));
    TEST_ASSERT_EQUAL_size_t(1, s_count);
    expect_rtcm(0, k_v1074, sizeof(k_v1074), 1074);
    gnss_framing_stats_t st = stats();
    TEST_ASSERT_EQUAL_UINT64(1, st.bin_skipped);
    TEST_ASSERT_EQUAL_UINT64(sizeof(log), st.bytes_discarded);
    TEST_ASSERT_EQUAL_UINT64(0, st.resyncs);             /* a recognised unit, not lost sync */

    /* An implausible length is a false sync: the frame inside the "header" is found. */
    uint8_t fake[3 + 3 + 2 + sizeof(k_v1005)];
    fake[0] = 0xAA;
    fake[1] = 0x44;
    fake[2] = 0xB5;
    fake[3] = 0;
    fake[4] = 0;
    fake[5] = 0;
    fake[6] = 0xFF;                                      /* 65535 */
    fake[7] = 0xFF;
    memcpy(fake + 8, k_v1005, sizeof(k_v1005));
    start();
    feed(fake, sizeof(fake));
    TEST_ASSERT_EQUAL_size_t(1, s_count);
    expect_rtcm(0, k_v1005, sizeof(k_v1005), 1005);
    TEST_ASSERT_EQUAL_UINT64(0, stats().bin_skipped);

    /* 0xAA alone is noise. */
    static const uint8_t aa[] = { 0xAA, 0x45, 0xAA, 0x44, 0x00 };
    start();
    feed(aa, sizeof(aa));
    feed(k_v1005, sizeof(k_v1005));
    TEST_ASSERT_EQUAL_size_t(1, s_count);
    TEST_ASSERT_EQUAL_UINT64(sizeof(aa), stats().bytes_discarded);
}

/* ---- fuzzed stream ------------------------------------------------------ */

typedef struct {
    size_t off;
    size_t len;
    gnss_frame_kind_t kind;
} fuzz_unit_t;

#define FUZZ_UNITS 300
static uint8_t s_fuzz[FUZZ_UNITS * 1200 + GNSS_RTCM3_MAX_FRAME];
static fuzz_unit_t s_fuzz_units[FUZZ_UNITS];
static size_t s_fuzz_next;

static uint32_t s_rng;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static void fuzz_check(const gnss_frame_t *fr, void *ctx)
{
    (void)ctx;
    TEST_ASSERT_LESS_THAN_size_t(FUZZ_UNITS, s_fuzz_next);
    const fuzz_unit_t *u = &s_fuzz_units[s_fuzz_next++];
    TEST_ASSERT_EQUAL(u->kind, fr->kind);
    TEST_ASSERT_EQUAL_size_t(u->len, fr->len);
    TEST_ASSERT_EQUAL_MEMORY(s_fuzz + u->off, fr->data, u->len);
}

/* Noise that cannot start a line or a binary log, rich in false RTCM headers. */
static size_t put_noise(uint8_t *p, size_t max)
{
    size_t n = rnd() % (max + 1);
    for (size_t i = 0; i < n; i++) {
        uint32_t r = rnd();
        uint8_t c;
        switch (r % 8) {
        case 0: c = 0x00; break;
        case 1: c = GNSS_RTCM3_PREAMBLE; break;
        case 2: c = (uint8_t)(r >> 8) & 0x03u; break;      /* zero reserved bits */
        case 3: c = (r & 0x100) ? '\r' : '\n'; break;
        default: c = (uint8_t)(r >> 16); break;
        }
        if (c == '$' || c == '#' || c == 0xAA) {
            c = 0x7F;
        }
        p[i] = c;
    }
    return n;
}

TEST_CASE("fuzzed stream: every unit recovered in order, every byte accounted", "[gnss_framing]")
{
    static const char *const lines[] = {
        L_ACK, L_ACK_VER, L_CONFIG, L_FAILED, L_BANNER, L_NMEA, L_NMEA_IN, L_MODE, L_VERSION, L_NOTICE, L_SG_NO,
    };
    static gnss_framer_t fr;
    for (uint32_t seed = 1; seed <= 4; seed++) {
        s_rng = 0x9E3779B9u * seed;
        size_t n = 0, units = 0, noise = 0, unit_bytes = 0;
        while (units < FUZZ_UNITS) {
            size_t g = put_noise(s_fuzz + n, (rnd() % 4 == 0) ? 1100 : 30);
            n += g;
            noise += g;
            fuzz_unit_t *u = &s_fuzz_units[units++];
            u->off = n;
            switch (rnd() % 4) {
            case 0:
                memcpy(s_fuzz + n, k_v1005, sizeof(k_v1005));
                u->len = sizeof(k_v1005);
                u->kind = GNSS_FRAME_RTCM3;
                break;
            case 1:
                memcpy(s_fuzz + n, k_v1074, sizeof(k_v1074));
                u->len = sizeof(k_v1074);
                u->kind = GNSS_FRAME_RTCM3;
                break;
            case 2: {
                uint8_t payload[GNSS_RTCM3_MAX_PAYLOAD];
                size_t pl = 2 + rnd() % (GNSS_RTCM3_MAX_PAYLOAD - 1);
                for (size_t i = 0; i < pl; i++) {
                    payload[i] = (uint8_t)rnd();
                }
                u->len = make_frame(s_fuzz + n, payload, pl);
                u->kind = GNSS_FRAME_RTCM3;
                break;
            }
            default: {
                const char *l = lines[rnd() % (sizeof(lines) / sizeof(lines[0]))];
                u->len = strlen(l);
                u->kind = GNSS_FRAME_TEXT;
                memcpy(s_fuzz + n, l, u->len);
                memcpy(s_fuzz + n + u->len, "\r\n", 2);
                n += 2;
                unit_bytes += 2;
                break;
            }
            }
            n += u->len;
            unit_bytes += u->len;
        }
        /* Quiet tail: lets a false candidate still open complete and fail. */
        memset(s_fuzz + n, 0, GNSS_RTCM3_MAX_FRAME);
        n += GNSS_RTCM3_MAX_FRAME;
        noise += GNSS_RTCM3_MAX_FRAME;
        TEST_ASSERT_LESS_OR_EQUAL_size_t(sizeof(s_fuzz), n);

        s_fuzz_next = 0;
        gnss_framer_init(&fr, fuzz_check, NULL);
        for (size_t off = 0; off < n;) {
            size_t chunk = 1 + rnd() % 700;
            if (chunk > n - off) {
                chunk = n - off;
            }
            gnss_framer_feed(&fr, s_fuzz + off, chunk);
            off += chunk;
        }
        gnss_framer_reset(&fr);
        TEST_ASSERT_EQUAL_size_t(FUZZ_UNITS, s_fuzz_next);

        gnss_framing_stats_t st;
        gnss_framer_get_stats(&fr, &st);
        TEST_ASSERT_EQUAL_UINT64(n, st.bytes_in);
        TEST_ASSERT_EQUAL_UINT64(noise, st.bytes_discarded);
        TEST_ASSERT_EQUAL_UINT64(n, unit_bytes + noise);
        TEST_ASSERT_EQUAL_UINT64(0, st.text_rejected);
        TEST_ASSERT_EQUAL_UINT64(FUZZ_UNITS, st.rtcm_frames + st.text_lines);
        /* The noise really exercised the rescans. */
        TEST_ASSERT_GREATER_THAN_UINT64(500, st.rtcm_crc_errors);
        TEST_ASSERT_GREATER_THAN_UINT64(500, st.rtcm_reserved_nonzero);
        TEST_ASSERT_GREATER_THAN_UINT64(10, st.rtcm_bad_length);
    }
}

/* ---- loader prompt detector --------------------------------------------- */

static size_t count_prompts(void)
{
    size_t n = 0;
    for (size_t i = 0; i < s_count; i++) {
        if (s_units[i].kind == GNSS_FRAME_PROMPT) {
            TEST_ASSERT_EQUAL_size_t(5, s_units[i].len);
            TEST_ASSERT_EQUAL_MEMORY("boot>", s_units[i].data, 5);
            n++;
        }
    }
    return n;
}

TEST_CASE("framer prompt detector arms and disarms", "[gnss_framing]")
{
    start();
    TEST_ASSERT_FALSE(s_framer.prompt_armed);
    gnss_framer_set_prompt_detect(&s_framer, true);
    TEST_ASSERT_TRUE(s_framer.prompt_armed);
    gnss_framer_reset(&s_framer);                          /* a baud switch keeps it armed */
    TEST_ASSERT_TRUE(s_framer.prompt_armed);
    gnss_framer_set_prompt_detect(&s_framer, false);
    TEST_ASSERT_FALSE(s_framer.prompt_armed);
    TEST_ASSERT_EQUAL_UINT8(0, s_framer.prompt_match);
}

TEST_CASE("prompt found in one read and across read boundaries", "[gnss_framing]")
{
    start();
    gnss_framer_set_prompt_detect(&s_framer, true);
    feed_str("\r\nboot>");
    TEST_ASSERT_EQUAL_size_t(1, count_prompts());

    feed_str("Invalid input\r\nbo");
    TEST_ASSERT_EQUAL_size_t(1, count_prompts());
    feed_str("ot");
    feed_str(">");
    TEST_ASSERT_EQUAL_size_t(2, count_prompts());

    feed_str("bbboboot>");                                 /* restarts on a repeated first byte */
    TEST_ASSERT_EQUAL_size_t(3, count_prompts());
    TEST_ASSERT_EQUAL_UINT64(3, stats().prompts);
}

TEST_CASE("prompt is NUL-safe", "[gnss_framing]")
{
    static const uint8_t after_nul[] = { 0x00, 0x00, 'b', 'o', 'o', 't', '>' };
    static const uint8_t nul_inside[] = { 'b', 'o', 0x00, 'o', 't', '>' };
    start();
    gnss_framer_set_prompt_detect(&s_framer, true);
    feed(after_nul, sizeof(after_nul));
    TEST_ASSERT_EQUAL_size_t(1, count_prompts());
    feed(nul_inside, sizeof(nul_inside));
    TEST_ASSERT_EQUAL_size_t(1, count_prompts());
}

TEST_CASE("prompt only while armed; disarming forgets a partial match", "[gnss_framing]")
{
    start();
    feed_str("boot>");
    TEST_ASSERT_EQUAL_size_t(0, count_prompts());

    gnss_framer_set_prompt_detect(&s_framer, true);
    feed_str("boo");
    gnss_framer_set_prompt_detect(&s_framer, true);       /* re-arming keeps the match */
    feed_str("t>");
    TEST_ASSERT_EQUAL_size_t(1, count_prompts());

    feed_str("boo");
    gnss_framer_set_prompt_detect(&s_framer, false);
    gnss_framer_set_prompt_detect(&s_framer, true);
    feed_str("t>");
    TEST_ASSERT_EQUAL_size_t(1, count_prompts());
}

TEST_CASE("prompt split across a framer reset (baud switch)", "[gnss_framing]")
{
    start();
    gnss_framer_set_prompt_detect(&s_framer, true);
    feed_str("boo");
    gnss_framer_reset(&s_framer);
    feed_str("t>");
    TEST_ASSERT_EQUAL_size_t(1, count_prompts());
}

TEST_CASE("prompt ignored with RTCM within the window", "[gnss_framing]")
{
    static uint8_t gap[GNSS_PROMPT_WINDOW + 100];
    memset(gap, 'z', sizeof(gap));
    start();
    gnss_framer_set_prompt_detect(&s_framer, true);

    feed(k_v1005, sizeof(k_v1005));                        /* frame just before */
    feed_str("boot>");
    TEST_ASSERT_EQUAL_size_t(0, count_prompts());
    TEST_ASSERT_EQUAL_UINT64(1, stats().prompts_ignored);

    feed(gap, sizeof(gap));                                /* far enough behind now */
    feed_str("boot>");
    TEST_ASSERT_EQUAL_size_t(1, count_prompts());

    /* Frame just after, in the same read. */
    uint8_t buf[5 + 100 + sizeof(k_v1005)];
    memcpy(buf, "boot>", 5);
    memset(buf + 5, 'z', 100);
    memcpy(buf + 105, k_v1005, sizeof(k_v1005));
    feed(gap, sizeof(gap));
    feed(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(1, count_prompts());
    TEST_ASSERT_EQUAL_UINT64(2, stats().prompts_ignored);

    /* Frame after, but beyond the window: reported. */
    feed(gap, sizeof(gap));
    feed_str("boot>");
    feed(gap, sizeof(gap));
    feed(k_v1005, sizeof(k_v1005));
    TEST_ASSERT_EQUAL_size_t(2, count_prompts());
}

/* A frame whose payload carries "boot>" near its start and is long enough to
 * end more than a window after it. */
static size_t prompt_frame(uint8_t *f)
{
    static uint8_t payload[1000];
    memset(payload, 'x', sizeof(payload));
    payload[0] = 0x3E;
    payload[1] = 0xD0;
    memcpy(payload + 2, "boot>", 5);
    return make_frame(f, payload, sizeof(payload));
}

TEST_CASE("prompt inside a valid frame is never reported", "[gnss_framing]")
{
    static uint8_t f[GNSS_RTCM3_MAX_FRAME];
    size_t n = prompt_frame(f);
    start();
    gnss_framer_set_prompt_detect(&s_framer, true);
    feed(f, 10);                                           /* up to the prompt's last byte */
    TEST_ASSERT_EQUAL_size_t(0, count_prompts());          /* waits for the candidate */
    feed(f + 10, n - 10);
    TEST_ASSERT_EQUAL_size_t(1, s_count);
    TEST_ASSERT_EQUAL(GNSS_FRAME_RTCM3, s_units[0].kind);
    TEST_ASSERT_EQUAL_UINT64(1, stats().prompts_ignored);
    TEST_ASSERT_EQUAL_UINT64(0, stats().prompts);
}

TEST_CASE("prompt inside a candidate that fails is reported then", "[gnss_framing]")
{
    static uint8_t f[GNSS_RTCM3_MAX_FRAME];
    size_t n = prompt_frame(f);
    f[n - 1] ^= 0x01;                                      /* broken CRC */
    start();
    gnss_framer_set_prompt_detect(&s_framer, true);
    feed(f, 10);
    TEST_ASSERT_EQUAL_size_t(0, count_prompts());
    feed(f + 10, n - 10);
    TEST_ASSERT_EQUAL_size_t(1, count_prompts());

    /* A reset also settles a hit that waits on an open candidate. */
    static const uint8_t hdr[] = { 0xD3, 0x00, 0x64 };
    feed(hdr, sizeof(hdr));
    feed_str("boot>");
    TEST_ASSERT_EQUAL_size_t(1, count_prompts());
    gnss_framer_reset(&s_framer);
    TEST_ASSERT_EQUAL_size_t(2, count_prompts());
}

/* ---- version log -------------------------------------------------------- */

TEST_CASE("version log parser reads the body fields", "[gnss_framing]")
{
    gnss_versiona_t v;
    memset(&v, 0x5A, sizeof(v));
    TEST_ASSERT_TRUE(gnss_versiona_parse(L_VERSION, strlen(L_VERSION), &v));
    TEST_ASSERT_EQUAL_STRING("UM982", v.model);
    TEST_ASSERT_EQUAL_STRING("R4.10Build18975", v.firmware);
    TEST_ASSERT_EQUAL_UINT32(18975, v.build);
    TEST_ASSERT_EQUAL_STRING("2310415000001", v.pn);
    TEST_ASSERT_EQUAL_STRING("XY12345678901", v.serial);
    TEST_ASSERT_EQUAL_STRING("ffff48ffff0fffff", v.efuse_id);
    TEST_ASSERT_EQUAL_STRING("2025/04/30", v.compile_date);

    /* Trailing CR LF and upper-case CRC digits are accepted. */
    static const char crlf[] = L_VERSION "\r\n";
    TEST_ASSERT_TRUE(gnss_versiona_parse(crlf, strlen(crlf), &v));
    char upper[sizeof(L_VERSION)];
    memcpy(upper, L_VERSION, sizeof(upper));
    for (size_t i = sizeof(upper) - 9; i < sizeof(upper) - 1; i++) {
        if (upper[i] >= 'a' && upper[i] <= 'f') {
            upper[i] = (char)(upper[i] - 'a' + 'A');
        }
    }
    TEST_ASSERT_TRUE(gnss_versiona_parse(upper, strlen(upper), &v));
    TEST_ASSERT_EQUAL_UINT32(18975, v.build);

    /* No "Build", seven fields, PN-SN without '-' (constructed). */
    static const char other[] = "#VERSIONA,79,GPS,FINE,2338,412345000,0,0,18,975;\"UM980\",\"R4.10\",\"X\","
                                "\"SN0001\",\"ID\",\"2024/01/02\",\"extra\"*2782e601";
    TEST_ASSERT_TRUE(gnss_versiona_parse(other, strlen(other), &v));
    TEST_ASSERT_EQUAL_STRING("UM980", v.model);
    TEST_ASSERT_EQUAL_STRING("R4.10", v.firmware);
    TEST_ASSERT_EQUAL_UINT32(0, v.build);
    TEST_ASSERT_EQUAL_STRING("", v.pn);
    TEST_ASSERT_EQUAL_STRING("SN0001", v.serial);

    /* A build number too large to compare counts as absent. */
    static const char huge[] = "#VERSIONA,COM1;\"UM982\",\"R4.10Build99999999999\",\"A\",\"P-S\",\"E\",\"2025/04/30\"*e8127411";
    TEST_ASSERT_TRUE(gnss_versiona_parse(huge, strlen(huge), &v));
    TEST_ASSERT_EQUAL_UINT32(0, v.build);
}

TEST_CASE("version log parser rejects lines that are not a valid version log", "[gnss_framing]")
{
    static const char *const lines[] = {
        "",
        L_NMEA,
        L_MODE,
        "#VERSIONA,COM1;\"UM982\"*00000000",                 /* bad CRC, too few fields */
        "#VERSIONA,COM1;\"UM982\",\"R4.10Build18975\",\"A\",\"P-S\",\"E\"*699bd987",   /* five fields */
        "#VERSIONA,COM1;\"UM982-THIS-IS-TOO-LONG\",\"R4.10Build1\",\"A\",\"P-S\",\"E\",\"2025/04/30\"*3b125524",
        "#VERSIONA,79,GPS,FINE,2338,412345000,0,0,18,975;\"UM982\",\"R4.10Build18975\","
        "\"HRPT00-S10C-P\",\"2310415000001-XY12345678901\",\"ffff48ffff0fffff\",\"2025/04/31\"*9ba66a41",
        "#VERSION,79,GPS,FINE,2338,412345000,0,0,18,975;\"UM982\",\"R4.10Build18975\","
        "\"HRPT00-S10C-P\",\"2310415000001-XY12345678901\",\"ffff48ffff0fffff\",\"2025/04/30\"*9ba66a41",
    };
    gnss_versiona_t v;
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        memset(&v, 0x5A, sizeof(v));
        TEST_ASSERT_FALSE_MESSAGE(gnss_versiona_parse(lines[i], strlen(lines[i]), &v), lines[i]);
        TEST_ASSERT_EQUAL_STRING("", v.model);
        TEST_ASSERT_EQUAL_UINT32(0, v.build);
    }

    /* A NUL cut into the line (raw text) is refused even though the CRC is not reached. */
    char with_nul[sizeof(L_VERSION)];
    memcpy(with_nul, L_VERSION, sizeof(with_nul));
    with_nul[60] = '\0';
    TEST_ASSERT_FALSE(gnss_versiona_parse(with_nul, sizeof(with_nul) - 1, &v));
    TEST_ASSERT_FALSE(gnss_versiona_parse(NULL, 0, &v));
    TEST_ASSERT_FALSE(gnss_versiona_parse(L_VERSION, strlen(L_VERSION), NULL));
}

TEST_CASE("version log line from the framer feeds the parser", "[gnss_framing]")
{
    start();
    feed(k_v1074, sizeof(k_v1074));
    feed_str(L_ACK_VER "\r\n");
    feed(k_v1005, sizeof(k_v1005));
    feed_str(L_VERSION "\r\n");
    TEST_ASSERT_EQUAL_size_t(4, s_count);
    TEST_ASSERT_EQUAL(GNSS_FRAME_TEXT, s_units[3].kind);
    gnss_versiona_t v;
    TEST_ASSERT_TRUE(gnss_versiona_parse((const char *)s_units[3].data, s_units[3].len, &v));
    TEST_ASSERT_EQUAL_STRING("UM982", v.model);
    TEST_ASSERT_FALSE(gnss_versiona_parse((const char *)s_units[1].data, s_units[1].len, &v));
}
