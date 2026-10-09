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
 *  - Epochs: MSM frames are tagged with their epoch time and the public MSM
 *    "multiple message" bit, so the hub knows where each epoch starts.
 *  - Station metadata: the latest 1005/1006, 1033 and 1230 frames are kept
 *    as sticky copies outside the ring.
 *  - A new consumer, and one that calls rtcm_hub_resync() (e.g. after a
 *    caster reconnect), first receives the sticky metadata frames, then live
 *    data starting at the next epoch start. A caster never starts with the
 *    tail of an epoch or waits 10 s for the station position.
 *  - A consumer whose oldest unread frame is older than
 *    CONFIG_RTCM_HUB_MAX_LAG_MS drops whole oldest epochs until its lag is
 *    back under the limit (not a jump to the very newest frame, which would
 *    throw away still-valid data). A consumer that was overwritten ("lapped")
 *    resyncs as above.
 *  - Reads return whole frames only, so a caster never sends a partial frame.
 *
 * Ring and concurrency (single producer):
 *  - Records {u16 len, u16 msg_type, u32 seq, u32 rx_ms, u32 epoch} + frame,
 *    padded to 4, never wrapping (a wrap marker sends the record to offset 0).
 *    Offsets are monotonic 64-bit byte counts.
 *  - The producer first publishes reserve_off = write_off + record length
 *    (including any wrap waste) under a short spinlock, copies the record,
 *    then publishes write_off. Consumers validate their read offset against
 *    reserve_off before parsing any header and again after copying, so bytes
 *    being overwritten are never trusted; a header's len is clamped to
 *    GNSS_RTCM3_MAX_FRAME before use.
 *  - Wake-up uses task notification index 1
 *    (CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES >= 2), so it never
 *    collides with IDF APIs on index 0. Attach and detach take the hub lock;
 *    detach clears the consumer's task handle before freeing it, so the
 *    producer never notifies a freed task.
 *  - rx_ms is a wrapping millisecond stamp; it is only ever compared as
 *    (uint32_t)(now - then).
 *
 * Loss accounting: frames a consumer skips while it is not streaming
 * (before attach, during resync after an outage or pause) are expected and
 * go to `frames_skipped_idle`. Only frames lost while streaming (lapped or
 * over the lag limit) count as `frames_dropped`, the real data-loss figure.
 *
 * Thread safety: rtcm_hub_publish() from one task (the GNSS link RX task);
 * every consumer handle used by exactly one task.
 *
 * Status: API settled; ring not implemented yet. Limits are provisional
 * until measured on the bench.
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
    uint64_t epochs_in;          /**< MSM epochs started */
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
    uint32_t lag_drops;          /**< number of lag-limit events */
    uint32_t laps;               /**< number of times overwritten */
    uint32_t lag_ms;             /**< age of the oldest unread frame right now */
} rtcm_hub_consumer_stats_t;

/** Allocate the ring (PSRAM) and synchronisation objects. */
esp_err_t rtcm_hub_init(void);

/**
 * Publish one complete RTCM3 frame (header + payload + CRC).
 *
 * @param msg_type RTCM3 message number, kept per frame for statistics.
 * @return ESP_OK, ESP_ERR_INVALID_ARG if len exceeds a maximum RTCM3 frame,
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

/** Hub-wide counters. */
void rtcm_hub_get_stats(rtcm_hub_stats_t *out);

/** Counters for one consumer. */
esp_err_t rtcm_hub_get_consumer_stats(rtcm_hub_consumer_t consumer, rtcm_hub_consumer_stats_t *out);

#ifdef __cplusplus
}
#endif
