/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <string.h>

#include "gnss_link_core.h"
#include "unity.h"

/* Clock near the 32-bit millisecond wrap, to show nothing depends on it. */
#define T0_US ((int64_t)UINT32_MAX * 1000 - 5000000)

#define RING 32768u   /* driver RX ring (CONFIG_GNSS_LINK_RX_BUF_SIZE default) */

static void open_link(gnss_link_core_t *c)
{
    gnss_link_core_init(c, 115200);
}

TEST_CASE("link core: a fresh link streams, counts nothing and has seen nothing", "[gnss_link]")
{
    gnss_link_core_t c;
    open_link(&c);
    gnss_link_stats_t s;
    gnss_link_core_snapshot(&c, T0_US, &s);
    TEST_ASSERT_EQUAL(GNSS_LINK_MODE_STREAM, s.mode);
    TEST_ASSERT_EQUAL_UINT32(115200, s.baud);
    TEST_ASSERT_EQUAL_UINT64(0, s.rx_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, s.tx_bytes);
    TEST_ASSERT_EQUAL_UINT32(0, s.hw_fifo_overflows);
    TEST_ASSERT_EQUAL_UINT32(0, s.rx_buffer_full);
    TEST_ASSERT_EQUAL_UINT32(0, s.line_errors);
    TEST_ASSERT_EQUAL_UINT64(0, s.rtcm_held);
    TEST_ASSERT_EQUAL_UINT64(0, s.flushed_bytes);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s.last_rx_age_ms);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s.last_rtcm_age_ms);

    /* A link that was never opened refuses everything. */
    gnss_link_core_t off;
    memset(&off, 0, sizeof(off));
    TEST_ASSERT_EQUAL(GNSS_LINK_MODE_OFF, off.mode);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&off, GNSS_LINK_OP_WRITE, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&off, GNSS_LINK_OP_RESYNC, 0));
    uint32_t tok = 99;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_acquire(&off, GNSS_LINK_MODE_COMMAND, "x", &tok));
    TEST_ASSERT_EQUAL_UINT32(0, tok);
}

TEST_CASE("link core: one lease at a time, only COMMAND or RAW", "[gnss_link]")
{
    gnss_link_core_t c;
    open_link(&c);
    uint32_t tok = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, gnss_link_core_acquire(&c, GNSS_LINK_MODE_STREAM, "x", &tok));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, gnss_link_core_acquire(&c, GNSS_LINK_MODE_OFF, "x", &tok));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, gnss_link_core_acquire(&c, GNSS_LINK_MODE_COMMAND, "x", NULL));
    TEST_ASSERT_EQUAL(GNSS_LINK_MODE_STREAM, c.mode);

    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_acquire(&c, GNSS_LINK_MODE_COMMAND, "detect", &tok));
    TEST_ASSERT_NOT_EQUAL(0, tok);
    TEST_ASSERT_EQUAL(GNSS_LINK_MODE_COMMAND, c.mode);
    TEST_ASSERT_EQUAL_STRING("detect", c.owner);

    uint32_t other = 7;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_acquire(&c, GNSS_LINK_MODE_RAW, "flash", &other));
    TEST_ASSERT_EQUAL_UINT32(0, other);
    TEST_ASSERT_EQUAL(GNSS_LINK_MODE_COMMAND, c.mode);

    gnss_link_mode_t was = GNSS_LINK_MODE_OFF;
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_release(&c, tok, &was));
    TEST_ASSERT_EQUAL(GNSS_LINK_MODE_COMMAND, was);
    TEST_ASSERT_EQUAL(GNSS_LINK_MODE_STREAM, c.mode);
    TEST_ASSERT_NULL(c.owner);
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_acquire(&c, GNSS_LINK_MODE_RAW, "flash", &other));
    TEST_ASSERT_EQUAL(GNSS_LINK_MODE_RAW, c.mode);
}

TEST_CASE("link core: a stale lease never matches again", "[gnss_link]")
{
    gnss_link_core_t c;
    open_link(&c);
    uint32_t first = 0;
    uint32_t second = 0;
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_acquire(&c, GNSS_LINK_MODE_COMMAND, "a", &first));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_release(&c, first, NULL));

    /* Released: the old token is refused everywhere, and a second release is too. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_release(&c, first, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_WRITE, first));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_SET_BAUD, first));

    /* Same mode taken again: a new token, and the old one still does not match. */
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_acquire(&c, GNSS_LINK_MODE_COMMAND, "b", &second));
    TEST_ASSERT_NOT_EQUAL(first, second);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_WRITE, first));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_release(&c, first, NULL));
    TEST_ASSERT_EQUAL(GNSS_LINK_MODE_COMMAND, c.mode);
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_check(&c, GNSS_LINK_OP_WRITE, second));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_release(&c, second, NULL));

    /* The generation wraps past 0 ("no lease") without issuing it. */
    c.last_token = UINT32_MAX;
    uint32_t wrapped = 0;
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_acquire(&c, GNSS_LINK_MODE_RAW, "c", &wrapped));
    TEST_ASSERT_EQUAL_UINT32(1, wrapped);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_release(&c, 0, NULL));
}

TEST_CASE("link core: who may do what in each mode", "[gnss_link]")
{
    gnss_link_core_t c;
    open_link(&c);
    const uint32_t bogus = 12345;

    /* STREAM: NULL-lease writes and the lease-free recovery steps only. */
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_check(&c, GNSS_LINK_OP_WRITE, 0));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_check(&c, GNSS_LINK_OP_RESYNC, 0));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_check(&c, GNSS_LINK_OP_RESTART, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_SET_BAUD, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_BREAK, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_PROMPT, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_RAW_IO, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_WRITE, bogus));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_RELEASE, 0));

    /* COMMAND: everything with the lease, nothing without it, no raw I/O. */
    uint32_t cmd = 0;
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_acquire(&c, GNSS_LINK_MODE_COMMAND, "cfg", &cmd));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_check(&c, GNSS_LINK_OP_WRITE, cmd));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_check(&c, GNSS_LINK_OP_SET_BAUD, cmd));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_check(&c, GNSS_LINK_OP_BREAK, cmd));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_check(&c, GNSS_LINK_OP_PROMPT, cmd));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_check(&c, GNSS_LINK_OP_RESYNC, cmd));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_check(&c, GNSS_LINK_OP_RESTART, cmd));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_RAW_IO, cmd));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_WRITE, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_RESYNC, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_RESTART, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_WRITE, bogus));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_release(&c, cmd, NULL));

    /* RAW: raw I/O, writes, baud and break with the lease; no prompt
     * detector (the framer is not fed) and no resync or reinstall. */
    uint32_t raw = 0;
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_acquire(&c, GNSS_LINK_MODE_RAW, "flash", &raw));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_check(&c, GNSS_LINK_OP_RAW_IO, raw));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_check(&c, GNSS_LINK_OP_WRITE, raw));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_check(&c, GNSS_LINK_OP_SET_BAUD, raw));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_check(&c, GNSS_LINK_OP_BREAK, raw));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_PROMPT, raw));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_RESYNC, raw));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_RESTART, raw));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_WRITE, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_RESYNC, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_RAW_IO, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_set_prompt(&c, raw, true));
    TEST_ASSERT_FALSE(c.prompt_armed);
}

