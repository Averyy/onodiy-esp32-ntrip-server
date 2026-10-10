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
 *   STREAM   default. RX task frames everything. Read-only queries (the
 *            version log VERSIONA and the base-mode query MODE) may be
 *            written with lease == NULL (see gnss_link_write); replies arrive
 *            as text lines via the sink.
 *   COMMAND  exclusive command/config session (baud scan, configuration,
 *            receiver restart). RTCM is still framed but not published, so
 *            casters see a clean gap instead of half-configured output; those
 *            frames are counted in rtcm_held (expected, never real loss).
 *   RAW      exclusive byte access for the firmware flasher. The RX task parks
 *            on its task notification and reads neither the driver ring nor
 *            the event queue; the lease holder is the only reader
 *            (gnss_link_raw_read). On release, the events queued meanwhile
 *            (overflow / buffer-full / line errors; up to 32) are added to
 *            the counters first, then input is flushed, the framer reset and
 *            the RX task resubscribed to the task watchdog. Nobody takes
 *            events during RAW, so the queue fills and the driver drops
 *            further events; a raw read that finds it full counts what is
 *            queued, empties it and applies the full-queue rule below, so a
 *            stall of the lease holder is counted like one of the RX task.
 *            The framer is not fed during RAW, so its loader prompt detector
 *            is unused there; the flasher matches the loader's text itself.
 *
 * Hands off (gnss_link_set_hands_off, the station's receiver maintenance
 * state): every write and break and every lease acquisition is refused with
 * ESP_ERR_INVALID_STATE, so nothing reaches the receiver's COM1 while the
 * user works on it through its other port; reception, framing and
 * publishing go on.
 *
 * A lease is required for COMMAND and RAW. Leases carry a generation number,
 * so a stale handle (after release) is rejected with ESP_ERR_INVALID_STATE
 * instead of touching the wire. The handle is a token, never dereferenced.
 *
 * Mode switches are synchronous with the RX task: acquire and release hand
 * the request to the RX task, which applies it between two reads and
 * acknowledges, so no byte is consumed by two readers and no RTCM frame is
 * published after gnss_link_acquire() returns. Before gnss_link_start() (no
 * RX task yet) they take effect in the caller.
 *
 * RX task loop: wait on the UART event queue (32 entries) for 20 ms, or 0 ms
 * when the previous drain pass hit its cap; handle the event, if any; drain
 * by buffered length in 1024 B reads with zero timeout, at most 16 KB per
 * pass, feeding each chunk to the framer (never sized from an event's size
 * field); feed the task watchdog; repeat. Overflow and buffer-full events
 * never flush the driver or reset the event queue: they increment their
 * counter, reset the framer (the partial frame is counted as discarded) and
 * reading continues. The RX task never logs and never touches flash; it
 * only counts. The driver's interrupt is allocated on the RX task's core.
 *
 * Full event queue: the driver posts one data event per RX interrupt (about
 * 120 B) and silently drops any event that finds the queue full. A reader
 * stall therefore fills the queue with data events long before the 32 KB
 * ring is full, and the buffer-full and overflow events of that stall are
 * dropped. So whoever reads the ring (the RX task before it takes an event,
 * the RAW lease holder before a read) first looks at the queue: found full,
 * the queued events are counted and the queue emptied, rx_events_dropped
 * counts the episode (not in RAW, where a full queue is normal), and a ring
 * that is full at that moment counts as a buffer-full and as an overflow
 * (inferred: with RX interrupts paused the 128-byte hardware FIFO overruns
 * within 128 byte times, and its overflow events were dropped), each once
 * per episode and only if the event itself did not count it. The framer is
 * reset for every new episode.
 *
 * The driver is installed once and never deleted: its 32 KB RX ring is one
 * contiguous internal-RAM block (ISR in IRAM), which a fragmented heap may
 * not have again. gnss_link_restart() reconfigures the installed driver in
 * place. If the install itself fails (gnss_link_init), the link is closed
 * and gnss_link reopens it on its own: 1 s after the failure, then 5 s,
 * 30 s and every 60 s for ever, each with the full parameter set at the
 * stored baud, without waiting for anyone; the first failure is logged and
 * then at most one line per 10 min. link_open and open_failures in the
 * stats show it ("receiver UART closed (reopening)"); while closed nothing
 * is read, so a stall is classed as "no bytes". PROVISIONAL, to be confirmed
 * on hardware: whether a wedged UART peripheral ever needs a reinstall; if
 * so, only with the largest free internal block at least the RX ring plus
 * 8 KB.
 *
 * Only gnss_link changes the UART RX-FIFO-full threshold, and only right
 * after a baud change (gnss_link_init, gnss_link_set_baud, gnss_link_restart).
 * Version 1 keeps the driver default threshold and RX timeout at every baud.
 * Every host baud change discards input for 20 ms and resets the framer; the
 * discarded bytes are counted in baud_switch_bytes, not as errors.
 *
 * Watchdog: the RX task is subscribed to the task watchdog. While RAW is
 * held it parks, so gnss_link unsubscribes it on entering RAW and
 * resubscribes it on release. The lease holder's own task (the flasher, on
 * the station task) feeds its watchdog inside every wait, in slices of at
 * most 1 s, not only between blocks. A RAW lease therefore never trips the
 * watchdog however long the transfer takes.
 *
 * Buffering: the driver RX buffer (CONFIG_GNSS_LINK_RX_BUF_SIZE, internal
 * RAM) must cover the longest time the RX task can be kept off the CPU. With
 * XIP from PSRAM, flash writes no longer stall it; the buffer is sized for
 * the remaining cases (a flash operation while an mmap is open, bursts of
 * higher-priority work) with margin: 32 KB is ~2.8 s at 115200 baud and
 * ~0.7 s at 460800.
 *
 * Counter units: events for the UART conditions (hw_fifo_overflows,
 * rx_buffer_full, line_errors; the bytes an overflow loses are unknown;
 * rx_events_dropped counts episodes of a full event queue),
 * bytes for byte counters, frames for frame counters. Input dropped on
 * purpose (a lease release, gnss_link_resync, gnss_link_restart, the bytes
 * that piled up before gnss_link_start) is read out and counted in
 * flushed_bytes, so every byte that never reached the framer is counted
 * somewhere.
 *
 * Time values are ages computed from 64-bit esp_timer timestamps, never
 * differences of wrapping millisecond counters.
 *
 * Writers: only the station supervisor and the command layer it drives write
 * to the wire. Other readers of this header (health, web) use the counters
 * only; gnss_link_write(NULL, ...) cannot tell a query from a configuring
 * command, so "only the station configures the receiver" holds by
 * convention here; the command layer enforces its read-only allow-list.
 *
 * Uninitialised (safe mode skips gnss_link_init): gnss_link_get_stats(),
 * gnss_link_get_mode() and gnss_link_get_baud() still work and report the
 * link as OFF; everything else returns an error. gnss_link_init() may be
 * called twice (an early boot step can open the link before the normal init
 * runs); the second call returns ESP_OK.
 *
 * Lease and mode rules, routing, counters and the drain rule live in a pure
 * core (gnss_link_core.c) that the host tests cover.
 *
 * Status: PROVISIONAL, to be confirmed on hardware: buffer sizes, RX task
 * timing, error counters, the RAW-mode contract, baud switching, break and
 * restart. The mode/lease model is ours and expected to hold.
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
 * Where framed data goes. Called from the RX task: must not block, allocate
 * or log (copy and return). Any member may be NULL. From a sink only the
 * getters of this header work (gnss_link_get_stats/_mode/_baud); everything
 * else returns ESP_ERR_INVALID_STATE there, so a sink signals its own task
 * instead of acting. `frame` and `line` are valid only during the call;
 * `line` is not NUL-terminated.
 */
