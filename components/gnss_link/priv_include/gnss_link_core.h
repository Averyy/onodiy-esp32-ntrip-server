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
 * Hands off (gnss_link_core_set_hands_off: the receiver maintenance state,
 * the user works on the receiver through its other port): every write and
 * break, with or without a lease, and every lease acquisition are refused
 * with ESP_ERR_INVALID_STATE, so nothing reaches the receiver by mistake.
 * Reception goes on (routing is unchanged).
 *
 * Routing (gnss_link_core_route), per unit the framer emits:
 *   RTCM3   STREAM: publish. COMMAND or RAW: held, counted in rtcm_held
 *           (expected, never real loss). The RTCM age is stamped either way,
 *           from the frame's arrival: it was received and CRC-valid.
 *   text    STREAM and COMMAND: to the line sink. RAW: dropped.
 *   prompt  only in COMMAND with the detector armed.
 *
 * Events (gnss_link_core_on_event; units are events):
 *   FIFO overflow   hw_fifo_overflows += 1, reset the framer
 *   buffer full     rx_buffer_full += 1 once per ring-full episode, reset the
 *                   framer (drain at once)
 *   frame/parity/break   line_errors += 1
 * None of them flushes the driver input or resets the event queue.
 *
 * Full event queue (gnss_link_core_on_queue_full): the driver drops every
 * event that finds the queue full, and a reader stall fills it with data
 * events (one per ~120 B) long before the ring is full. Whoever reads the
 * ring looks at the queue first; found full, the queued events are counted
 * and taken, then this rule stands in for the dropped ones:
 *   - rx_events_dropped += 1 once per episode (the queue was seen with room
 *     in between), except in RAW, where nobody takes events and a full queue
 *     is normal;
 *   - the ring is full (less than one hardware FIFO of room left: the driver
 *     has paused RX interrupts): rx_buffer_full += 1 and hw_fifo_overflows
 *     += 1 (inferred: the 128-byte FIFO overruns within 128 byte times while
 *     RX interrupts are paused, and those overflow events were dropped),
 *     each once per ring-full episode and only if its event did not count it;
 *   - a new episode resets the framer.
 * A ring-full episode ends when a reader finds the ring below the full mark
 * (gnss_link_core_on_level).
 *
 * Drain pass (gnss_link_pass_*): reads of at most GNSS_LINK_READ_CHUNK bytes,
 * sized from the driver's buffered length (never from an event's size), at
 * most GNSS_LINK_DRAIN_CAP bytes per pass. A pass that stopped at the cap
 * with data still buffered makes the next event wait zero, otherwise it is
 * GNSS_LINK_EVENT_WAIT_MS.
 *
 * Prompt observer (bench hooks, gnss_link_core_set_observe): the framer's
 * prompt detector also runs outside the detection's lease, report-only. A
 * prompt the lease-armed detector does not take (anything but COMMAND with
 * it armed) is routed GNSS_LINK_ROUTE_OBSERVE while observing: counted with
 * its time, never to the prompt sink. The framer's own rule still applies:
 * a CRC-valid RTCM frame within GNSS_PROMPT_WINDOW bytes of the match
 * suppresses it. The context of a hit is the GNSS_LINK_PROMPT_CTX bytes
 * ending at the match's last byte, as far as the link still holds them
 * (the chunk being framed and the bytes right before it).
 *
 * Timing summary (gnss_link_timing_summary): see gnss_link.h, bench hooks.
 *
 * Arrival ledger (gnss_link_arr_*, the RX task's; see gnss_link.h "Arrival
 * times"). The UART interrupt writes one 32-bit esp_timer stamp per RX chunk
 * it moves into the driver's ring (the select notification, called before
 * the chunk's data event is queued) into a ring of GNSS_LINK_ARR_STAMPS
 * entries and advances a head counter. The ledger:
 *   - pairs the k-th data event taken since the last resync with stamp
 *     index base + k (gnss_link_arr_on_data), keeping {stamp, bytes left}
 *     per chunk; an event with no stamp, or whose stamp was overwritten
 *     already, is a mismatch;
 *   - turns each read of n bytes into segments (gnss_link_arr_next), one per
 *     chunk it touches, each with that chunk's arrival rebuilt to 64 bits
 *     against the read time (the stamp is never after the read), clamped so
 *     arrivals never go back and never pass the read time; a chunk only
 *     partly read keeps the rest for the next read;
 *   - bytes beyond the chunks it knows take the read time, inexact, and
 *     drop the pairing (a mismatch);
 *   - drops the pairing on every known cause (gnss_link_arr_degrade);
 *   - starts afresh only at a quiet point (gnss_link_arr_quiet: the ring and
 *     the event queue both empty, read with the stamp head in one critical
 *     section on the RX core, where the interrupt cannot run): base = head.
 *     A head that differs from the events taken there, or bytes still owed
 *     to a chunk, count as a mismatch.
 * Without the pairing every byte takes its read time and is inexact.
 *
 * Closed driver (gnss_link_core_reopen_*): when the UART driver could not be
 * installed, gnss_link retries the install on its own: 1 s after the
 * failure, then 5 s, 30 s, and every 60 s for ever; the first failure is
 * logged, then at most one line per GNSS_LINK_REOPEN_LOG_MS. link_open and
 * open_failures show it.
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
#define GNSS_LINK_HW_FIFO_LEN      128u          /**< UART hardware RX FIFO (bytes) */
#define GNSS_LINK_READ_CHUNK       1024u         /**< bytes per driver read */
#define GNSS_LINK_DRAIN_CAP        (16u * 1024u) /**< bytes per drain pass */
#define GNSS_LINK_EVENT_WAIT_MS    20u           /**< event wait after a pass that emptied the buffer */
#define GNSS_LINK_BAUD_SETTLE_MS   20u           /**< input discarded after a host baud change */
#define GNSS_LINK_BREAK_MAX_MS     1000u         /**< longest break one call may hold */
#define GNSS_LINK_REOPEN_LOG_MS    (10u * 60u * 1000u) /**< a closed driver logs its failed reopens at most this often */

/** Driver defaults for the 128 B hardware FIFO: RX-full threshold 120 B, RX timeout 10 symbols. */
#define GNSS_LINK_RX_FULL_THRESH_DEFAULT 120
#define GNSS_LINK_RX_TIMEOUT_DEFAULT     10
/** RX-full threshold above 115200: the 8 B the default leaves last 0.17 ms at 460800, 64 B last 1.4 ms. */
#define GNSS_LINK_RX_FULL_THRESH_FAST    64

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
    GNSS_LINK_ROUTE_OBSERVE,   /**< loader prompt counted by the report-only observer (no sink) */
} gnss_link_route_t;

