/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * XMODEM-1K sender and the receive classifier against the simulated
 * receiver (fl_sim.c): one test per row of the handshake, per-block and EOT
 * tables, the framing and padding rules and the limits.
 */
#include <stdlib.h>
#include <string.h>

#include "fl_crc.h"
#include "fl_rx.h"
#include "fl_sim.h"
#include "unity.h"
#include "xmodem_tx.h"

static sim_t s_sim;
static fl_io_t s_io;
static fl_rx_t s_rx;
static xm_state_t s_xm;
static uint8_t s_data[300u * 1024u + 333u];
static uint8_t s_post[4096];
static size_t s_post_len;
static uint32_t s_first_frame_calls;
static bool s_refuse_first;

static void post_tap(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (s_post_len + n <= sizeof(s_post)) {
        memcpy(s_post + s_post_len, d, n);
        s_post_len += n;
    }
}

static bool first_frame(void *ctx, xm_mode_t mode)
{
    (void)ctx;
    (void)mode;
    s_first_frame_calls++;
    return !s_refuse_first;
}

static void setup(uint32_t baud)
{
    sim_init(&s_sim);
    sim_io(&s_sim, &s_io);
    sim_at_xrx(&s_sim, baud);
    fl_rx_init(&s_rx, &s_io);
    s_post_len = 0;
    s_first_frame_calls = 0;
    s_refuse_first = false;
    for (size_t i = 0; i < sizeof(s_data); i++) {
        s_data[i] = (uint8_t)(i * 31u + (i >> 10));
    }
}

static xm_result_t send(uint32_t len)
{
    xm_job_t job = {
        .data = s_data,
        .len = len,
        .before_first_frame = first_frame,
        .post_tap = post_tap,
    };
    return xm_send(&s_io, &s_rx, &job, &s_xm);
}

static void assert_image(uint32_t len)
{
    TEST_ASSERT_EQUAL_size_t((len + 1023u) / 1024u * 1024u, s_sim.img_len);
    TEST_ASSERT_EQUAL_MEMORY(s_data, s_sim.img, len);
    /* Padding: one 0x1A, then 0x00. */
    if (len % 1024u != 0) {
        TEST_ASSERT_EQUAL_HEX8(0x1A, s_sim.img[len]);
        for (size_t i = len + 1u; i < s_sim.img_len; i++) {
            TEST_ASSERT_EQUAL_HEX8(0x00, s_sim.img[i]);
        }
    }
}

TEST_CASE("xmodem: frame layout, block wrap, CRC high byte first, checksum mode", "[xmodem]")
{
    static uint8_t data[257u * 1024u + 5u];
    static uint8_t f[XM_FRAME_MAX];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)i;
    }
    size_t n = xm_build_frame(data, sizeof(data), 0, XM_MODE_CRC, f);
    TEST_ASSERT_EQUAL_size_t(1029, n);
    TEST_ASSERT_EQUAL_HEX8(0x02, f[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, f[1]);
    TEST_ASSERT_EQUAL_HEX8(0xFE, f[2]);
    uint16_t crc = fl_crc16_xmodem(data, 1024);
    TEST_ASSERT_EQUAL_HEX8(crc >> 8, f[1027]);
    TEST_ASSERT_EQUAL_HEX8(crc & 0xFF, f[1028]);
    /* Block numbers wrap 255 -> 0. */
    xm_build_frame(data, sizeof(data), 254, XM_MODE_CRC, f);
    TEST_ASSERT_EQUAL_HEX8(255, f[1]);
    TEST_ASSERT_EQUAL_HEX8(0, f[2]);
    xm_build_frame(data, sizeof(data), 255, XM_MODE_CRC, f);
    TEST_ASSERT_EQUAL_HEX8(0, f[1]);
    TEST_ASSERT_EQUAL_HEX8(255, f[2]);
    xm_build_frame(data, sizeof(data), 256, XM_MODE_CRC, f);
    TEST_ASSERT_EQUAL_HEX8(1, f[1]);
    /* Checksum mode: one sum byte of the data only. */
    n = xm_build_frame(data, sizeof(data), 1, XM_MODE_SUM, f);
    TEST_ASSERT_EQUAL_size_t(1028, n);
    TEST_ASSERT_EQUAL_HEX8(fl_sum8(data + 1024, 1024), f[1027]);
    /* Last short frame: data, one 0x1A, then zeros. */
    xm_build_frame(data, sizeof(data), 257, XM_MODE_CRC, f);
    TEST_ASSERT_EQUAL_MEMORY(data + 257u * 1024u, f + 3, 5);
    TEST_ASSERT_EQUAL_HEX8(0x1A, f[3 + 5]);
    for (int i = 3 + 6; i < 3 + 1024; i++) {
        TEST_ASSERT_EQUAL_HEX8(0, f[i]);
    }
    TEST_ASSERT_EQUAL_UINT32(258, xm_blocks(sizeof(data)));
}