TEST_CASE("link core: the prompt detector needs the COMMAND lease and the release disarms it", "[gnss_link]")
{
    gnss_link_core_t c;
    open_link(&c);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_set_prompt(&c, 0, true));
    uint32_t cmd = 0;
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_acquire(&c, GNSS_LINK_MODE_COMMAND, "detect", &cmd));

    /* Not armed yet: a prompt the framer might report goes nowhere. */
    TEST_ASSERT_EQUAL(GNSS_LINK_ROUTE_DROP, gnss_link_core_route(&c, GNSS_FRAME_PROMPT, T0_US));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_set_prompt(&c, cmd, true));
    TEST_ASSERT_TRUE(c.prompt_armed);
    TEST_ASSERT_EQUAL(GNSS_LINK_ROUTE_PROMPT, gnss_link_core_route(&c, GNSS_FRAME_PROMPT, T0_US));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_set_prompt(&c, cmd, false));
    TEST_ASSERT_EQUAL(GNSS_LINK_ROUTE_DROP, gnss_link_core_route(&c, GNSS_FRAME_PROMPT, T0_US));

    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_set_prompt(&c, cmd, true));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_release(&c, cmd, NULL));
    TEST_ASSERT_FALSE(c.prompt_armed);
    TEST_ASSERT_EQUAL(GNSS_LINK_ROUTE_DROP, gnss_link_core_route(&c, GNSS_FRAME_PROMPT, T0_US));

    /* A new lease starts disarmed. */
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_acquire(&c, GNSS_LINK_MODE_COMMAND, "detect", &cmd));
    TEST_ASSERT_FALSE(c.prompt_armed);
}

TEST_CASE("link core: RTCM is published only in STREAM and counted as held under a lease", "[gnss_link]")
{
    gnss_link_core_t c;
    open_link(&c);
    gnss_link_stats_t s;

    TEST_ASSERT_EQUAL(GNSS_LINK_ROUTE_PUBLISH, gnss_link_core_route(&c, GNSS_FRAME_RTCM3, T0_US));
    TEST_ASSERT_EQUAL(GNSS_LINK_ROUTE_LINE, gnss_link_core_route(&c, GNSS_FRAME_TEXT, T0_US));
    gnss_link_core_snapshot(&c, T0_US + 250000, &s);
    TEST_ASSERT_EQUAL_UINT64(0, s.rtcm_held);
    TEST_ASSERT_EQUAL_UINT32(250, s.last_rtcm_age_ms);

    uint32_t cmd = 0;
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_acquire(&c, GNSS_LINK_MODE_COMMAND, "cfg", &cmd));
    TEST_ASSERT_EQUAL(GNSS_LINK_ROUTE_HOLD, gnss_link_core_route(&c, GNSS_FRAME_RTCM3, T0_US + 1000000));
    TEST_ASSERT_EQUAL(GNSS_LINK_ROUTE_HOLD, gnss_link_core_route(&c, GNSS_FRAME_RTCM3, T0_US + 2000000));
    /* Replies keep flowing to the command layer during the session. */
    TEST_ASSERT_EQUAL(GNSS_LINK_ROUTE_LINE, gnss_link_core_route(&c, GNSS_FRAME_TEXT, T0_US + 2000000));
    gnss_link_core_snapshot(&c, T0_US + 2000000, &s);
    TEST_ASSERT_EQUAL_UINT64(2, s.rtcm_held);
    TEST_ASSERT_EQUAL_UINT32(0, s.last_rtcm_age_ms);   /* held frames were still received */
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_release(&c, cmd, NULL));

    TEST_ASSERT_EQUAL(GNSS_LINK_ROUTE_PUBLISH, gnss_link_core_route(&c, GNSS_FRAME_RTCM3, T0_US + 3000000));
    uint32_t raw = 0;
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_acquire(&c, GNSS_LINK_MODE_RAW, "flash", &raw));
    TEST_ASSERT_EQUAL(GNSS_LINK_ROUTE_HOLD, gnss_link_core_route(&c, GNSS_FRAME_RTCM3, T0_US + 4000000));
    TEST_ASSERT_EQUAL(GNSS_LINK_ROUTE_DROP, gnss_link_core_route(&c, GNSS_FRAME_TEXT, T0_US + 4000000));
    gnss_link_core_snapshot(&c, T0_US + 4000000, &s);
    TEST_ASSERT_EQUAL_UINT64(3, s.rtcm_held);
}

TEST_CASE("link core: UART conditions count events and only overflow/full reset the framer", "[gnss_link]")
{
    gnss_link_core_t c;
    open_link(&c);
    TEST_ASSERT_TRUE(gnss_link_core_on_event(&c, GNSS_LINK_EV_FIFO_OVF));
    TEST_ASSERT_TRUE(gnss_link_core_on_event(&c, GNSS_LINK_EV_FIFO_OVF));
    TEST_ASSERT_TRUE(gnss_link_core_on_event(&c, GNSS_LINK_EV_BUFFER_FULL));
    TEST_ASSERT_FALSE(gnss_link_core_on_event(&c, GNSS_LINK_EV_FRAME_ERR));
    TEST_ASSERT_FALSE(gnss_link_core_on_event(&c, GNSS_LINK_EV_PARITY_ERR));
    TEST_ASSERT_FALSE(gnss_link_core_on_event(&c, GNSS_LINK_EV_BREAK));
    TEST_ASSERT_FALSE(gnss_link_core_on_event(&c, GNSS_LINK_EV_DATA));
    TEST_ASSERT_FALSE(gnss_link_core_on_event(&c, GNSS_LINK_EV_OTHER));

    gnss_link_stats_t s;
    gnss_link_core_snapshot(&c, T0_US, &s);
    TEST_ASSERT_EQUAL_UINT32(2, s.hw_fifo_overflows);
    TEST_ASSERT_EQUAL_UINT32(1, s.rx_buffer_full);
    TEST_ASSERT_EQUAL_UINT32(3, s.line_errors);
    /* No byte counter moves on an event: the bytes an overflow loses are unknown. */
    TEST_ASSERT_EQUAL_UINT64(0, s.rx_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, s.flushed_bytes);

    /* Events counted while a RAW lease was held land in the same counters
     * (the ring was read below its full mark in between: a new episode). */
    gnss_link_core_on_level(&c, 0, RING);
    uint32_t raw = 0;
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_acquire(&c, GNSS_LINK_MODE_RAW, "flash", &raw));
    (void)gnss_link_core_on_event(&c, GNSS_LINK_EV_BUFFER_FULL);
    (void)gnss_link_core_on_event(&c, GNSS_LINK_EV_FIFO_OVF);
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_release(&c, raw, NULL));
    gnss_link_core_snapshot(&c, T0_US, &s);
    TEST_ASSERT_EQUAL_UINT32(3, s.hw_fifo_overflows);
    TEST_ASSERT_EQUAL_UINT32(2, s.rx_buffer_full);
}