typedef struct {
    void (*on_rtcm)(const uint8_t *frame, size_t len, uint16_t msg_type, void *ctx);
    void (*on_line)(const char *line, size_t len, void *ctx);
    /** The framer was reset (overflow, baud change, lease release): data before it is discontinuous. */
    void (*on_reset)(void *ctx);
    /** The armed loader prompt detector saw the loader prompt (gnss_link_set_prompt_detect). */
    void (*on_prompt)(void *ctx);
    void *ctx;
} gnss_link_sinks_t;

/** Link counters (monotonic since boot). */
typedef struct {
    gnss_link_mode_t mode;
    uint32_t baud;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    /** UART hardware FIFO overruns (real loss, size unknown): events, plus one
     *  per stall inferred from a full ring found with a full event queue */
    uint32_t hw_fifo_overflows;
    /** driver software buffer full (back-pressure, no loss by itself): events,
     *  or a full ring found with a full event queue */
    uint32_t rx_buffer_full;
    uint32_t rx_events_dropped;    /**< episodes of a full UART event queue: the counts above may be low */
    uint32_t line_errors;          /**< framing/parity/break events */
    uint64_t baud_switch_bytes;    /**< bytes discarded in the 20 ms after a baud change or reopen (expected) */
    uint64_t rtcm_held;            /**< CRC-valid RTCM frames not published because a lease was held (expected) */
    uint64_t flushed_bytes;        /**< input dropped on purpose: lease release, resync, restart, before start (expected) */
    uint32_t last_rx_age_ms;       /**< time since any byte arrived (UINT32_MAX = never) */
    uint32_t last_rtcm_age_ms;     /**< time since the last CRC-valid RTCM frame, published or held (UINT32_MAX = never) */
    bool hands_off;                /**< transmitting and new leases are refused (receiver maintenance) */
    bool link_open;                /**< the UART driver is installed (false: closed, gnss_link reopens it on its own) */
    uint32_t open_failures;        /**< failed driver installs since boot (the first open and the reopens) */
    gnss_framing_stats_t framing;
} gnss_link_stats_t;

