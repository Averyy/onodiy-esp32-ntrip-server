/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Monitor: how the station's connection and the ESP32 itself are doing, the
 * latest reading and the running 24 h, for the Status page.
 *
 * Every MONITOR_SAMPLE_S one sample: a ping (ICMP echo, 32 bytes, one try,
 * MONITOR_PING_TIMEOUT_MS) to the router (the station's IPv4 gateway) and
 * one to the server the casters depend on most (the Onocoy slot's address
 * while it is on and connected, else the first enabled slot's: the address
 * that slot's own lookup found, so no name lookup of its own), the Wi-Fi
 * signal, the busier core's CPU load over the sample (from the idle tasks'
 * run-time counters: no task list is scanned) and the free internal memory.
 * A ping without a target (offline, no server connected yet) is no sample
 * of it; one without an answer counts as lost.
 *
 * Load: two echo requests and their replies per sample, about 4 bytes a
 * second; one low-priority task asleep between samples, its stack and the
 * 24 h history in PSRAM; nothing goes to a caster's connection.
 *
 * The samples are summed into MONITOR_BUCKET_S buckets, MONITOR_BUCKETS of
 * them (24 h, the current one included), in RAM: a restart starts the
 * history again. Not started in safe mode.
 *
 * The types below have no IDF dependency (the host tests build the JSON
 * from them); monitor_core.c sums them.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MONITOR_SAMPLE_S         30u
#define MONITOR_BUCKET_SAMPLES   10u                                       /* 5 min */
#define MONITOR_BUCKET_S         (MONITOR_SAMPLE_S * MONITOR_BUCKET_SAMPLES)
#define MONITOR_BUCKETS          288u                                      /* 24 h */
#define MONITOR_PING_TIMEOUT_MS  2000u

/** One ping's outcome. */
typedef enum {
    MONITOR_PING_NONE = 0,   /**< no target (offline, no server connected): not a sample */
    MONITOR_PING_OK,
    MONITOR_PING_LOST,       /**< no reply within MONITOR_PING_TIMEOUT_MS */
} monitor_ping_t;

/** One sample. */
typedef struct {
    monitor_ping_t router;
    uint16_t router_ms;      /**< with MONITOR_PING_OK */
    monitor_ping_t server;
    uint16_t server_ms;
    bool rssi_known;         /**< associated with an access point */
    int8_t rssi;             /**< dBm */
    bool cpu_known;          /**< false for the first sample (no interval yet) */
    uint8_t cpu_pct;         /**< the busier core's load over the sample, 0-100 */
    uint32_t mem_free;       /**< free internal memory, bytes */
} monitor_sample_t;

/** A ping's sums. */
typedef struct {
    uint16_t sent;           /**< samples with a target */
    uint16_t ok;
    uint32_t sum_ms;         /**< over the replies */
    uint16_t max_ms;
} monitor_ping_acc_t;

/** One bucket (MONITOR_BUCKET_SAMPLES samples; the current one fewer). */
typedef struct {
    uint16_t samples;
    monitor_ping_acc_t router;
    monitor_ping_acc_t server;
    uint16_t rssi_n;
    int32_t rssi_sum;
    int8_t rssi_min;         /**< the weakest */
    uint16_t cpu_n;
    uint32_t cpu_sum;
    uint8_t cpu_max;
    uint32_t mem_min;        /**< UINT32_MAX = none */
} monitor_bucket_t;

/** A ping over the running 24 h. */
typedef struct {
    uint32_t sent;
    uint32_t lost;
    uint32_t avg_ms;         /**< over the replies; 0 without one */
    uint32_t max_ms;
} monitor_ping_sum_t;

/** The latest sample and the running 24 h. */
typedef struct {
    bool running;            /**< the monitor runs (not in safe mode, started) */
    bool have_last;
    monitor_sample_t last;
    uint32_t span_s;         /**< time the 24 h figures cover: samples x MONITOR_SAMPLE_S, at most 24 h */
    char server_name[24];    /**< the server pinged by the last sample with a server target ("" = none yet) */
    monitor_ping_sum_t router;
    monitor_ping_sum_t server;
    bool rssi_known;
    int8_t rssi_avg;
    int8_t rssi_min;
    bool cpu_known;
    uint8_t cpu_avg;
    uint8_t cpu_max;
    bool mem_known;
    uint32_t mem_min;
} monitor_status_t;

/** Start the sampling task. Call once, after the network and the casters. */
esp_err_t monitor_start(void);

/** The latest sample and the running 24 h (running false before monitor_start or when it failed). */
void monitor_get_status(monitor_status_t *out);

/**
 * The buckets, oldest first, the current (unfinished) one last: at most
 * `max`, the newest ones. *n gets how many were written.
 */
void monitor_get_series(monitor_bucket_t *out, size_t max, size_t *n);

#ifdef __cplusplus
}
#endif