TEST_CASE("link core: a reader stall with a saturated event queue still counts the loss", "[gnss_link]")
{
    /* The RX task stalls 5 s at 115200: 32 data events fill the queue after
     * ~330 ms, the 32 KB ring is full after ~2.8 s, and the driver drops the
     * buffer-full event and every overflow event after it. On resume the
     * reader finds a full queue and a full ring. */
    gnss_link_core_t c;
    open_link(&c);
    for (unsigned i = 0; i < GNSS_LINK_EVENT_QUEUE_LEN; i++) {
        TEST_ASSERT_FALSE(gnss_link_core_on_event(&c, GNSS_LINK_EV_DATA));   /* the queued events */
    }
    TEST_ASSERT_TRUE(gnss_link_core_ring_full(RING - GNSS_LINK_HW_FIFO_LEN, RING));
    TEST_ASSERT_FALSE(gnss_link_core_ring_full(RING - GNSS_LINK_HW_FIFO_LEN - 1, RING));
    TEST_ASSERT_TRUE(gnss_link_core_on_queue_full(&c, RING - 40, RING, true));
    gnss_link_stats_t s;
    gnss_link_core_snapshot(&c, T0_US, &s);
    TEST_ASSERT_EQUAL_UINT32(1, s.rx_events_dropped);
    TEST_ASSERT_EQUAL_UINT32(1, s.rx_buffer_full);
    TEST_ASSERT_EQUAL_UINT32(1, s.hw_fifo_overflows);   /* the alert input moves */

    /* The same episode is counted once, however often the queue is found full. */
    gnss_link_core_on_level(&c, RING - 40, RING);
    TEST_ASSERT_FALSE(gnss_link_core_on_queue_full(&c, RING - 40, RING, true));
    /* A late buffer-full event of this episode is not counted again. */
    TEST_ASSERT_FALSE(gnss_link_core_on_event(&c, GNSS_LINK_EV_BUFFER_FULL));
    gnss_link_core_snapshot(&c, T0_US, &s);
    TEST_ASSERT_EQUAL_UINT32(1, s.rx_events_dropped);
    TEST_ASSERT_EQUAL_UINT32(1, s.rx_buffer_full);
    TEST_ASSERT_EQUAL_UINT32(1, s.hw_fifo_overflows);

    /* Drained, queue with room again: a shorter stall (queue full, ring not)
     * counts only the dropped events and resets the framer. */
    gnss_link_core_on_level(&c, 0, RING);
    gnss_link_core_on_queue_ok(&c);
    TEST_ASSERT_TRUE(gnss_link_core_on_queue_full(&c, 12000, RING, true));
    gnss_link_core_snapshot(&c, T0_US, &s);
    TEST_ASSERT_EQUAL_UINT32(2, s.rx_events_dropped);
    TEST_ASSERT_EQUAL_UINT32(1, s.rx_buffer_full);
    TEST_ASSERT_EQUAL_UINT32(1, s.hw_fifo_overflows);

    /* Buffer-full and overflow events that did fit are not counted twice. */
    gnss_link_core_on_level(&c, 0, RING);
    gnss_link_core_on_queue_ok(&c);
    TEST_ASSERT_TRUE(gnss_link_core_on_event(&c, GNSS_LINK_EV_BUFFER_FULL));
    TEST_ASSERT_TRUE(gnss_link_core_on_event(&c, GNSS_LINK_EV_FIFO_OVF));
    TEST_ASSERT_TRUE(gnss_link_core_on_queue_full(&c, RING, RING, true));   /* only the dropped events */
    gnss_link_core_snapshot(&c, T0_US, &s);
    TEST_ASSERT_EQUAL_UINT32(3, s.rx_events_dropped);
    TEST_ASSERT_EQUAL_UINT32(2, s.rx_buffer_full);
    TEST_ASSERT_EQUAL_UINT32(2, s.hw_fifo_overflows);

    /* RAW: nobody takes events, so a full queue is normal and not counted;
     * a stalled lease holder still shows as a full ring with lost bytes. */
    gnss_link_core_on_level(&c, 0, RING);
    uint32_t raw = 0;
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_acquire(&c, GNSS_LINK_MODE_RAW, "flash", &raw));
    gnss_link_core_on_queue_full(&c, 100, RING, false);
    gnss_link_core_on_queue_full(&c, RING - 1, RING, false);
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_release(&c, raw, NULL));
    gnss_link_core_snapshot(&c, T0_US, &s);
    TEST_ASSERT_EQUAL_UINT32(3, s.rx_events_dropped);
    TEST_ASSERT_EQUAL_UINT32(3, s.rx_buffer_full);
    TEST_ASSERT_EQUAL_UINT32(3, s.hw_fifo_overflows);
}

TEST_CASE("link core: byte counters, baud switches and flushes", "[gnss_link]")
{
    gnss_link_core_t c;
    open_link(&c);
    gnss_link_core_on_rx(&c, 1024, T0_US);
    gnss_link_core_on_rx(&c, 0, T0_US + 9000000);   /* an empty read is not "bytes arrived" */
    gnss_link_core_on_tx(&c, 13);
    gnss_link_core_on_baud(&c, 460800, 37);
    gnss_link_core_on_baud(&c, 115200, 0);
    gnss_link_core_on_flush(&c, 200);
    gnss_link_core_on_flush(&c, 0);

    gnss_framing_stats_t f;
    memset(&f, 0, sizeof(f));
    f.bytes_in = 1024;
    f.rtcm_frames = 3;
    f.rtcm_crc_errors = 1;
    gnss_link_core_set_framing(&c, &f);

    gnss_link_stats_t s;
    gnss_link_core_snapshot(&c, T0_US + 1500000, &s);
    TEST_ASSERT_EQUAL_UINT64(1024, s.rx_bytes);
    TEST_ASSERT_EQUAL_UINT64(13, s.tx_bytes);
    TEST_ASSERT_EQUAL_UINT64(37, s.baud_switch_bytes);
    TEST_ASSERT_EQUAL_UINT64(200, s.flushed_bytes);
    TEST_ASSERT_EQUAL_UINT32(115200, s.baud);
    TEST_ASSERT_EQUAL_UINT32(1500, s.last_rx_age_ms);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s.last_rtcm_age_ms);
    TEST_ASSERT_EQUAL_UINT64(1024, s.framing.bytes_in);
    TEST_ASSERT_EQUAL_UINT64(3, s.framing.rtcm_frames);
    TEST_ASSERT_EQUAL_UINT64(1, s.framing.rtcm_crc_errors);

    /* Byte counters are 64-bit: they run past 4 GiB without wrapping. */
    for (int i = 0; i < 5; i++) {
        gnss_link_core_on_rx(&c, (size_t)UINT32_MAX, T0_US);
    }
    gnss_link_core_snapshot(&c, T0_US, &s);
    TEST_ASSERT_EQUAL_UINT64(1024 + 5 * (uint64_t)UINT32_MAX, s.rx_bytes);
}