typedef struct {
    gnss_link_mode_t mode;   /**< OFF until gnss_link_core_init() */
    uint32_t lease;          /**< token of the lease held, 0 = none */
    uint32_t last_token;     /**< last token issued */
    const char *owner;       /**< label of the lease holder (not copied) */
    bool prompt_armed;
    bool hands_off;          /**< transmitting and new leases refused (receiver maintenance) */
    uint32_t baud;
    bool rx_seen;
    int64_t last_rx_us;
    bool rtcm_seen;
    int64_t last_rtcm_us;
    bool ring_full_seen;     /**< this ring-full episode was counted in rx_buffer_full */
    bool overflow_seen;      /**< an overflow was counted since the ring was last below the full mark */
    bool queue_full_seen;    /**< this full-queue episode was counted in rx_events_dropped */
    bool link_open;          /**< the UART driver is installed */
    gnss_link_stats_t stats; /**< counters; mode, baud, ages and link_open are filled by the snapshot */

    /* report-only prompt observer (bench hooks) */
    bool observe;
    uint32_t obs_hits;
    bool obs_hit_seen;
    int64_t obs_hit_us;
    uint8_t obs_ctx[GNSS_LINK_PROMPT_CTX];
    uint8_t obs_ctx_len;
    uint8_t obs_hist[GNSS_LINK_PROMPT_CTX];   /**< the bytes framed right before obs_hist_end */
    uint8_t obs_hist_len;
    uint64_t obs_hist_end;                    /**< stream offset right after the history's last byte */
    const uint8_t *obs_chunk;                 /**< the chunk being framed (RX task, during the feed only) */
    size_t obs_chunk_len;
    uint64_t obs_chunk_at;                    /**< stream offset of obs_chunk[0] */
} gnss_link_core_t;

#define GNSS_LINK_ARR_STAMPS GNSS_LINK_EVENT_QUEUE_LEN   /**< interrupt stamps kept: one per data event the queue holds */
#define GNSS_LINK_ARR_CHUNKS 128u                        /**< chunks paired with their stamp and not read yet */