TEST_CASE("xmodem: an exact multiple of 1024 or nothing is refused before any byte", "[xmodem]")
{
    setup(460800);
    TEST_ASSERT_EQUAL(XM_REFUSED, send(4096));
    TEST_ASSERT_EQUAL(XM_REFUSED, send(0));
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_frames);
    TEST_ASSERT_EQUAL_UINT32(0, s_first_frame_calls);
    sim_free(&s_sim);
}

TEST_CASE("xmodem: 'C' first locks CRC mode; 300 frames with the block number wrapping", "[xmodem]")
{
    setup(460800);
    uint32_t len = 300u * 1024u + 333u;
    TEST_ASSERT_EQUAL(XM_OK, send(len));
    TEST_ASSERT_EQUAL(XM_MODE_CRC, s_xm.mode);
    TEST_ASSERT_EQUAL_UINT32(301, s_xm.blocks_acked);
    TEST_ASSERT_EQUAL_UINT32(301, s_sim.rx_frames);
    TEST_ASSERT_TRUE(s_xm.eot_acked);
    TEST_ASSERT_EQUAL_UINT8(1, s_xm.eots);
    TEST_ASSERT_EQUAL_UINT32(1, s_first_frame_calls);
    assert_image(len);
    sim_free(&s_sim);
}

TEST_CASE("xmodem: NAK only locks checksum mode; a delayed start inside the window is fine", "[xmodem]")
{
    setup(460800);
    s_sim.start = 0x15;
    s_sim.start_delay_ms = 9000;
    TEST_ASSERT_EQUAL(XM_OK, send(5000));
    TEST_ASSERT_EQUAL(XM_MODE_SUM, s_xm.mode);
    TEST_ASSERT_TRUE(s_xm.handshake_ms >= 9000);
    assert_image(5000);
    sim_free(&s_sim);
}

TEST_CASE("xmodem: 'C' first then NAK later: the first valid start byte wins", "[xmodem]")
{
    setup(460800);
    s_sim.start_delay_ms = 20;
    s_sim.nak_after_ms = 5000;
    TEST_ASSERT_EQUAL(XM_OK, send(3000));
    TEST_ASSERT_EQUAL(XM_MODE_CRC, s_xm.mode);
    sim_free(&s_sim);
}

TEST_CASE("xmodem: junk and a 'C' that starts a line are not start bytes", "[xmodem]")
{
    setup(460800);
    s_sim.start_junk = true;
    s_sim.c_text_before_start = true;    /* "Checking flash" before the start byte */
    s_sim.start_delay_ms = 600;
    TEST_ASSERT_EQUAL(XM_OK, send(3000));
    TEST_ASSERT_EQUAL(XM_MODE_CRC, s_xm.mode);
    TEST_ASSERT_TRUE(s_xm.start_junk > 0);
    TEST_ASSERT_EQUAL_UINT32(3, s_sim.rx_frames);   /* the text never caused a resend */
    sim_free(&s_sim);
}

