/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * GNSS link core: lease and mode arbitration, routing of framed units, loss
 * counters and the drain-pass rule (pure logic, no IDF driver or RTOS
 * includes, host-testable).
 *
 * gnss_link.c owns the UART, the RX task and the locking; it calls these
 * functions under its own spinlock and acts on the results after releasing
 * it. Nothing here blocks, allocates or logs.
 *
 * Leases: exactly one mode is active. STREAM is the default; COMMAND and RAW
 * need a lease. Every acquire issues a fresh non-zero token (a generation
 * number); the token is the lease handle. A token is valid only while its
 * lease is the one held, so a stale handle (after release) can never match
 * again, even if the same mode is taken again later. Token 0 means "no
 * lease".
 *
 * Who may do what (gnss_link_core_check):
 *   write            token 0 in STREAM only; a lease token in COMMAND or RAW
 *   set_baud, break  the lease token (COMMAND or RAW)
 *   prompt detector  the COMMAND lease token
 *   resync, restart  token 0 in STREAM, or the COMMAND lease token
 *   raw read/flush   the RAW lease token
 *   release          the lease token
 * Everything else, and everything while the link is OFF, is refused with
 * ESP_ERR_INVALID_STATE.
 *
 * Routing (gnss_link_core_route), per unit the framer emits:
 *   RTCM3   STREAM: publish. COMMAND or RAW: held, counted in rtcm_held
 *           (expected, never real loss). The RTCM age is stamped either way:
 *           the frame was received and CRC-valid.
 *   text    STREAM and COMMAND: to the line sink. RAW: dropped.
 *   prompt  only in COMMAND with the detector armed.
 *
 * Events (gnss_link_core_on_event; units are events):
 *   FIFO overflow   hw_fifo_overflows += 1, reset the framer
 *   buffer full     rx_buffer_full += 1, reset the framer (drain at once)
 *   frame/parity/break   line_errors += 1
 * None of them flushes the driver input or resets the event queue.
 *
 * Drain pass (gnss_link_pass_*): reads of at most GNSS_LINK_READ_CHUNK bytes,
 * sized from the driver's buffered length (never from an event's size), at
 * most GNSS_LINK_DRAIN_CAP bytes per pass. A pass that stopped at the cap
 * with data still buffered makes the next event wait zero, otherwise it is
 * GNSS_LINK_EVENT_WAIT_MS.
 *
 * Time: 64-bit microsecond stamps; ages are computed values clamped to
 * UINT32_MAX - 1 ms, UINT32_MAX meaning "never".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gnss_framing.h"
#include "gnss_link.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GNSS_LINK_EVENT_QUEUE_LEN  32u           /**< UART event queue entries */
#define GNSS_LINK_READ_CHUNK       1024u         /**< bytes per driver read */
#define GNSS_LINK_DRAIN_CAP        (16u * 1024u) /**< bytes per drain pass */
#define GNSS_LINK_EVENT_WAIT_MS    20u           /**< event wait after a pass that emptied the buffer */
#define GNSS_LINK_BAUD_SETTLE_MS   20u           /**< input discarded after a host baud change */
#define GNSS_LINK_BREAK_MAX_MS     1000u         /**< longest break one call may hold */

/** Driver defaults for the 128 B hardware FIFO: RX-full threshold 120 B, RX timeout 10 symbols. */
#define GNSS_LINK_RX_FULL_THRESH_DEFAULT 120
#define GNSS_LINK_RX_TIMEOUT_DEFAULT     10

/** UART conditions, mapped from the driver's event types by gnss_link.c. */
typedef enum {
    GNSS_LINK_EV_DATA = 0,     /**< data arrived (the drain reads it; nothing to count) */
    GNSS_LINK_EV_FIFO_OVF,     /**< hardware FIFO overrun: bytes lost */
    GNSS_LINK_EV_BUFFER_FULL,  /**< driver ring full: back-pressure, no loss by itself */
    GNSS_LINK_EV_FRAME_ERR,
    GNSS_LINK_EV_PARITY_ERR,
    GNSS_LINK_EV_BREAK,
    GNSS_LINK_EV_OTHER,        /**< anything else (wake-ups, pattern, TX-side): ignored */
} gnss_link_ev_t;

/** Operations that need a permission check. */
typedef enum {
    GNSS_LINK_OP_WRITE = 0,
    GNSS_LINK_OP_SET_BAUD,
    GNSS_LINK_OP_BREAK,
    GNSS_LINK_OP_PROMPT,
    GNSS_LINK_OP_RESYNC,
    GNSS_LINK_OP_RESTART,
    GNSS_LINK_OP_RAW_IO,
    GNSS_LINK_OP_RELEASE,
} gnss_link_op_t;

/** What to do with one framed unit. */
typedef enum {
    GNSS_LINK_ROUTE_DROP = 0,
    GNSS_LINK_ROUTE_PUBLISH,   /**< RTCM to the frame sink */
    GNSS_LINK_ROUTE_HOLD,      /**< RTCM held during a lease (counted) */
    GNSS_LINK_ROUTE_LINE,      /**< text line to the line sink */
    GNSS_LINK_ROUTE_PROMPT,    /**< loader prompt to the prompt sink */
} gnss_link_route_t;