TEST_CASE("link core: ages come from 64-bit stamps and are clamped", "[gnss_link]")
{
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, gnss_link_core_age_ms(false, 0, T0_US));
    TEST_ASSERT_EQUAL_UINT32(0, gnss_link_core_age_ms(true, T0_US, T0_US));
    TEST_ASSERT_EQUAL_UINT32(0, gnss_link_core_age_ms(true, T0_US + 5, T0_US));   /* never negative */
    TEST_ASSERT_EQUAL_UINT32(0, gnss_link_core_age_ms(true, T0_US, T0_US + 999));
    TEST_ASSERT_EQUAL_UINT32(1, gnss_link_core_age_ms(true, T0_US, T0_US + 1000));

    /* Across the 32-bit millisecond wrap (~49.7 days) the age stays right. */
    int64_t stamp = (int64_t)UINT32_MAX * 1000 - 2000000;
    TEST_ASSERT_EQUAL_UINT32(4000, gnss_link_core_age_ms(true, stamp, stamp + 4000000));

    /* A multi-year gap is clamped below the "never" value. */
    int64_t years = (int64_t)200 * 24 * 3600 * 1000000;
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX - 1, gnss_link_core_age_ms(true, 0, years));
}

TEST_CASE("link core: drain passes read 1 KB chunks, at most 16 KB, and say how long to wait", "[gnss_link]")
{
    gnss_link_pass_t p;

    /* Nothing buffered: the pass ends at once and the next wait is 20 ms. */
    gnss_link_pass_begin(&p);
    TEST_ASSERT_EQUAL_size_t(0, gnss_link_pass_next(&p, 0));
    TEST_ASSERT_FALSE(p.capped);
    TEST_ASSERT_EQUAL_UINT32(GNSS_LINK_EVENT_WAIT_MS, gnss_link_pass_wait_ms(&p));

    /* A small amount is read in one go, whatever any event said. */
    gnss_link_pass_begin(&p);
    TEST_ASSERT_EQUAL_size_t(300, gnss_link_pass_next(&p, 300));
    gnss_link_pass_add(&p, 300);
    TEST_ASSERT_EQUAL_size_t(0, gnss_link_pass_next(&p, 0));
    TEST_ASSERT_EQUAL_UINT32(GNSS_LINK_EVENT_WAIT_MS, gnss_link_pass_wait_ms(&p));

    /* A 32 KB backlog: 16 reads of 1 KB, then the cap with data left -> no wait. */
    gnss_link_pass_begin(&p);
    size_t buffered = 32 * 1024;
    unsigned reads = 0;
    for (;;) {
        size_t n = gnss_link_pass_next(&p, buffered);
        if (n == 0) {
            break;
        }
        TEST_ASSERT_LESS_OR_EQUAL_size_t(GNSS_LINK_READ_CHUNK, n);
        gnss_link_pass_add(&p, n);
        buffered -= n;
        reads++;
    }
    TEST_ASSERT_EQUAL_UINT(16, reads);
    TEST_ASSERT_EQUAL_size_t(GNSS_LINK_DRAIN_CAP, p.done);
    TEST_ASSERT_TRUE(p.capped);
    TEST_ASSERT_EQUAL_UINT32(0, gnss_link_pass_wait_ms(&p));

    /* The next pass takes the rest and ends with the buffer empty -> 20 ms. */
    gnss_link_pass_begin(&p);
    while (buffered > 0) {
        size_t n = gnss_link_pass_next(&p, buffered);
        TEST_ASSERT_NOT_EQUAL(0, n);
        gnss_link_pass_add(&p, n);
        buffered -= n;
    }
    TEST_ASSERT_EQUAL_size_t(0, gnss_link_pass_next(&p, 0));
    TEST_ASSERT_FALSE(p.capped);
    TEST_ASSERT_EQUAL_UINT32(GNSS_LINK_EVENT_WAIT_MS, gnss_link_pass_wait_ms(&p));

    /* Exactly 16 KB that empties the buffer is not a capped pass. */
    gnss_link_pass_begin(&p);
    buffered = GNSS_LINK_DRAIN_CAP;
    while (buffered > 0) {
        size_t n = gnss_link_pass_next(&p, buffered);
        gnss_link_pass_add(&p, n);
        buffered -= n;
    }
    TEST_ASSERT_EQUAL_size_t(0, gnss_link_pass_next(&p, 0));
    TEST_ASSERT_FALSE(p.capped);

    /* Short reads (the driver returned less) never overshoot the cap. */
    gnss_link_pass_begin(&p);
    gnss_link_pass_add(&p, GNSS_LINK_DRAIN_CAP - 100);
    TEST_ASSERT_EQUAL_size_t(100, gnss_link_pass_next(&p, 5000));
    gnss_link_pass_add(&p, 100);
    TEST_ASSERT_EQUAL_size_t(0, gnss_link_pass_next(&p, 4900));
    TEST_ASSERT_TRUE(p.capped);
}

TEST_CASE("link core: open baud, receiver rates, thresholds and break length", "[gnss_link]")
{
    static const uint32_t ok[] = { 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600 };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        TEST_ASSERT_TRUE(gnss_link_core_baud_supported(ok[i]));
        /* v1 keeps the driver defaults at every rate. */
        TEST_ASSERT_EQUAL_INT(GNSS_LINK_RX_FULL_THRESH_DEFAULT, gnss_link_core_rx_full_threshold(ok[i]));
        TEST_ASSERT_EQUAL_UINT8(GNSS_LINK_RX_TIMEOUT_DEFAULT, gnss_link_core_rx_timeout(ok[i]));
    }
    TEST_ASSERT_FALSE(gnss_link_core_baud_supported(0));
    TEST_ASSERT_FALSE(gnss_link_core_baud_supported(4800));
    TEST_ASSERT_FALSE(gnss_link_core_baud_supported(115201));
    TEST_ASSERT_FALSE(gnss_link_core_baud_supported(1843200));

    TEST_ASSERT_EQUAL_UINT32(460800, gnss_link_core_open_baud(ESP_OK, 460800, 115200));
    TEST_ASSERT_EQUAL_UINT32(115200, gnss_link_core_open_baud(ESP_ERR_NOT_FOUND, 460800, 115200));
    TEST_ASSERT_EQUAL_UINT32(115200, gnss_link_core_open_baud(ESP_OK, 12345, 115200));
    TEST_ASSERT_EQUAL_UINT32(230400, gnss_link_core_open_baud(ESP_FAIL, 0, 230400));
    TEST_ASSERT_EQUAL_UINT32(115200, gnss_link_core_open_baud(ESP_FAIL, 0, 100000));

    TEST_ASSERT_FALSE(gnss_link_core_break_ok(0));
    TEST_ASSERT_TRUE(gnss_link_core_break_ok(1));
    TEST_ASSERT_TRUE(gnss_link_core_break_ok(100));
    TEST_ASSERT_TRUE(gnss_link_core_break_ok(GNSS_LINK_BREAK_MAX_MS));
    TEST_ASSERT_FALSE(gnss_link_core_break_ok(GNSS_LINK_BREAK_MAX_MS + 1));
}