/**
 * Install the UART driver (pins from board, baud from gnss/baud if it is one
 * of the receiver's rates, else CONFIG_GNSS_LINK_DEFAULT_BAUD; 8N1, no flow
 * control, RX buffer CONFIG_GNSS_LINK_RX_BUF_SIZE, no TX buffer, event queue
 * of 32, no pattern detection). The link is then open in STREAM mode, but
 * nothing reads it until gnss_link_start() (or a RAW lease holder).
 * ESP_ERR_INVALID_STATE unless board_is_valid(): a rejected pin map never
 * reaches the UART. If the install fails, its error is returned but the link
 * is initialised as closed: once gnss_link_start() has run, the RX task
 * reopens it on its own schedule (see above). A second call returns ESP_OK.
 */
esp_err_t gnss_link_init(void);

/** Set the sinks. Call before gnss_link_start(); later calls replace them atomically. */
esp_err_t gnss_link_set_sinks(const gnss_link_sinks_t *sinks);

/**
 * Start the RX task (CONFIG_GNSS_LINK_RX_TASK_PRIO, pinned to
 * CONFIG_GNSS_LINK_RX_TASK_CORE, internal-RAM stack). Without a lease it
 * streams at once; what piled up since gnss_link_init() is dropped first
 * (counted in flushed_bytes; the UART events of that time are not counted,
 * since nobody was reading yet). With a RAW lease already held (the early
 * boot step) it starts parked and the lease holder keeps the input. A second
 * call returns ESP_OK.
 */
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
 * @return ESP_ERR_TIMEOUT if another lease is held (or a NULL-lease write
 *         did not finish) within timeout_ms, or the RX task did not take
 *         the request within 1 s (nothing changed); ESP_ERR_INVALID_ARG for
 *         mode STREAM/OFF or out == NULL, ESP_ERR_INVALID_STATE before
 *         gnss_link_init().
 */
esp_err_t gnss_link_acquire(gnss_link_mode_t mode, const char *owner, uint32_t timeout_ms,
                            gnss_link_lease_t *out);

/**
 * Return to STREAM mode. After RAW, pending UART events are added to the
 * counters first; then stale input is flushed (counted in flushed_bytes),
 * the framer reset (sinks get on_reset) and the RX task resubscribed to the
 * task watchdog. The loader prompt detector is disarmed. The handle is stale
 * afterwards. ESP_ERR_INVALID_STATE for a stale or unknown handle (nothing
 * held). Never times out: the release waits until the RX task has taken it
 * (it always gets to it, through its event queue or its task
 * notification), so a lease is never left held behind a failed release.
 */
esp_err_t gnss_link_release(gnss_link_lease_t lease);

/**
 * Arm or disarm the framer's loader prompt detector (COMMAND lease
 * required). Armed only while the station looks for the receiver
 * (detection); a hit reaches the on_prompt sink. Releasing the lease disarms
 * it. RAW sessions never use it (see RAW above). ESP_ERR_TIMEOUT if the RX
 * task did not take the request within 1 s (nothing changed). PROVISIONAL,
 * to be confirmed on hardware.
 */
esp_err_t gnss_link_set_prompt_detect(gnss_link_lease_t lease, bool armed);

/**
 * Change the host UART baud rate (COMMAND or RAW lease required; one of
 * 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, else
 * ESP_ERR_INVALID_ARG). Waits for pending TX to leave, switches, discards
 * input for 20 ms (counted in baud_switch_bytes) and resets the framer
 * (sinks get on_reset; in RAW the framer is reset on release instead).
 * Returns after the 20 ms, so every byte read afterwards arrived at the new
 * rate. ESP_ERR_TIMEOUT if a writer held the line for 1 s or the RX task did
 * not take the request within 1 s (the baud is unchanged then).
 * PROVISIONAL, to be confirmed on hardware.
 */