/** One chunk the interrupt moved into the ring: its stamp and the bytes not read yet. */
typedef struct {
    uint32_t stamp;          /**< (uint32_t) esp_timer microseconds at the interrupt */
    uint16_t left;
} gnss_link_arr_chunk_t;

/** The arrival ledger (RX task; in PSRAM). */
typedef struct {
    bool synced;             /**< the pairing is known (false: every byte takes its read time) */
    uint32_t next;           /**< stamp index the next data event pairs with */
    uint16_t first;          /**< chunk FIFO */
    uint16_t n;
    gnss_link_arr_chunk_t c[GNSS_LINK_ARR_CHUNKS];
    bool have_last;
    int64_t last_us;         /**< the latest arrival handed out (arrivals never go back) */
    gnss_link_arrival_stats_t st;
} gnss_link_arr_t;

/** One part of a read: `len` bytes that arrived at `arrival_us` (`exact` false: the read time). */
typedef struct {
    size_t len;
    int64_t arrival_us;
    bool exact;
} gnss_link_arr_seg_t;

/** Not paired yet (the first quiet point pairs it). */
void gnss_link_arr_init(gnss_link_arr_t *a);

/** A known cause broke the pairing (bytes or events went past the ledger): read times until a quiet point. */
void gnss_link_arr_degrade(gnss_link_arr_t *a);

/**
 * The RX task took a data event of `size` bytes. `stamps` is the interrupt's
 * ring (GNSS_LINK_ARR_STAMPS entries) and `head` its head counter, read
 * together in one critical section.
 */
void gnss_link_arr_on_data(gnss_link_arr_t *a, size_t size, const volatile uint32_t *stamps, uint32_t head);

/**
 * The next segment of a read with `left` bytes still to place, which
 * returned at `read_us`. Its len is at least 1 and at most `left` (left > 0).
 */
gnss_link_arr_seg_t gnss_link_arr_next(gnss_link_arr_t *a, size_t left, int64_t read_us);

/** A quiet point (ring and event queue empty) with the stamp ring's head at `head`. */
void gnss_link_arr_quiet(gnss_link_arr_t *a, uint32_t head);

/** An RTCM frame was framed with an interrupt time (`exact`) or without. */
void gnss_link_arr_count_frame(gnss_link_arr_t *a, bool exact);

/** One drain pass. */
typedef struct {
    size_t done;     /**< bytes read in this pass */
    bool capped;     /**< stopped at the cap with data still buffered */
} gnss_link_pass_t;

/** Link open at `baud`, STREAM mode, counters zero. */
void gnss_link_core_init(gnss_link_core_t *c, uint32_t baud);

/** The driver is installed (true) or closed (false). */
void gnss_link_core_set_open(gnss_link_core_t *c, bool open);

/** An install of the driver failed (open_failures += 1, the link is closed). */
void gnss_link_core_on_open_failed(gnss_link_core_t *c);

/**
 * Delay before the next install of a closed driver, after `failures` failed
 * reopen attempts (0 = right after the failure that closed it): 1 s, 5 s,
 * 30 s, then 60 s for ever.
 */
uint32_t gnss_link_core_reopen_delay_ms(uint32_t failures);

/**
 * Delay after a reopen attempt that fails, with `failed_before` failed
 * reopen attempts before it: gnss_link_core_reopen_delay_ms(failed_before + 1).
 * From the closing failure the attempts so run 1 s, 6 s, 36 s, 96 s, ...
 * later (gaps of 1, 5, 30, then 60 s).
 */
uint32_t gnss_link_core_reopen_next_ms(uint32_t failed_before);

/**
 * Should this failed reopen be logged? The first one always
 * (`logged` false), then one at most every GNSS_LINK_REOPEN_LOG_MS after
 * `last_log_us`.
 */
bool gnss_link_core_reopen_log_due(bool logged, int64_t last_log_us, int64_t now_us);

/**
 * Take a COMMAND or RAW lease (the caller waits for a free slot itself).
 * @return ESP_OK with a fresh token, ESP_ERR_INVALID_ARG for STREAM/OFF or a
 *         NULL token pointer, ESP_ERR_INVALID_STATE if the link is OFF or a
 *         lease is already held.
 */
esp_err_t gnss_link_core_acquire(gnss_link_core_t *c, gnss_link_mode_t mode, const char *owner, uint32_t *token);