TEST_CASE("link core: hands off refuses every write and every lease, reception goes on", "[gnss_link]")
{
    gnss_link_core_t c;
    open_link(&c);
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_set_hands_off(&c, true));
    gnss_link_stats_t s;
    gnss_link_core_snapshot(&c, T0_US, &s);
    TEST_ASSERT_TRUE(s.hands_off);

    /* Nothing may reach the receiver: no lease-free query, no lease. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_WRITE, 0));
    uint32_t tok = 99;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_acquire(&c, GNSS_LINK_MODE_COMMAND, "detect", &tok));
    TEST_ASSERT_EQUAL_UINT32(0, tok);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_acquire(&c, GNSS_LINK_MODE_RAW, "flash", &tok));
    TEST_ASSERT_EQUAL(GNSS_LINK_MODE_STREAM, c.mode);
    /* The stream is still published. */
    TEST_ASSERT_EQUAL(GNSS_LINK_ROUTE_PUBLISH, gnss_link_core_route(&c, GNSS_FRAME_RTCM3, T0_US));
    TEST_ASSERT_EQUAL(GNSS_LINK_ROUTE_LINE, gnss_link_core_route(&c, GNSS_FRAME_TEXT, T0_US));

    /* Off again: as before. */
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_set_hands_off(&c, false));
    gnss_link_core_snapshot(&c, T0_US, &s);
    TEST_ASSERT_FALSE(s.hands_off);
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_check(&c, GNSS_LINK_OP_WRITE, 0));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_acquire(&c, GNSS_LINK_MODE_COMMAND, "detect", &tok));

    /* Not while a lease is held (its holder would be cut off mid-session). */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_set_hands_off(&c, true));
    TEST_ASSERT_FALSE(c.hands_off);
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_check(&c, GNSS_LINK_OP_WRITE, tok));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_release(&c, tok, NULL));

    /* Even with a lease that slipped through, writes and breaks are refused. */
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_set_hands_off(&c, true));
    c.lease = 7;
    c.mode = GNSS_LINK_MODE_COMMAND;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_BREAK, 7));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, gnss_link_core_check(&c, GNSS_LINK_OP_WRITE, 7));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_release(&c, 7, NULL));
}

TEST_CASE("link core: a closed driver reopens after 1 s, 5 s, 30 s, then every 60 s", "[gnss_link]")
{
    static const uint32_t want[] = { 1000, 5000, 30000, 60000, 60000, 60000 };
    for (uint32_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        TEST_ASSERT_EQUAL_UINT32(want[i], gnss_link_core_reopen_delay_ms(i));
    }
    TEST_ASSERT_EQUAL_UINT32(60000, gnss_link_core_reopen_delay_ms(UINT32_MAX));

    /* The schedule as the RX task runs it: the closing failure waits
     * delay(0); each failed attempt then waits next(failures before it). */
    static const uint32_t gaps[] = { 1000, 5000, 30000, 60000, 60000 };
    uint32_t failures = 0;
    uint32_t gap = gnss_link_core_reopen_delay_ms(0);
    for (uint32_t i = 0; i < sizeof(gaps) / sizeof(gaps[0]); i++) {
        TEST_ASSERT_EQUAL_UINT32(gaps[i], gap);
        gap = gnss_link_core_reopen_next_ms(failures);   /* this attempt failed */
        failures++;
    }
    TEST_ASSERT_EQUAL_UINT32(60000, gnss_link_core_reopen_next_ms(UINT32_MAX));

    /* the first failure is logged, then at most one line per 10 min */
    TEST_ASSERT_TRUE(gnss_link_core_reopen_log_due(false, 0, T0_US));
    TEST_ASSERT_FALSE(gnss_link_core_reopen_log_due(true, T0_US, T0_US + 599LL * 1000000));
    TEST_ASSERT_TRUE(gnss_link_core_reopen_log_due(true, T0_US, T0_US + 600LL * 1000000));
}

TEST_CASE("link core: link_open and open_failures show a closed driver", "[gnss_link]")
{
    gnss_link_core_t c;
    memset(&c, 0, sizeof(c));
    gnss_link_stats_t st;
    gnss_link_core_snapshot(&c, T0_US, &st);
    TEST_ASSERT_FALSE(st.link_open);   /* not initialised: OFF */

    open_link(&c);
    gnss_link_core_snapshot(&c, T0_US, &st);
    TEST_ASSERT_TRUE(st.link_open);
    TEST_ASSERT_EQUAL_UINT32(0, st.open_failures);

    gnss_link_core_on_open_failed(&c);
    gnss_link_core_on_open_failed(&c);
    gnss_link_core_snapshot(&c, T0_US, &st);
    TEST_ASSERT_FALSE(st.link_open);
    TEST_ASSERT_EQUAL_UINT32(2, st.open_failures);
    TEST_ASSERT_EQUAL(GNSS_LINK_MODE_STREAM, st.mode);   /* still STREAM: the reopen keeps the mode */

    gnss_link_core_set_open(&c, true);
    gnss_link_core_snapshot(&c, T0_US, &st);
    TEST_ASSERT_TRUE(st.link_open);
    TEST_ASSERT_EQUAL_UINT32(2, st.open_failures);   /* since boot */
}

/* ---- bench hooks: the report-only prompt observer --------------------------------------- */

#include "gnss_framing.h"
#include "rtcm_vectors.h"

/* A framer wired to the core as gnss_link.c wires it: the detector armed as
 * the core says, every chunk shown to the observer, units routed. */
typedef struct {
    gnss_link_core_t c;
    gnss_framer_t f;
    unsigned routed[GNSS_LINK_ROUTE_OBSERVE + 1];
} obs_rig_t;

static obs_rig_t s_obs;

static void obs_on_frame(const gnss_frame_t *fr, void *ctx)
{
    obs_rig_t *r = ctx;
    gnss_link_route_t route = gnss_link_core_route(&r->c, fr->kind, T0_US);
    if (route == GNSS_LINK_ROUTE_OBSERVE) {
        gnss_link_core_observe_hit(&r->c, fr->at);
    }
    r->routed[route]++;
}

static void obs_rig_init(obs_rig_t *r)
{
    memset(r, 0, sizeof(*r));
    open_link(&r->c);
    gnss_framer_init(&r->f, obs_on_frame, r);
}

static void obs_sync_detector(obs_rig_t *r)
{
    gnss_framer_set_prompt_detect(&r->f, gnss_link_core_detector_on(&r->c));
}