TEST_CASE("xmodem: no start byte in 15 s: CAN CAN, handshake failed, no frame", "[xmodem]")
{
    setup(460800);
    s_sim.start_delay_ms = 16000;
    uint32_t t0 = sim_now_ms(&s_sim);
    TEST_ASSERT_EQUAL(XM_HANDSHAKE_FAILED, send(3000));
    uint32_t took = sim_now_ms(&s_sim) - t0;
    TEST_ASSERT_TRUE(took >= 15000 && took < 16000);
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_frames);
    TEST_ASSERT_EQUAL_UINT32(2, s_sim.rx_cans);
    TEST_ASSERT_FALSE(s_xm.first_frame_sent);
    TEST_ASSERT_EQUAL_UINT32(0, s_first_frame_calls);
    sim_free(&s_sim);
}

TEST_CASE("xmodem: loader text in the handshake fails it; other text waits", "[xmodem]")
{
    const char *fail[] = { "Get input timeout", "## Binary download aborted", "Invalid input", "boot> ",
                           "input is too long", "Choose firmware for update" };
    for (size_t i = 0; i < sizeof(fail) / sizeof(fail[0]); i++) {
        setup(460800);
        s_sim.start_delay_ms = 5000;
        fl_rx_push(&s_rx, (const uint8_t *)fail[i], strlen(fail[i]));
        fl_rx_push(&s_rx, (const uint8_t *)"\r\n", 2);
        TEST_ASSERT_EQUAL_MESSAGE(XM_HANDSHAKE_FAILED, send(3000), fail[i]);
        TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_frames);
        sim_free(&s_sim);
    }
    setup(460800);
    s_sim.start_delay_ms = 2000;
    fl_rx_push(&s_rx, (const uint8_t *)"erasing\r\n", 9);
    TEST_ASSERT_EQUAL(XM_OK, send(3000));
    sim_free(&s_sim);
}

TEST_CASE("xmodem: the receiver's CAN CAN in the handshake: one ACK, failed", "[xmodem]")
{
    setup(460800);
    s_sim.start_delay_ms = 5000;
    static const uint8_t can2[2] = { FL_CAN, FL_CAN };
    fl_rx_push(&s_rx, can2, 2);
    TEST_ASSERT_EQUAL(XM_HANDSHAKE_FAILED, send(3000));
    TEST_ASSERT_EQUAL_UINT32(1, s_sim.rx_acks);
    sim_free(&s_sim);
}

TEST_CASE("xmodem: before_first_frame refusing sends nothing (pre-PoNR)", "[xmodem]")
{
    setup(460800);
    s_refuse_first = true;
    TEST_ASSERT_EQUAL(XM_NOT_STARTED, send(3000));
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_frames);
    TEST_ASSERT_FALSE(s_xm.first_frame_sent);
    sim_free(&s_sim);
}

TEST_CASE("xmodem: NAK and a lone 'C' resend the same frame and count", "[xmodem]")
{
    setup(460800);
    s_sim.nak_block = 2;
    s_sim.nak_times = 3;
    s_sim.c_block = 3;
    TEST_ASSERT_EQUAL(XM_OK, send(4000));
    TEST_ASSERT_EQUAL_UINT32(4, s_xm.retries);
    TEST_ASSERT_EQUAL_UINT32(4 + 4, s_sim.rx_frames);
    assert_image(4000);
    sim_free(&s_sim);
}

TEST_CASE("xmodem: 30 s without an answer resends; 60 s per block is the cap", "[xmodem]")
{
    setup(460800);
    s_sim.pause_block = 2;
    s_sim.pause_ms = 40000;    /* the ACK of the first send arrives after the resend */
    TEST_ASSERT_EQUAL(XM_OK, send(3000));
    TEST_ASSERT_EQUAL_UINT32(1, s_xm.retries);
    sim_free(&s_sim);

    setup(460800);
    s_sim.silent_block = 2;
    s_sim.junk_every_s = 1;    /* junk every second never stops the clock */
    uint32_t t0 = sim_now_ms(&s_sim);
    TEST_ASSERT_EQUAL(XM_FRAME_TIMEOUT, send(3000));
    uint32_t took = sim_now_ms(&s_sim) - t0;
    TEST_ASSERT_TRUE(took >= 60000 && took < 62000);
    TEST_ASSERT_EQUAL_UINT32(2, s_sim.rx_cans);   /* CAN CAN */
    TEST_ASSERT_EQUAL_UINT32(1 + 2, s_sim.rx_frames);   /* block 2 sent at 0 and 30 s */
    sim_free(&s_sim);
}