esp_err_t gnss_link_set_baud(gnss_link_lease_t lease, uint32_t baud);

/**
 * Hold the TX line in break for duration_ms, 1..1000 (COMMAND or RAW lease
 * required), e.g. before a full baud rescan. Waits for pending TX first;
 * blocks for the duration (the caller feeds its own watchdog around it).
 * PROVISIONAL, to be confirmed on hardware.
 */
esp_err_t gnss_link_send_break(gnss_link_lease_t lease, uint32_t duration_ms);

/**
 * Recovery: reset the framer and drop the input buffered in the driver,
 * without reinstalling anything. With lease == NULL in STREAM mode (no lease
 * is taken, so no post-lease pause follows) or with the COMMAND lease. The
 * RX task does it between two reads, so no byte is read twice. Sinks get
 * on_reset; counters are kept (the dropped bytes count in flushed_bytes).
 * ESP_ERR_INVALID_STATE in any other mode/lease combination, ESP_ERR_TIMEOUT
 * if the RX task did not take the request within 1 s (nothing done).
 * PROVISIONAL, to be confirmed on hardware.
 */
esp_err_t gnss_link_resync(gnss_link_lease_t lease);

/**
 * Recovery: restart the link at the stored baud (gnss/baud, else
 * CONFIG_GNSS_LINK_DEFAULT_BAUD), which may differ from the current one
 * after a scan or a failed baud change, keeping the installed driver (no
 * delete, no install, no allocation). In the RX task, in this order: RX
 * interrupts off; the pending UART events read and counted (the queue is
 * never reset); the buffered input flushed (counted in flushed_bytes); line
 * parameters (8N1, no flow control), pins, the stored baud, the RX-FIFO-full
 * threshold and RX timeout applied again as at init; RX interrupts on; the
 * first 20 ms discarded (baud_switch_bytes); the framer reset. Every step is
 * tried; the first error is returned and the driver stays installed.
 * With lease == NULL in STREAM mode, or with the COMMAND lease. Counters are
 * kept. ESP_ERR_INVALID_STATE while the driver is closed (the reopen loop
 * owns it) or under someone else's lease; ESP_ERR_TIMEOUT if a writer held
 * the line for 1 s or the RX task did not take the request within 1 s
 * (nothing done). PROVISIONAL, to be confirmed on hardware.
 */
esp_err_t gnss_link_restart(gnss_link_lease_t lease);

/** Current UART baud rate. */
uint32_t gnss_link_get_baud(void);

/**
 * Hands off on or off (see above; the station's receiver maintenance
 * state). Turning it on while a lease is held is refused with
 * ESP_ERR_INVALID_STATE (nothing changes). ESP_ERR_INVALID_STATE before
 * gnss_link_init() and from a sink.
 */
esp_err_t gnss_link_set_hands_off(bool on);

/**
 * Write bytes to the receiver.
 *
 * lease == NULL is allowed in STREAM mode only, for short read-only queries
 * while RTCM keeps flowing (VERSIONA, MODE). Anything that changes receiver
 * configuration, and the configuration dump CONFIG, goes through the station
 * supervisor with a COMMAND lease. This rule is by convention here (the
 * bytes are not inspected); um98x_ctl_command() enforces it.
 * Writers are serialised internally. Returns once the bytes have left the
 * UART (TX done), so reply timeouts can be measured from the return.
 *
 * @return ESP_ERR_TIMEOUT if another writer held the line, or the bytes did
 *         not leave, within timeout_ms; ESP_ERR_INVALID_STATE for a stale
 *         lease, lease == NULL outside STREAM, or hands off.
 */
esp_err_t gnss_link_write(gnss_link_lease_t lease, const void *data, size_t len, uint32_t timeout_ms);

/**
 * RAW mode: read up to len bytes, waiting up to timeout_ms for the first,
 * then taking whatever else is already buffered.
 * @return ESP_OK with *out_len >= 1, or ESP_ERR_TIMEOUT with *out_len == 0.
 */
esp_err_t gnss_link_raw_read(gnss_link_lease_t lease, void *buf, size_t len, size_t *out_len,
                             uint32_t timeout_ms);

/** RAW mode: discard everything buffered on input (uart_flush_input; not counted). */
esp_err_t gnss_link_raw_flush(gnss_link_lease_t lease);

/** Snapshot of the counters. Works before gnss_link_init() (mode OFF, counters zero). */
esp_err_t gnss_link_get_stats(gnss_link_stats_t *out);

#ifdef __cplusplus
}
#endif
