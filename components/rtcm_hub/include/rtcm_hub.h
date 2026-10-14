/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * RTCM fan-out hub: one producer, many independent consumers.
 *
 * The GNSS link publishes each complete, CRC-checked RTCM3 frame once. The hub
 * stores frames back-to-back in a single ring in PSRAM. Every consumer (one per
 * caster, the web live view, later a LAN server or SD logger) holds its own
 * read cursor, so a slow or disconnected consumer never delays the producer or
 * any other consumer.
 *
 * Policy:
 *  - The producer never blocks. When the ring is full the oldest frames are
 *    overwritten.
 *  - Epochs: every record carries an epoch sequence number (the `epoch`
 *    field), assigned at publish time. A new epoch starts (a) at the first
 *    frame after an MSM whose multiple-message bit is 0 (last MSM of its
 *    epoch), (b) at an MSM whose constellation already has an MSM with a
 *    different epoch time in the current epoch (a receiver that never clears
 *    the bit), (c) at a frame that is no MSM arriving
 *    CONFIG_RTCM_HUB_BURST_GAP_MS or more after the previous one while the
 *    current epoch holds an MSM not yet terminated (a lost terminating MSM:
 *    the next epoch's station messages; its first MSM meets rule (b)), and
 *    at the first frame after rtcm_hub_clear_sticky(). Rule (c) applies
 *    only between two frames with exact arrival times (the UART interrupt's
 *    time of their last byte, gnss_link.h "Arrival times"): a frame with
 *    only its read time never starts an epoch by a gap, so a reader that
 *    stalled (flash writes stop every task) makes no gap inside an epoch.
 *    An MSM after a gap continues its epoch. Non-MSM frames after an
 *    epoch's last MSM belong to the next epoch. Epoch times are compared
 *    within one constellation only, so no time-system conversion is
 *    needed.
 *  - Unsure epochs: the epoch started by the very first frame, and by the
 *    first frame after rtcm_hub_clear_sticky(), is "unsure". Framer resets
 *    (lease release, UART events, apply, flash) drop bytes anywhere in an
 *    epoch, so that frame may be the middle or the tail of an epoch (e.g. a
 *    GLONASS MSM whose GPS MSM was lost). No consumer ever starts on an
 *    unsure epoch: one that is waiting skips it whole, and one that is
 *    streaming ends the epoch it was sending there, goes back to waiting
 *    and counts the unsure frames as idle skips. The next epoch started by
 *    rule (a), (b) or (c) is sure; while an epoch is unsure, a frame that is
 *    no MSM a gap of CONFIG_RTCM_HUB_BURST_GAP_MS after the previous one
 *    also starts a sure one, so a stream without MSMs still flows after a
 *    reset. An MSM after such a gap never does, unsure epoch or not: right
 *    after a reset the station writes to flash, and the rest of the epoch
 *    read late would otherwise start a sure epoch partway through. A caster
 *    therefore resumes after every pause or UART reset at the first frame of
 *    a whole epoch.
 *  - Station metadata: the latest 1005/1006, 1033 and 1230 frames are kept
 *    as sticky copies outside the ring. They are cleared on every framer
 *    reset and every receiver reconfiguration (rtcm_hub_clear_sticky), and a
 *    copy older than 30 s is never replayed, so a superseded position is
 *    never sent.
 *  - A new consumer, and one that calls rtcm_hub_resync() (e.g. after a
 *    caster reconnect), first receives the sticky metadata frames, then live
 *    data starting at the next sure epoch start. A caster never starts with
 *    the tail of an epoch or waits for the station position. Epoch starts that
 *    are already over the consumer's lag limit when it reads are skipped too
 *    (as idle skips).
 *  - Hold (a pause for receiver work): rtcm_hub_hold() fixes a point in the
 *    ring at an epoch boundary, and no consumer gets a frame at or after it
 *    until rtcm_hub_release(). A caster still behind it sends the rest of
 *    its epochs up to that point at its own pace (a slot stuck in a TCP
 *    write included), then gets ESP_ERR_INVALID_STATE from rtcm_hub_read().
 *    The point is a position, not a time: RTCM keeps arriving until the
 *    receiver session takes the line, and a caster waiting in
 *    rtcm_hub_read() when the pause is decided would otherwise send what
 *    arrives meanwhile (the next epoch's station messages and first MSMs),
 *    an epoch the receiver work then cuts. Where it goes: see
 *    rtcm_hub_hold(). After the release every consumer starts over at its
 *    next read, as after rtcm_hub_resync(): sticky copies, then the next
 *    sure epoch; what was published from the hold point on is never sent.
 *  - Finishing (a session about to close, one consumer): after
 *    rtcm_hub_finish_epoch() the consumer gets the rest of the epoch it is
 *    sending and nothing of the next one, so a planned close (reboot, new
 *    settings) ends the stream on a whole epoch.
 *  - Lag limit per consumer (rtcm_hub_set_max_lag; default
 *    CONFIG_RTCM_HUB_MAX_LAG_MS, the Onocoy slot 1000 ms): a consumer whose
 *    oldest unread frame is older than its limit drops whole oldest epochs
 *    until it is back under the limit, but an epoch already partly delivered
 *    is finished first, never cut (only properly terminated epochs count
 *    for Onocoy). Not a jump to the very newest frame, which would throw
 *    away still-valid data. A consumer that was overwritten ("lapped")
 *    resyncs as above.
 *  - Reads return whole frames only, so a caster never sends a partial frame.
 *  - A failed publish loses that frame only and is counted
 *    (publish_failures); the link counters still show it received.
 *  - Types 1101-1107 (SBAS MSM numbers the receiver may reuse for its own
 *    messages) are dropped before the hub by the station and counted there.
 *  - A per-consumer filter on non-MSM types (MSMs always pass, so epoch
 *    termination is never broken) is planned (SHOULD), not provided yet.
 *
 * Ring and concurrency (single producer):
 *  - Records {u16 len, u16 msg_type, u32 seq, u32 rx_ms, u32 epoch (sequence
 *    number)} + frame, padded to 4, never wrapping (a wrap marker sends the
 *    record to offset 0).
 *    Offsets are monotonic 64-bit byte counts.
 *  - The producer first publishes reserve_off = write_off + record length
 *    (including any wrap waste) under a short spinlock, copies the record,
 *    then publishes write_off. Consumers validate their read offset against
 *    reserve_off before parsing any header and again after copying, so bytes
 *    being overwritten are never trusted; a header's len is clamped to
 *    GNSS_RTCM3_MAX_FRAME before use.
 *  - Wake-up uses task notification index 1
 *    (CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES >= 2), so it never
 *    collides with IDF APIs on index 0; a consumer task leaves index 1 to the
 *    hub (a stray notification only costs an extra check). Attach and detach
 *    take the hub lock; detach clears the consumer's task handle and, if the
 *    producer is notifying at that moment, waits for that pass to end, so the
 *    producer never notifies a task that has gone away after detaching.
 *  - rx_ms is a wrapping millisecond stamp; ages are only ever computed as
 *    (uint32_t)(now - then), clamped at 0. Once the newest frame is older
 *    than 2^31 ms, every stored frame counts as older than any lag limit.
 *
 * Loss accounting: frames a consumer skips while it is not streaming
 * (before attach, during resync after an outage or pause, unsure epochs)
 * are expected and go to `frames_skipped_idle`. Only frames lost while streaming (lapped or
 * over the lag limit) count as `frames_dropped`, the real data-loss figure;
 * after a lap, the rest of the newest epoch skipped while resyncing counts as
 * dropped too. Every frame published while a consumer is attached ends up in
 * exactly one of frames_out, frames_dropped or frames_skipped_idle
 * (frames_out also counts the sticky replays).
 *
 * Thread safety: rtcm_hub_publish() and rtcm_hub_clear_sticky() from one task
 * (the GNSS link RX task); every consumer handle used by exactly one task for
 * read, resync, finish_epoch, set_max_lag, restarted and detach. The stats
 * getters, the hold and the release may be called from any task.
 *
 * The ring, epoch and cursor logic is a pure core (rtcm_hub_core.c,
 * rtcm_epoch.c) with host tests; this component wraps it with the spinlock,
 * PSRAM buffers, the clock and task notifications.
 *
 * Status: implemented. Ring size, lag limits and the burst gap are
 * PROVISIONAL, to be confirmed on hardware (epoch rule counters on a real
 * stream, burst timing).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque consumer handle. */
typedef struct rtcm_hub_consumer *rtcm_hub_consumer_t;

/** Smallest read buffer accepted: one maximum RTCM3 frame (3 + 1023 + 3). */
#define RTCM_HUB_MIN_READ_BUF 1029u

/** Hub-wide counters. */
typedef struct {
    uint64_t frames_in;          /**< frames published */
    uint64_t bytes_in;
    uint64_t epochs_in;          /**< epochs started */
    uint64_t epochs_by_mmb;      /**< started by rule (a): the previous epoch's last MSM said so */
    uint64_t epochs_by_time;     /**< started by rule (b): an epoch-time change within a constellation */
    uint64_t epochs_by_gap;      /**< started by rule (c): a gap while an MSM epoch was open */
    uint64_t publish_failures;   /**< frames rtcm_hub_publish() could not store (lost) */
    uint32_t consumers;          /**< currently attached */
    size_t capacity;             /**< ring size in bytes */
    uint32_t last_frame_age_ms;  /**< time since the last publish (UINT32_MAX = never) */
} rtcm_hub_stats_t;

/** Per-consumer counters. */
typedef struct {
    uint64_t frames_out;
    uint64_t bytes_out;
    uint64_t frames_dropped;     /**< lost while streaming (lapped or over the lag limit): real loss */
    uint64_t bytes_dropped;
    uint64_t frames_skipped_idle;/**< skipped while not streaming (attach, resync after outage/pause, unsure epochs): expected */
    uint64_t epochs_dropped;     /**< whole epochs dropped by the lag limit or a lap (each lowers Onocoy's data rate) */
    uint64_t epochs_out;         /**< epochs delivered whole (counted when the next epoch begins) */
    uint32_t max_lag_ms;         /**< this consumer's lag limit */
    uint32_t lag_drops;          /**< number of lag-limit events */
    uint32_t laps;               /**< number of times overwritten */
    uint32_t lag_ms;             /**< age of the oldest unread frame right now */
} rtcm_hub_consumer_stats_t;

/** Allocate the ring (PSRAM) and synchronisation objects. */
esp_err_t rtcm_hub_init(void);

/**
 * Publish one complete RTCM3 frame (header + payload + CRC).
 *
 * Never blocks, allocates or logs. A rejected frame is counted in
 * publish_failures. Ages, consumer lag, last_frame_age_ms and the quiet line
 * of a forced rtcm_hub_hold() are measured from the frame's arrival, not
 * from the publish, so a frame that waited in the UART ring counts as old
 * as it is.
 *
 * @param msg_type RTCM3 message number (epoch rules, sticky copies).
 * @param arrival_us esp_timer time the frame's last byte arrived.
 * @param arrival_exact the UART interrupt's time (else a read time, which
 *        the epoch gap rule ignores).
 * @return ESP_OK, ESP_ERR_INVALID_ARG if len exceeds a maximum RTCM3 frame
 *         (or frame is NULL or shorter than an empty frame),
 *         ESP_ERR_INVALID_STATE before init.
 */
esp_err_t rtcm_hub_publish(const uint8_t *frame, size_t len, uint16_t msg_type, int64_t arrival_us,
                           bool arrival_exact);

/**
 * Attach a consumer: sticky metadata first, then live data from the next
 * epoch start (no backlog replay).
 *
 * @param name short label for logs/stats (copied).
 * @return ESP_ERR_NO_MEM when CONFIG_RTCM_HUB_MAX_CONSUMERS are attached.
 */
esp_err_t rtcm_hub_attach(const char *name, rtcm_hub_consumer_t *out);

/** Detach and free a consumer. NULL is ignored. */
void rtcm_hub_detach(rtcm_hub_consumer_t consumer);

/**
 * Copy as many whole frames as fit into `buf`, waiting up to timeout_ms for
 * the first one. While the hub is held, only frames before the hold point;
 * after rtcm_hub_finish_epoch(), only the rest of the epoch being sent.
 *
 * @param max_len must be >= RTCM_HUB_MIN_READ_BUF.
 * @param out_len bytes written (whole frames only; 0 on timeout).
 * @return ESP_OK with *out_len > 0, ESP_ERR_TIMEOUT if nothing arrived,
 *         ESP_ERR_INVALID_STATE at once when the hub is held and this
 *         consumer has passed every frame before the hold point (nothing
 *         more until rtcm_hub_release()) or when it was finishing its epoch
 *         and that epoch is over (nothing more until rtcm_hub_resync()),
 *         ESP_ERR_INVALID_SIZE if max_len < RTCM_HUB_MIN_READ_BUF (checked
 *         first, so a too-small buffer fails at once instead of forever).
 */
esp_err_t rtcm_hub_read(rtcm_hub_consumer_t consumer, uint8_t *buf, size_t max_len,
                        size_t *out_len, uint32_t timeout_ms);

/**
 * Re-position a consumer as on attach (sticky metadata, then the next epoch
 * start), e.g. after a caster reconnects. Skipped frames count as idle skips.
 * Ends rtcm_hub_finish_epoch().
 */
esp_err_t rtcm_hub_resync(rtcm_hub_consumer_t consumer);

/**
 * Finish the epoch this consumer is sending, before its reader stops (a
 * caster session about to close for a reboot, new settings or a restart):
 * rtcm_hub_read() then delivers the rest of that epoch and nothing of the
 * next one (no sticky copy either), and returns ESP_ERR_INVALID_STATE once
 * it is over: at the next epoch's first frame, or, with every frame read,
 * as soon as the newest epoch is that one and needs nothing more (its last
 * MSM is in, or it holds only its leading station messages so far). Never
 * by a quiet line: the caller bounds the wait (an epoch whose last MSM was
 * lost ends only when the next one starts). A consumer that is not inside
 * an epoch gets ESP_ERR_INVALID_STATE at once. The consumer's task;
 * rtcm_hub_resync() ends it.
 */
esp_err_t rtcm_hub_finish_epoch(rtcm_hub_consumer_t consumer);

/**
 * Set a consumer's lag limit (the Onocoy slot uses 1000 ms; others keep
 * CONFIG_RTCM_HUB_MAX_LAG_MS). The limit bounds queueing in the hub only.
 * @return ESP_ERR_INVALID_ARG for ms outside 100..60000.
 */
esp_err_t rtcm_hub_set_max_lag(rtcm_hub_consumer_t consumer, uint32_t ms);

/**
 * Forget the sticky metadata copies and start a new epoch at the next frame.
 * The station calls it on every framer reset (the GNSS link's on_reset sink)
 * and whenever the receiver was reconfigured (lease release). Producer
 * context only (same task as rtcm_hub_publish).
 */
void rtcm_hub_clear_sticky(void);

/** Hub-wide counters. */
void rtcm_hub_get_stats(rtcm_hub_stats_t *out);

/** Where rtcm_hub_hold() put the hold point. */
typedef enum {
    RTCM_HUB_HOLD_NONE = 0,   /**< not placed: an epoch is in flight (try again, or force) */
    RTCM_HUB_HOLD_BOUNDARY,   /**< between two epochs */
    RTCM_HUB_HOLD_QUIET,      /**< forced: after an open epoch on a quiet line (its last MSM never came) */
    RTCM_HUB_HOLD_CUT,        /**< forced while an epoch was in flight: at that epoch's first frame */
} rtcm_hub_hold_t;

/**
 * Hold the stream for a pause (the station, before a receiver session; any
 * task). The point is chosen together with the epoch state, so a frame being
 * stored at that moment lands on the right side of it:
 *  - after the newest epoch when its last MSM is in (or nothing came yet, or
 *    the next frame starts an epoch anyway: a framer reset);
 *  - at the newest epoch's first frame while it holds only its leading
 *    station messages (1005/1033/1230 come before its first MSM). A caster
 *    that already sent some of them stops where it is: station messages
 *    are never part of an MSM run;
 *  - while the newest epoch's MSMs announced more, never on its own: the
 *    epoch is in flight however quiet the line is (its rest may come late),
 *    RTCM_HUB_HOLD_NONE and no hold. With `force` (the caller waited long
 *    enough): after it when its newest frame is `quiet_ms` or more old and
 *    nothing is `arriving` (its last MSM was lost, or the receiver never
 *    clears the bit: RTCM_HUB_HOLD_QUIET), else at its first frame
 *    (RTCM_HUB_HOLD_CUT), so no caster starts it, and one already inside it
 *    ends there.
 * `arriving`: bytes the producer received and has not published yet (the
 * caller reads gnss_link_rx_unread(), which the hub cannot see): the reader
 * is behind, not the line quiet, so a forced hold never goes after an open
 * epoch then. The other placements are positions that hold whatever comes
 * next, so it does not change them. A hold already set is kept and its kind
 * returned. Without a hub (init failed): RTCM_HUB_HOLD_BOUNDARY, nothing to
 * hold.
 */
rtcm_hub_hold_t rtcm_hub_hold(uint32_t quiet_ms, bool force, bool arriving);

/**
 * End the hold (any task; nothing if none is set): every consumer starts over
 * at its next read, as after rtcm_hub_resync() (skipped frames are idle
 * skips), so nothing published from the hold point on is ever sent.
 */
void rtcm_hub_release(void);

/** A hold is set. Any task. */
bool rtcm_hub_held(void);

/**
 * True once after a read in which the hub started this consumer over at the
 * end of a hold (the consumer's task): the data of that read begins with the
 * sticky copies and a whole epoch, so per-epoch bookkeeping starts afresh.
 */
bool rtcm_hub_restarted(rtcm_hub_consumer_t consumer);

/** Counters for one consumer. */
esp_err_t rtcm_hub_get_consumer_stats(rtcm_hub_consumer_t consumer, rtcm_hub_consumer_stats_t *out);

#ifdef __cplusplus
}
#endif