TEST_CASE("xmodem: 10 failed tries on one block, or 40 in the transfer, end it with CAN CAN", "[xmodem]")
{
    setup(460800);
    s_sim.nak_block = 2;
    s_sim.nak_times = 100;
    TEST_ASSERT_EQUAL(XM_RETRY_LIMIT, send(3000));
    TEST_ASSERT_EQUAL_UINT32(1 + 10, s_sim.rx_frames);
    TEST_ASSERT_EQUAL_UINT32(2, s_sim.rx_cans);
    sim_free(&s_sim);

    /* 9 failures on one block are still allowed. */
    setup(460800);
    s_sim.nak_block = 1;
    s_sim.nak_times = 9;
    TEST_ASSERT_EQUAL(XM_OK, send(3000));
    TEST_ASSERT_EQUAL_UINT32(9, s_xm.retries);
    sim_free(&s_sim);

    /* 9 failures on every block: the transfer-wide 40 ends it in block 5. */
    setup(460800);
    s_sim.nak_every = 9;
    TEST_ASSERT_EQUAL(XM_RETRY_LIMIT, send(8u * 1024u + 1u));
    TEST_ASSERT_EQUAL_UINT32(40, s_xm.retries);
    TEST_ASSERT_EQUAL_UINT32(4, s_xm.blocks_acked);
    TEST_ASSERT_EQUAL_UINT32(4u * 10u + 4u, s_sim.rx_frames);
    TEST_ASSERT_EQUAL_UINT32(2, s_sim.rx_cans);
    sim_free(&s_sim);
}

TEST_CASE("xmodem: junk, a lone CAN and a stale duplicate ACK are ignored", "[xmodem]")
{
    setup(460800);
    s_sim.junk_before_ack = true;
    s_sim.lone_can_before_ack = true;
    s_sim.dup_ack_block = 1;
    TEST_ASSERT_EQUAL(XM_OK, send(5000));
    TEST_ASSERT_EQUAL_UINT32(0, s_xm.retries);
    /* The duplicate ACK of block 1 did not acknowledge block 2 unseen: every block reached the receiver. */
    assert_image(5000);
    sim_free(&s_sim);
}

TEST_CASE("xmodem: the receiver's CAN CAN mid-transfer: one ACK, stop", "[xmodem]")
{
    setup(460800);
    s_sim.can_block = 3;
    TEST_ASSERT_EQUAL(XM_CANCELLED, send(6000));
    TEST_ASSERT_TRUE(s_xm.first_frame_sent);
    TEST_ASSERT_EQUAL_UINT32(1, s_sim.rx_acks);
    TEST_ASSERT_EQUAL_UINT32(3, s_sim.rx_frames);
    sim_free(&s_sim);
}

TEST_CASE("xmodem: an error report waits; any other text stops at once, never resent", "[xmodem]")
{
    setup(460800);
    s_sim.text_block = 2;
    s_sim.text = "xmodem error";
    s_sim.text_then_nak = true;
    TEST_ASSERT_EQUAL(XM_OK, send(4000));
    TEST_ASSERT_EQUAL_UINT32(1, s_xm.retries);
    sim_free(&s_sim);

    setup(460800);
    s_sim.text_block = 2;
    s_sim.text = "uart getc timeout";
    s_sim.text_then_nak = true;
    TEST_ASSERT_EQUAL(XM_OK, send(4000));
    sim_free(&s_sim);

    const char *stop[] = { "## Binary download aborted", "boot> ", "Invalid input", "something unknown" };
    for (size_t i = 0; i < sizeof(stop) / sizeof(stop[0]); i++) {
        setup(460800);
        s_sim.text_block = 2;
        s_sim.text = stop[i];
        s_sim.text_then_nak = false;
        TEST_ASSERT_EQUAL_MESSAGE(XM_TEXT_ABORT, send(6000), stop[i]);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, s_sim.rx_frames, stop[i]);   /* nothing after the text */
        TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_cans);
        sim_free(&s_sim);
    }
}