static void obs_feed(obs_rig_t *r, const void *data, size_t len)
{
    gnss_framing_stats_t st;
    gnss_framer_get_stats(&r->f, &st);
    gnss_link_core_observe_feed(&r->c, data, len, st.bytes_in);
    gnss_framer_feed(&r->f, data, len);
    gnss_link_core_observe_fed(&r->c);
}

TEST_CASE("link core: the observer counts a loader prompt in STREAM and never routes it to the prompt sink",
          "[gnss_link]")
{
    obs_rig_t *r = &s_obs;
    obs_rig_init(r);
    TEST_ASSERT_FALSE(gnss_link_core_detector_on(&r->c));
    gnss_link_core_set_observe(&r->c, true);
    TEST_ASSERT_TRUE(gnss_link_core_detector_on(&r->c));
    obs_sync_detector(r);

    obs_feed(r, "noise before the loader ", 24);
    obs_feed(r, "\r\nboot>", 7);
    TEST_ASSERT_EQUAL_UINT(1, r->routed[GNSS_LINK_ROUTE_OBSERVE]);
    TEST_ASSERT_EQUAL_UINT(0, r->routed[GNSS_LINK_ROUTE_PROMPT]);
    gnss_link_prompt_obs_t o;
    gnss_link_core_observe_get(&r->c, &o);
    TEST_ASSERT_TRUE(o.on);
    TEST_ASSERT_EQUAL_UINT32(1, o.hits);
    TEST_ASSERT_EQUAL_INT64(T0_US / 1000, o.last_hit_ms);
    /* The 32 bytes ending at the prompt's last byte, across the two chunks. */
    const char *want = "e before the loader \r\nboot>";
    const char *tail = "noise before the loader \r\nboot>";
    size_t tl = strlen(tail);
    TEST_ASSERT_EQUAL_UINT8(tl < GNSS_LINK_PROMPT_CTX ? tl : GNSS_LINK_PROMPT_CTX, o.context_len);
    TEST_ASSERT_EQUAL_MEMORY(tail + tl - o.context_len, o.context, o.context_len);
    TEST_ASSERT_EQUAL_MEMORY(want, o.context + o.context_len - strlen(want), strlen(want));

    /* Off: the detector stays off and a prompt goes nowhere. */
    gnss_link_core_set_observe(&r->c, false);
    TEST_ASSERT_FALSE(gnss_link_core_detector_on(&r->c));
    TEST_ASSERT_EQUAL(GNSS_LINK_ROUTE_DROP, gnss_link_core_route(&r->c, GNSS_FRAME_PROMPT, T0_US));
    gnss_link_core_observe_get(&r->c, &o);
    TEST_ASSERT_FALSE(o.on);
    TEST_ASSERT_EQUAL_UINT32(1, o.hits);   /* counters are kept */
}

TEST_CASE("link core: an RTCM frame within the window suppresses an observed prompt", "[gnss_link]")
{
    obs_rig_t *r = &s_obs;
    obs_rig_init(r);
    gnss_link_core_set_observe(&r->c, true);
    obs_sync_detector(r);

    /* "boot>" right after a CRC-valid frame: five bytes in a stream match by chance. */
    uint8_t buf[sizeof(k_v1005) + 5];
    memcpy(buf, k_v1005, sizeof(k_v1005));
    memcpy(buf + sizeof(k_v1005), "boot>", 5);
    obs_feed(r, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(1, r->routed[GNSS_LINK_ROUTE_PUBLISH]);
    TEST_ASSERT_EQUAL_UINT(0, r->routed[GNSS_LINK_ROUTE_OBSERVE]);

    /* ... and before it, a frame that follows within the window. */
    uint8_t buf2[5 + sizeof(k_v1005)];
    memcpy(buf2, "boot>", 5);
    memcpy(buf2 + 5, k_v1005, sizeof(k_v1005));
    uint8_t gap[600];
    memset(gap, 'x', sizeof(gap));   /* past the window of the frame above */
    obs_feed(r, gap, sizeof(gap));
    obs_feed(r, buf2, sizeof(buf2));
    TEST_ASSERT_EQUAL_UINT(0, r->routed[GNSS_LINK_ROUTE_OBSERVE]);
    gnss_link_prompt_obs_t o;
    gnss_link_core_observe_get(&r->c, &o);
    TEST_ASSERT_EQUAL_UINT32(0, o.hits);
    TEST_ASSERT_EQUAL_INT64(-1, o.last_hit_ms);

    /* Far from any frame it counts. */
    obs_feed(r, gap, sizeof(gap));
    obs_feed(r, "boot>", 5);
    TEST_ASSERT_EQUAL_UINT(1, r->routed[GNSS_LINK_ROUTE_OBSERVE]);
}

TEST_CASE("link core: the lease-armed detector is unchanged while the observer runs", "[gnss_link]")
{
    obs_rig_t *r = &s_obs;
    obs_rig_init(r);
    gnss_link_core_set_observe(&r->c, true);
    uint32_t tok = 0;
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_acquire(&r->c, GNSS_LINK_MODE_COMMAND, "detect", &tok));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_set_prompt(&r->c, tok, true));
    obs_sync_detector(r);
    obs_feed(r, "boot>", 5);
    /* The detection's own detector takes it: to the prompt sink, not the observer. */
    TEST_ASSERT_EQUAL_UINT(1, r->routed[GNSS_LINK_ROUTE_PROMPT]);
    TEST_ASSERT_EQUAL_UINT(0, r->routed[GNSS_LINK_ROUTE_OBSERVE]);

    /* Disarmed under the same lease (the station's prompt watch): the observer counts it. */
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_set_prompt(&r->c, tok, false));
    TEST_ASSERT_TRUE(gnss_link_core_detector_on(&r->c));
    obs_sync_detector(r);
    obs_feed(r, "\r\nboot>", 7);
    TEST_ASSERT_EQUAL_UINT(1, r->routed[GNSS_LINK_ROUTE_PROMPT]);
    TEST_ASSERT_EQUAL_UINT(1, r->routed[GNSS_LINK_ROUTE_OBSERVE]);

    /* RAW: the framer is not fed; a prompt that reached the core would go nowhere. */
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_release(&r->c, tok, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_acquire(&r->c, GNSS_LINK_MODE_RAW, "flash", &tok));
    TEST_ASSERT_EQUAL(GNSS_LINK_ROUTE_DROP, gnss_link_core_route(&r->c, GNSS_FRAME_PROMPT, T0_US));

    /* Without the observer, lease-armed routing is exactly as before. */
    gnss_link_core_t c;
    open_link(&c);
    TEST_ASSERT_EQUAL(GNSS_LINK_ROUTE_DROP, gnss_link_core_route(&c, GNSS_FRAME_PROMPT, T0_US));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_acquire(&c, GNSS_LINK_MODE_COMMAND, "detect", &tok));
    TEST_ASSERT_EQUAL(GNSS_LINK_ROUTE_DROP, gnss_link_core_route(&c, GNSS_FRAME_PROMPT, T0_US));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_set_prompt(&c, tok, true));
    TEST_ASSERT_TRUE(gnss_link_core_detector_on(&c));
    TEST_ASSERT_EQUAL(GNSS_LINK_ROUTE_PROMPT, gnss_link_core_route(&c, GNSS_FRAME_PROMPT, T0_US));
    TEST_ASSERT_EQUAL(ESP_OK, gnss_link_core_release(&c, tok, NULL));
    TEST_ASSERT_FALSE(gnss_link_core_detector_on(&c));
}

