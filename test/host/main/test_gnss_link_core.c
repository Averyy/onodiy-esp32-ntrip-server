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