TEST_CASE("xmodem: the baud-mismatch pattern stops the transfer", "[xmodem]")
{
    setup(460800);
    s_sim.mismatch_block = 2;
    s_sim.app_period_ms = 50;
    TEST_ASSERT_EQUAL(XM_BAUD_MISMATCH, send(6000));
    TEST_ASSERT_EQUAL_UINT32(2, s_sim.rx_frames);
    sim_free(&s_sim);
}

TEST_CASE("xmodem: EOT NAK resent, at most ten; silence and text end it; nothing else after the last ACK", "[xmodem]")
{
    setup(460800);
    s_sim.eot_naks = 3;
    TEST_ASSERT_EQUAL(XM_OK, send(3000));
    TEST_ASSERT_EQUAL_UINT8(4, s_xm.eots);
    TEST_ASSERT_TRUE(s_xm.eot_acked);
    sim_free(&s_sim);

    setup(460800);
    s_sim.eot_naks = 50;
    TEST_ASSERT_EQUAL(XM_OK, send(3000));
    TEST_ASSERT_EQUAL_UINT8(10, s_xm.eots);
    TEST_ASSERT_FALSE(s_xm.eot_acked);
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_cans);
    sim_free(&s_sim);

    setup(460800);
    s_sim.eot_silent = true;
    s_sim.post = SIM_POST_SILENT;
    uint32_t t0;
    TEST_ASSERT_EQUAL(XM_OK, send(3000));
    t0 = s_xm.last_eot_ms;
    (void)t0;
    TEST_ASSERT_EQUAL_UINT8(1, s_xm.eots);
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_cans);
    sim_free(&s_sim);

    setup(460800);
    s_sim.eot_text = true;
    TEST_ASSERT_EQUAL(XM_OK, send(3000));
    TEST_ASSERT_EQUAL_UINT8(1, s_xm.eots);
    /* The text after EOT starts the post-transfer buffer. */
    TEST_ASSERT_NOT_NULL(fl_memmem(s_post, s_post_len, "writing"));
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_cans);
    sim_free(&s_sim);
}

TEST_CASE("xmodem: bench stop after N blocks sends nothing more", "[xmodem]")
{
    setup(460800);
    xm_job_t job = { .data = s_data, .len = 9000, .stop_after = 3 };
    TEST_ASSERT_EQUAL(XM_STOPPED, xm_send(&s_io, &s_rx, &job, &s_xm));
    TEST_ASSERT_EQUAL_UINT32(3, s_sim.rx_frames);
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_cans);
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_eots);
    sim_free(&s_sim);
}

/* ---- classifier primitives ------------------------------------------------------- */