/* ---- bench hooks: the timing summary ---------------------------------------------------- */

static gnss_link_rec_t frame_rec(int64_t t0, uint32_t dt, uint16_t msg, gnss_sys_t sys, uint32_t epoch, bool mmb)
{
    gnss_link_rec_t r;
    memset(&r, 0, sizeof(r));
    r.kind = GNSS_LINK_REC_FRAME;
    r.t0_us = t0;
    r.dt_us = dt;
    r.msg = msg;
    r.bytes = 200;
    if (sys != GNSS_SYS_COUNT) {
        r.flags = (uint8_t)(GNSS_LINK_REC_F_MSM | (mmb ? GNSS_LINK_REC_F_MMB : 0u));
        r.sys = (uint8_t)sys;
        r.epoch = epoch;
    }
    return r;
}

static gnss_link_rec_t pass_rec(int64_t t0, uint32_t dt, uint16_t bytes, bool capped, uint32_t gap)
{
    gnss_link_rec_t r;
    memset(&r, 0, sizeof(r));
    r.kind = GNSS_LINK_REC_PASS;
    r.t0_us = t0;
    r.dt_us = dt;
    r.bytes = bytes;
    r.flags = capped ? GNSS_LINK_REC_F_CAPPED : 0;
    r.wait_ms = capped ? 0 : 20;
    r.gap_us = gap;
    return r;
}

static gnss_link_rec_t s_recs[400];
static int32_t s_scratch[400];

TEST_CASE("link core: timing summary percentiles of read to published", "[gnss_link]")
{
    /* 100 published frames with latencies 1..100 us, one held frame with a huge one. */
    size_t n = 0;
    for (unsigned i = 1; i <= 100; i++) {
        s_recs[n++] = frame_rec(T0_US + i * 1000, i, 1005, GNSS_SYS_COUNT, 0, false);
    }
    gnss_link_rec_t held = frame_rec(T0_US, 999999, 1077, GNSS_SYS_COUNT, 0, false);
    held.flags |= GNSS_LINK_REC_F_HELD;
    s_recs[n++] = held;
    gnss_link_timing_summary_t s;
    gnss_link_timing_summary(s_recs, n, false, false, 0, s_scratch, 400, &s);
    TEST_ASSERT_EQUAL_UINT32(101, s.records);
    TEST_ASSERT_EQUAL_UINT32(101, s.frames);
    TEST_ASSERT_EQUAL_UINT32(100, s.read_to_pub_us.n);
    TEST_ASSERT_EQUAL_INT32(50, s.read_to_pub_us.p50);
    TEST_ASSERT_EQUAL_INT32(95, s.read_to_pub_us.p95);
    TEST_ASSERT_EQUAL_INT32(99, s.read_to_pub_us.p99);
    TEST_ASSERT_EQUAL_INT32(100, s.read_to_pub_us.p999);
    TEST_ASSERT_EQUAL_INT32(100, s.read_to_pub_us.max);
    TEST_ASSERT_FALSE(s.clock);
    for (unsigned k = 0; k < GNSS_SYS_COUNT; k++) {
        TEST_ASSERT_EQUAL_UINT32(0, s.epoch_delay_ms[k].n);   /* no clock: no epoch delays */
    }

    /* Nearest rank on a small set; an empty set is all zero. */
    int32_t v[] = { 30, 10, 20 };
    gnss_link_pct_t p;
    gnss_link_pct(v, 3, &p);
    TEST_ASSERT_EQUAL_INT32(20, p.p50);
    TEST_ASSERT_EQUAL_INT32(30, p.p95);
    TEST_ASSERT_EQUAL_INT32(30, p.max);
    gnss_link_pct(v, 0, &p);
    TEST_ASSERT_EQUAL_UINT32(0, p.n);
    TEST_ASSERT_EQUAL_INT32(0, p.max);
}

TEST_CASE("link core: timing summary of a stall: reader gap, zero-wait passes, first frame", "[gnss_link]")
{
    size_t n = 0;
    int64_t t = T0_US;
    /* Normal: passes every 20 ms with small gaps. */
    for (unsigned i = 0; i < 5; i++) {
        s_recs[n++] = pass_rec(t, 300, 120, false, 2000);
        t += 20000;
    }
    /* A 5 s RX stall, then the ring drains in three capped passes and one that empties it. */
    gnss_link_rec_t st;
    memset(&st, 0, sizeof(st));
    st.kind = GNSS_LINK_REC_STALL;
    st.t0_us = t;
    st.dt_us = 5000000;
    s_recs[n++] = st;
    int64_t end = t + 5000000;
    s_recs[n++] = pass_rec(end + 100, 4000, 16384, true, 5000100);
    s_recs[n++] = frame_rec(end + 2000, 800, 1077, GNSS_SYS_COUNT, 0, false);   /* first frame published */
    s_recs[n++] = pass_rec(end + 4200, 4000, 16384, true, 100);
    s_recs[n++] = pass_rec(end + 8300, 4000, 16384, true, 100);
    s_recs[n++] = pass_rec(end + 12400, 1500, 6000, false, 100);
    s_recs[n++] = pass_rec(end + 40000, 300, 120, false, 20000);
    /* A RAW-read stall counts as a stall but has no drain analysis. */
    gnss_link_rec_t raw = st;
    raw.t0_us = end + 100000;
    raw.dt_us = 2000000;
    raw.flags = GNSS_LINK_REC_F_RAW;
    s_recs[n++] = raw;

    gnss_link_timing_summary_t s;
    gnss_link_timing_summary(s_recs, n, true, false, 0, s_scratch, 400, &s);
    TEST_ASSERT_TRUE(s.full);
    TEST_ASSERT_EQUAL_UINT32(2, s.stalls);
    TEST_ASSERT_EQUAL_UINT32(10, s.passes);
    TEST_ASSERT_EQUAL_UINT32(5000100, s.longest_gap_us);
    TEST_ASSERT_EQUAL_UINT32(3, s.stall_capped_passes);
    TEST_ASSERT_EQUAL_UINT32(12400 + 1500, s.stall_drain_us);
    TEST_ASSERT_TRUE(s.stall_first_frame_valid);
    TEST_ASSERT_EQUAL_UINT32(2000 + 800, s.stall_first_frame_us);
    TEST_ASSERT_EQUAL_UINT32(1, s.frames);
}