/**
 * Hands off on or off (see above). Turning it on while a lease is held is
 * refused with ESP_ERR_INVALID_STATE (nothing changes): the holder would
 * be cut off mid-session. Off always succeeds.
 */
esp_err_t gnss_link_core_set_hands_off(gnss_link_core_t *c, bool on);

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

/** Turn the report-only prompt observer on or off (its counters are kept). */
void gnss_link_core_set_observe(gnss_link_core_t *c, bool on);

/** Should the framer's prompt detector be armed: the detection's lease-armed detector, or the observer. */
bool gnss_link_core_detector_on(const gnss_link_core_t *c);

/**
 * While observing, around every framer feed: the chunk about to be framed
 * (`at` = stream offset of data[0], the framer's bytes_in before the feed),
 * then gnss_link_core_observe_fed() once the feed returned (keeps the bytes
 * before the next chunk for a later hit's context).
 */
void gnss_link_core_observe_feed(gnss_link_core_t *c, const uint8_t *data, size_t len, uint64_t at);
void gnss_link_core_observe_fed(gnss_link_core_t *c);

/** A prompt was routed GNSS_LINK_ROUTE_OBSERVE: keep the bytes ending at `at` (its last byte) as its context. */
void gnss_link_core_observe_hit(gnss_link_core_t *c, uint64_t at);

/** The observer's state, its count and its last hit (time as esp_timer ms, -1 = none). */
void gnss_link_core_observe_get(const gnss_link_core_t *c, gnss_link_prompt_obs_t *out);

/**
 * Reduce a recording (see gnss_link.h, bench hooks). scratch: room for one
 * value per record (sorted in place). clock_ok and unix_minus_timer_us:
 * the SNTP-set system clock, else no epoch delays (clock false).
 */
void gnss_link_timing_summary(const gnss_link_rec_t *rec, size_t n, bool full, bool clock_ok,
                              int64_t unix_minus_timer_us, int32_t *scratch, size_t scratch_cap,
                              gnss_link_timing_summary_t *out);

/** Percentiles (nearest rank) of v[0..n), sorted in place. */
void gnss_link_pct(int32_t *v, size_t n, gnss_link_pct_t *out);

/**
 * One record as a CSV line with its newline:
 *   F,<arrival us>,<read us>,<published us>,<msg>,<len>,<sys>,<epoch>,<mmb>,<held>,<exact>
 *   P,<begin us>,<end us>,<bytes>,<capped>,<next wait ms>,<gap us>
 *   S,<begin us>,<end us>,<rx|raw>
 * (sys and epoch empty for a frame that is not an MSM).
 * @return length, 0 if it does not fit (buf then holds "").
 */
size_t gnss_link_rec_csv(const gnss_link_rec_t *r, char *buf, size_t cap);

/** Count one UART condition. @return true if the framer must be reset. */
bool gnss_link_core_on_event(gnss_link_core_t *c, gnss_link_ev_t ev);

/** Ring with less than one hardware FIFO of room: the driver pauses RX interrupts. */
bool gnss_link_core_ring_full(size_t buffered, size_t ring_size);

/**
 * The event queue was found full (see "Full event queue" above), after the
 * events still queued were counted with gnss_link_core_on_event().
 * @param buffered    bytes in the driver ring now
 * @param ring_size   the ring's size
 * @param count_drop  count the episode in rx_events_dropped (false in RAW)
 * @return true if the framer must be reset (a new episode).
 */
bool gnss_link_core_on_queue_full(gnss_link_core_t *c, size_t buffered, size_t ring_size, bool count_drop);

/** The event queue had room: the next full queue is a new episode. */
void gnss_link_core_on_queue_ok(gnss_link_core_t *c);

/** A reader found `buffered` bytes in the ring: below the full mark ends a ring-full episode. */
void gnss_link_core_on_level(gnss_link_core_t *c, size_t buffered, size_t ring_size);

/** `n` bytes were read from the driver at `now_us`. */
void gnss_link_core_on_rx(gnss_link_core_t *c, size_t n, int64_t now_us);

/** Route one framed unit and count it (`arrival_us`: when an RTCM frame's last byte arrived). */
gnss_link_route_t gnss_link_core_route(gnss_link_core_t *c, gnss_frame_kind_t kind, int64_t now_us,
                                       int64_t arrival_us);

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