typedef struct {
    gnss_link_mode_t mode;   /**< OFF until gnss_link_core_init() */
    uint32_t lease;          /**< token of the lease held, 0 = none */
    uint32_t last_token;     /**< last token issued */
    const char *owner;       /**< label of the lease holder (not copied) */
    bool prompt_armed;
    uint32_t baud;
    bool rx_seen;
    int64_t last_rx_us;
    bool rtcm_seen;
    int64_t last_rtcm_us;
    gnss_link_stats_t stats; /**< counters; mode, baud and ages are filled by the snapshot */
} gnss_link_core_t;

/** One drain pass. */
typedef struct {
    size_t done;     /**< bytes read in this pass */
    bool capped;     /**< stopped at the cap with data still buffered */
} gnss_link_pass_t;

/** Link open at `baud`, STREAM mode, counters zero. */
void gnss_link_core_init(gnss_link_core_t *c, uint32_t baud);

/**
 * Take a COMMAND or RAW lease (the caller waits for a free slot itself).
 * @return ESP_OK with a fresh token, ESP_ERR_INVALID_ARG for STREAM/OFF or a
 *         NULL token pointer, ESP_ERR_INVALID_STATE if the link is OFF or a
 *         lease is already held.
 */
esp_err_t gnss_link_core_acquire(gnss_link_core_t *c, gnss_link_mode_t mode, const char *owner, uint32_t *token);

/** May `token` (0 = no lease) perform `op` now? ESP_OK or ESP_ERR_INVALID_STATE. */
esp_err_t gnss_link_core_check(const gnss_link_core_t *c, gnss_link_op_t op, uint32_t token);

/**
 * Return to STREAM. *was (may be NULL) gets the mode that ended; the prompt
 * detector is disarmed and the token becomes stale.
 * @return ESP_ERR_INVALID_STATE for anything but the current lease token.
 */
esp_err_t gnss_link_core_release(gnss_link_core_t *c, uint32_t token, gnss_link_mode_t *was);

/** Arm or disarm the prompt detector (COMMAND lease only). */
esp_err_t gnss_link_core_set_prompt(gnss_link_core_t *c, uint32_t token, bool armed);

/** Count one UART condition. @return true if the framer must be reset. */
bool gnss_link_core_on_event(gnss_link_core_t *c, gnss_link_ev_t ev);

/** `n` bytes were read from the driver at `now_us`. */
void gnss_link_core_on_rx(gnss_link_core_t *c, size_t n, int64_t now_us);

/** Route one framed unit and count it. */
gnss_link_route_t gnss_link_core_route(gnss_link_core_t *c, gnss_frame_kind_t kind, int64_t now_us);

/** `n` bytes were written to the receiver. */
void gnss_link_core_on_tx(gnss_link_core_t *c, size_t n);

/** The host switched to `baud` and discarded `discarded` bytes while it settled. */
void gnss_link_core_on_baud(gnss_link_core_t *c, uint32_t baud, size_t discarded);

/** `n` buffered input bytes were dropped on purpose (resync, restart, lease release, start). */
void gnss_link_core_on_flush(gnss_link_core_t *c, size_t n);

/** Latest framer counters (copied into every snapshot). */
void gnss_link_core_set_framing(gnss_link_core_t *c, const gnss_framing_stats_t *f);

/** Counters plus mode, baud and ages at `now_us`. */
void gnss_link_core_snapshot(const gnss_link_core_t *c, int64_t now_us, gnss_link_stats_t *out);

/** Age in ms of a stamp; UINT32_MAX if never seen, 0 if the stamp is in the future. */
uint32_t gnss_link_core_age_ms(bool seen, int64_t stamp_us, int64_t now_us);

/** One of the receiver's COM rates: 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600. */
bool gnss_link_core_baud_supported(uint32_t baud);

/**
 * Baud to open the link at: the stored rate if it was read (stored_err ==
 * ESP_OK) and is supported, else `fallback` if supported, else 115200.
 */
uint32_t gnss_link_core_open_baud(esp_err_t stored_err, uint32_t stored, uint32_t fallback);

/** RX-FIFO-full threshold (bytes) to apply right after switching to `baud`. */
int gnss_link_core_rx_full_threshold(uint32_t baud);

/** RX timeout (symbol times) to apply right after switching to `baud`. */
uint8_t gnss_link_core_rx_timeout(uint32_t baud);

/** Is `duration_ms` a valid break length (1 .. GNSS_LINK_BREAK_MAX_MS)? */
bool gnss_link_core_break_ok(uint32_t duration_ms);

/** Start a drain pass. */
void gnss_link_pass_begin(gnss_link_pass_t *p);

/**
 * Bytes to read next, given what the driver has buffered; 0 ends the pass
 * (nothing buffered, or the cap is reached: then `capped` is set).
 */
size_t gnss_link_pass_next(gnss_link_pass_t *p, size_t buffered);

/** `n` bytes were read. */
void gnss_link_pass_add(gnss_link_pass_t *p, size_t n);

/** Event wait before the next pass: 0 after a capped pass, else GNSS_LINK_EVENT_WAIT_MS. */
uint32_t gnss_link_pass_wait_ms(const gnss_link_pass_t *p);

#ifdef __cplusplus
}
#endif