TEST_CASE("link core: a background recording that spans a stall fills the after-stall fields", "[gnss_link]")
{
    /* `ftimes 120` runs in the background; 30 s in, `stall 2000` stops the
     * RX task. The records come in the RX task's order: one pass and four
     * frames a second, the stall's record when the stall ends (its begin as
     * t0), then the backlog in capped passes and the frames it held. */
    size_t n = 0;
    int64_t t = T0_US;
    for (unsigned sec = 0; sec < 30; sec++) {
        s_recs[n++] = pass_rec(t, 400, 1600, false, 980000);
        for (unsigned f = 0; f < 4; f++) {
            s_recs[n++] = frame_rec(t + 100 + f * 50, 200, (uint16_t)(1077 + 10 * f), GNSS_SYS_COUNT, 0, false);
        }
        t += 1000000;
    }
    int64_t stall0 = t - 400000;
    gnss_link_rec_t st;
    memset(&st, 0, sizeof(st));
    st.kind = GNSS_LINK_REC_STALL;
    st.t0_us = stall0;
    st.dt_us = 2000000;
    s_recs[n++] = st;
    int64_t end = stall0 + 2000000;
    s_recs[n++] = pass_rec(end + 50, 3000, 4096, true, 2000050);
    s_recs[n++] = frame_rec(end + 900, 250, 1077, GNSS_SYS_COUNT, 0, false);
    s_recs[n++] = pass_rec(end + 3100, 1200, 1500, false, 50);
    t = end + 600000;
    for (unsigned sec = 0; sec < 80 && n + 5 <= 400; sec++) {
        s_recs[n++] = pass_rec(t, 400, 1600, false, 980000);
        for (unsigned f = 0; f < 4; f++) {
            s_recs[n++] = frame_rec(t + 100 + f * 50, 200, (uint16_t)(1077 + 10 * f), GNSS_SYS_COUNT, 0, false);
        }
        t += 1000000;
    }

    gnss_link_timing_summary_t s;
    gnss_link_timing_summary(s_recs, n, false, false, 0, s_scratch, 400, &s);
    TEST_ASSERT_EQUAL_UINT32(1, s.stalls);
    TEST_ASSERT_EQUAL_UINT32(2000050, s.longest_gap_us);
    TEST_ASSERT_EQUAL_UINT32(1, s.stall_capped_passes);
    TEST_ASSERT_EQUAL_UINT32(3100 + 1200, s.stall_drain_us);
    TEST_ASSERT_TRUE(s.stall_first_frame_valid);
    TEST_ASSERT_EQUAL_UINT32(900 + 250, s.stall_first_frame_us);
    TEST_ASSERT_FALSE(s.full);
}

TEST_CASE("link core: timing summary epoch delays per constellation from the set clock", "[gnss_link]")
{
    /* Unix time of esp_timer 0: the clock maps T0_US to GPS week 2400, Monday 10:00:00.000 GPS. */
    const int64_t week2400 = 2400LL * GNSS_RTCM3_WEEK_MS;
    const int64_t tow0 = GNSS_RTCM3_DAY_MS + 10LL * 3600000;   /* Monday 10:00 */
    const int64_t gps0_ms = week2400 + tow0;
    const int64_t unix0_ms = gps0_ms + GNSS_RTCM3_GPS_EPOCH_UNIX_MS - GNSS_RTCM3_GPS_UTC_LEAP_S * 1000;
    const int64_t off_us = unix0_ms * 1000 - T0_US;

    size_t n = 0;
    /* Ten GPS epochs, one a second, each read 120 ms + i ms after its time, two MSMs per epoch (the second
     * one later; only an epoch's first frame counts). */
    for (int i = 0; i < 10; i++) {
        uint32_t epoch = (uint32_t)(tow0 + i * 1000);
        int64_t read = T0_US + (int64_t)i * 1000000 + (120 + i) * 1000;
        s_recs[n++] = frame_rec(read, 300, 1077, GNSS_SYS_GPS, epoch, true);
        s_recs[n++] = frame_rec(read + 40000, 300, 1077, GNSS_SYS_GPS, epoch, false);
        /* BeiDou: the same instants, its epoch field 14 s behind. */
        s_recs[n++] = frame_rec(read + 5000, 300, 1127, GNSS_SYS_BEIDOU, epoch - 14000, false);
    }
    gnss_link_timing_summary_t s;
    gnss_link_timing_summary(s_recs, n, false, true, off_us, s_scratch, 400, &s);
    TEST_ASSERT_TRUE(s.clock);
    TEST_ASSERT_EQUAL_UINT32(10, s.epoch_delay_ms[GNSS_SYS_GPS].n);
    TEST_ASSERT_EQUAL_INT32(124, s.epoch_delay_ms[GNSS_SYS_GPS].p50);
    TEST_ASSERT_EQUAL_INT32(129, s.epoch_delay_ms[GNSS_SYS_GPS].p95);
    TEST_ASSERT_EQUAL_INT32(129, s.epoch_delay_ms[GNSS_SYS_GPS].max);
    TEST_ASSERT_EQUAL_UINT32(10, s.epoch_delay_ms[GNSS_SYS_BEIDOU].n);
    TEST_ASSERT_EQUAL_INT32(129, s.epoch_delay_ms[GNSS_SYS_BEIDOU].p50);
    TEST_ASSERT_EQUAL_INT32(134, s.epoch_delay_ms[GNSS_SYS_BEIDOU].max);
    TEST_ASSERT_EQUAL_UINT32(0, s.epoch_delay_ms[GNSS_SYS_GALILEO].n);
}

TEST_CASE("link core: recorder records as CSV lines", "[gnss_link]")
{
    char line[160];
    gnss_link_rec_t f = frame_rec(1000, 250, 1077, GNSS_SYS_GPS, 270524000u, true);
    TEST_ASSERT_EQUAL_size_t(strlen("F,1000,1250,1077,200,gps,270524000,1,0\n"), gnss_link_rec_csv(&f, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("F,1000,1250,1077,200,gps,270524000,1,0\n", line);
    gnss_link_rec_t h = frame_rec(5, 5, 1005, GNSS_SYS_COUNT, 0, false);
    h.flags |= GNSS_LINK_REC_F_HELD;
    gnss_link_rec_csv(&h, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING("F,5,10,1005,200,,,0,1\n", line);
    gnss_link_rec_t p = pass_rec(100, 50, 16384, true, 7);
    gnss_link_rec_csv(&p, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING("P,100,150,16384,1,0,7\n", line);
    gnss_link_rec_t s;
    memset(&s, 0, sizeof(s));
    s.kind = GNSS_LINK_REC_STALL;
    s.t0_us = 10;
    s.dt_us = 5;
    s.flags = GNSS_LINK_REC_F_RAW;
    gnss_link_rec_csv(&s, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING("S,10,15,raw\n", line);
    /* Too small: nothing, never half a line. */
    TEST_ASSERT_EQUAL_size_t(0, gnss_link_rec_csv(&f, line, 10));
    TEST_ASSERT_EQUAL_STRING("", line);
}
