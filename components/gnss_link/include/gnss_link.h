/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * GNSS link: sole owner of the UART wired to the receiver's COM1.
 *
 * Runs the RX task that drains the UART, feeds the framer and hands complete
 * units to the registered sinks (RTCM frames -> hub, text lines -> command
 * layer). Also arbitrates who may use the wire.
 *
 * Ownership modes (exactly one active):
 *   STREAM   default. RX task frames everything. Read-only queries may be
 *            written with lease == NULL (see gnss_link_write); replies arrive
 *            as text lines via the sink.
 *   COMMAND  exclusive command/config session (baud scan, configuration,
 *            receiver restart). RTCM is still framed but the hub is not fed,
 *            so casters see a clean gap instead of half-configured output.
 *   RAW      exclusive byte access for the firmware flasher. The RX task stops
 *            reading; the lease holder uses gnss_link_raw_read().
 *
 * A lease is required for COMMAND and RAW. Leases carry a generation number,
 * so a stale handle (after release or a forced reset) is rejected with
 * ESP_ERR_INVALID_STATE instead of touching the wire.
 *
 * Watchdog: the RX task is subscribed to the task watchdog. While RAW is
 * held it parks, so gnss_link unsubscribes it on entering RAW and
 * resubscribes it on release (the flasher's own task feeds the watchdog
 * between blocks). A RAW lease therefore never trips the watchdog however
 * long the transfer takes.
 *
 * Buffering: the driver RX buffer (CONFIG_GNSS_LINK_RX_BUF_SIZE, internal
 * RAM) must cover the longest time the RX task can be kept off the CPU. With
 * XIP from PSRAM, flash writes no longer stall it; the buffer is sized for
 * the remaining cases (a flash operation while an mmap is open, bursts of
 * higher-priority work) with margin: 32 KB is ~2.8 s at 115200 baud and
 * ~0.7 s at 460800.
 *
 * Time values are ages computed from 64-bit esp_timer timestamps, never
 * differences of wrapping millisecond counters.
 *
 * Writers: only the station supervisor and the command layer it drives write
 * to the wire. Other readers of this header (health, web) use the counters
 * only; gnss_link_write(NULL, ...) cannot tell a query from a configuring
 * command, so "only the station configures the receiver" holds by
 * convention, not by enforcement.
 *
 * Uninitialised (safe mode skips gnss_link_init): gnss_link_get_stats(),
 * gnss_link_get_mode() and gnss_link_get_baud() still work and report the
 * link as OFF; everything else returns an error.
 *
 * Status: PROVISIONAL. Buffer sizes, RX task timing, error counters, the
 * RAW-mode contract, baud switching, break and restart are confirmed against
 * the UART-path design and on the bench. The mode/lease model is ours and
 * expected to hold.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gnss_framing.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GNSS_LINK_MODE_OFF = 0,  /**< UART not running */
    GNSS_LINK_MODE_STREAM,
    GNSS_LINK_MODE_COMMAND,
    GNSS_LINK_MODE_RAW,
} gnss_link_mode_t;

/** Opaque lease for COMMAND/RAW access. */
typedef struct gnss_link_lease *gnss_link_lease_t;

/**
 * Where framed data goes. Called from the RX task: must not block for long
 * (copy and return). Any member may be NULL.
 */
typedef struct {
    void (*on_rtcm)(const uint8_t *frame, size_t len, uint16_t msg_type, void *ctx);
    void (*on_line)(const char *line, size_t len, void *ctx);
    void *ctx;
} gnss_link_sinks_t;

/** Link counters (monotonic since boot). */
typedef struct {
    gnss_link_mode_t mode;
    uint32_t baud;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint32_t hw_fifo_overflows;    /**< UART hardware FIFO overrun events */
    uint32_t rx_buffer_full;       /**< driver software buffer full events */
    uint32_t line_errors;          /**< framing/parity/break events */
    uint32_t last_rx_age_ms;       /**< time since any byte arrived (UINT32_MAX = never) */
    uint32_t last_rtcm_age_ms;     /**< time since the last valid RTCM frame */
    gnss_framing_stats_t framing;
} gnss_link_stats_t;

/**
 * Install the UART driver (pins from board, baud from settings or Kconfig).
 * ESP_ERR_INVALID_STATE unless board_is_valid(): a rejected pin map never
 * reaches the UART.
 */
esp_err_t gnss_link_init(void);

/** Set the sinks. Call before gnss_link_start(); later calls replace them atomically. */
esp_err_t gnss_link_set_sinks(const gnss_link_sinks_t *sinks);

/** Start the RX task in STREAM mode. */
esp_err_t gnss_link_start(void);

/** Current mode. */
gnss_link_mode_t gnss_link_get_mode(void);

/**
 * Take exclusive COMMAND or RAW access.
 *
 * Blocks up to timeout_ms while another lease is held. Only the station
 * supervisor is expected to call this; other components go through it.
 *
 * @param owner short label for logs/status (not copied: use a literal).
 * @return ESP_ERR_TIMEOUT if another lease is held, ESP_ERR_INVALID_ARG for
 *         mode STREAM/OFF.
 */
esp_err_t gnss_link_acquire(gnss_link_mode_t mode, const char *owner, uint32_t timeout_ms,
                            gnss_link_lease_t *out);

/** Return to STREAM mode. Flushes stale input and resets the framer. */
esp_err_t gnss_link_release(gnss_link_lease_t lease);

/** Change the UART baud rate (COMMAND or RAW lease required). PROVISIONAL. */
esp_err_t gnss_link_set_baud(gnss_link_lease_t lease, uint32_t baud);

/**
 * Hold the TX line in break for duration_ms (COMMAND or RAW lease required),
 * e.g. before a full baud rescan. PROVISIONAL.
 */
esp_err_t gnss_link_send_break(gnss_link_lease_t lease, uint32_t duration_ms);

/**
 * Recovery: reinstall the UART driver at the current baud, flush input and
 * reset the framer. With lease == NULL in STREAM mode (the RX task reopens
 * and keeps streaming), or with the COMMAND lease. Counters are kept.
 * PROVISIONAL.
 */
esp_err_t gnss_link_restart(gnss_link_lease_t lease);

/** Current UART baud rate. */
uint32_t gnss_link_get_baud(void);

/**
 * Write bytes to the receiver.
 *
 * lease == NULL is allowed in STREAM mode only, for short read-only queries
 * while RTCM keeps flowing (version query, status). Anything that changes
 * receiver configuration goes through the station supervisor with a COMMAND
 * lease. This rule is by convention: the bytes are not inspected here.
 * Writers are serialised internally.
 */
esp_err_t gnss_link_write(gnss_link_lease_t lease, const void *data, size_t len, uint32_t timeout_ms);

/** RAW mode: read up to len bytes, waiting up to timeout_ms for the first. */
esp_err_t gnss_link_raw_read(gnss_link_lease_t lease, void *buf, size_t len, size_t *out_len,
                             uint32_t timeout_ms);

/** RAW mode: discard everything buffered on input. */
esp_err_t gnss_link_raw_flush(gnss_link_lease_t lease);

/** Snapshot of the counters. Works before gnss_link_init() (mode OFF, counters zero). */
esp_err_t gnss_link_get_stats(gnss_link_stats_t *out);

#ifdef __cplusplus
}
#endif