TEST_CASE("rx classifier: lone C, C-run, C starting text, CAN rules, blank captures", "[xmodem]")
{
    setup(460800);
    s_sim.start_delay_ms = 1000000;   /* the simulator stays quiet */
    uint32_t far = sim_now_ms(&s_sim) + 5000;
    fl_rx_push(&s_rx, (const uint8_t *)"CCC", 3);
    TEST_ASSERT_EQUAL(FL_EV_C, fl_rx_next(&s_rx, far));
    fl_rx_push(&s_rx, (const uint8_t *)"Choose\r\n", 8);
    TEST_ASSERT_EQUAL(FL_EV_TEXT, fl_rx_next(&s_rx, far));
    TEST_ASSERT_TRUE(fl_rx_line_has(&s_rx, "Choose"));
    static const uint8_t can_x[2] = { FL_CAN, FL_NAK };
    fl_rx_push(&s_rx, can_x, 2);
    TEST_ASSERT_EQUAL(FL_EV_CAN1, fl_rx_next(&s_rx, far));
    TEST_ASSERT_EQUAL(FL_EV_NAK, fl_rx_next(&s_rx, far));   /* the byte after a lone CAN is not swallowed */
    static const uint8_t can_can[2] = { FL_CAN, FL_CAN };
    fl_rx_push(&s_rx, can_can, 2);
    TEST_ASSERT_EQUAL(FL_EV_CANCEL, fl_rx_next(&s_rx, far));
    fl_rx_push(&s_rx, (const uint8_t *)"\r\n", 2);
    TEST_ASSERT_EQUAL(FL_EV_JUNK, fl_rx_next(&s_rx, far));
    static const uint8_t c_ack[2] = { 'C', FL_ACK };
    fl_rx_push(&s_rx, c_ack, 2);
    TEST_ASSERT_EQUAL(FL_EV_C, fl_rx_next(&s_rx, far));
    TEST_ASSERT_EQUAL(FL_EV_ACK, fl_rx_next(&s_rx, far));
    /* Stale single control bytes are dropped before a resend, text is not. */
    static const uint8_t stale[5] = { FL_ACK, FL_NAK, 'C', 0x00, FL_ACK };
    fl_rx_push(&s_rx, stale, 5);
    fl_rx_drop_stale(&s_rx);
    TEST_ASSERT_EQUAL(FL_EV_JUNK, fl_rx_next(&s_rx, far));
    TEST_ASSERT_EQUAL(FL_EV_ACK, fl_rx_next(&s_rx, far));
    fl_rx_push(&s_rx, (const uint8_t *)"Cx\r\n", 4);
    fl_rx_drop_stale(&s_rx);
    TEST_ASSERT_EQUAL(FL_EV_TEXT, fl_rx_next(&s_rx, far));
    /* 32 mismatch-pattern bytes since the last ACK/NAK. */
    static uint8_t mm[40];
    for (int i = 0; i < 40; i++) {
        mm[i] = (i & 1) ? 0xF8 : 0x80;
    }
    fl_rx_reset_mismatch(&s_rx);
    fl_rx_push(&s_rx, mm, 40);
    fl_ev_t ev;
    int junk = 0;
    while ((ev = fl_rx_next(&s_rx, far)) == FL_EV_JUNK) {
        junk++;
    }
    TEST_ASSERT_EQUAL(FL_EV_MISMATCH, ev);
    TEST_ASSERT_EQUAL_INT(31, junk);
    sim_free(&s_sim);
}

TEST_CASE("rx window: a string split across reads is found once, with its stream offset", "[xmodem]")
{
    static fl_win_t w;
    fl_win_init(&w);
    int p = fl_win_watch(&w, "boot>");
    fl_win_append(&w, (const uint8_t *)"xxbo", 4);
    TEST_ASSERT_FALSE(fl_win_seen(&w, p));
    uint64_t mark = w.total;
    fl_win_append(&w, (const uint8_t *)"ot> ", 4);
    TEST_ASSERT_TRUE(fl_win_seen(&w, p));
    TEST_ASSERT_EQUAL_UINT64(7, w.w[p].first_end);
    TEST_ASSERT_TRUE(fl_win_seen_after(&w, p, mark));
    TEST_ASSERT_FALSE(fl_win_seen_after(&w, p, 7));
    /* NUL bytes do not hide a match; long input slides the window. */
    static uint8_t junk[3000];
    memset(junk, 0, sizeof(junk));
    fl_win_append(&w, junk, sizeof(junk));
    fl_win_append(&w, (const uint8_t *)"\0boot>", 6);
    TEST_ASSERT_TRUE(fl_win_seen_after(&w, p, 3000));
    TEST_ASSERT_EQUAL_UINT64(7, w.w[p].first_end);
}
