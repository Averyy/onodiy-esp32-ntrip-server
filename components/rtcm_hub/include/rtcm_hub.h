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
 *    the bit), (c) at any frame arriving CONFIG_RTCM_HUB_BURST_GAP_MS or more
 *    after the previous one while the current epoch holds an MSM not yet
 *    terminated (a lost terminating MSM), and at the first frame after
 *    rtcm_hub_clear_sticky(). Non-MSM frames after an epoch's last MSM
 *    belong to the next epoch. Epoch times are compared within one
 *    constellation only, so no time-system conversion is needed.
 *  - Station metadata: the latest 1005/1006, 1033 and 1230 frames are kept
 *    as sticky copies outside the ring. They are cleared on every framer
 *    reset and every receiver reconfiguration (rtcm_hub_clear_sticky), and a
 *    copy older than 30 s is never replayed, so a superseded position is
 *    never sent.
 *  - A new consumer, and one that calls rtcm_hub_resync() (e.g. after a
 *    caster reconnect), first receives the sticky metadata frames, then live
 *    data starting at the next epoch start. A caster never starts with the
 *    tail of an epoch or waits for the station position. Epoch starts that
 *    are already over the consumer's lag limit when it reads are skipped too
 *    (as idle skips).
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
 * (before attach, during resync after an outage or pause) are expected and
 * go to `frames_skipped_idle`. Only frames lost while streaming (lapped or
 * over the lag limit) count as `frames_dropped`, the real data-loss figure;
 * after a lap, the rest of the newest epoch skipped while resyncing counts as
 * dropped too. Every frame published while a consumer is attached ends up in
 * exactly one of frames_out, frames_dropped or frames_skipped_idle
 * (frames_out also counts the sticky replays).
 *
 * Thread safety: rtcm_hub_publish() and rtcm_hub_clear_sticky() from one task
 * (the GNSS link RX task); every consumer handle used by exactly one task for
 * read, resync, set_max_lag and detach. The stats getters may be called from
 * any task.
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
    uint64_t frames_skipped_idle;/**< skipped while not streaming (attach, resync after outage/pause): expected */
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
 * publish_failures.
 *
 * @param msg_type RTCM3 message number (epoch rules, sticky copies).
 * @return ESP_OK, ESP_ERR_INVALID_ARG if len exceeds a maximum RTCM3 frame
 *         (or frame is NULL or shorter than an empty frame),
 *         ESP_ERR_INVALID_STATE before init.
 */
esp_err_t rtcm_hub_publish(const uint8_t *frame, size_t len, uint16_t msg_type);

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
 * the first one.
 *
 * @param max_len must be >= RTCM_HUB_MIN_READ_BUF.
 * @param out_len bytes written (whole frames only; 0 on timeout).
 * @return ESP_OK with *out_len > 0, ESP_ERR_TIMEOUT if nothing arrived,
 *         ESP_ERR_INVALID_SIZE if max_len < RTCM_HUB_MIN_READ_BUF (checked
 *         first, so a too-small buffer fails at once instead of forever).
 */
esp_err_t rtcm_hub_read(rtcm_hub_consumer_t consumer, uint8_t *buf, size_t max_len,
                        size_t *out_len, uint32_t timeout_ms);

/**
 * Re-position a consumer as on attach (sticky metadata, then the next epoch
 * start), e.g. after a caster reconnects. Skipped frames count as idle skips.
 */
esp_err_t rtcm_hub_resync(rtcm_hub_consumer_t consumer);

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

/** Counters for one consumer. */
esp_err_t rtcm_hub_get_consumer_stats(rtcm_hub_consumer_t consumer, rtcm_hub_consumer_stats_t *out);

#ifdef __cplusplus
}
#endif
