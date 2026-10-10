/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Web UI and HTTP/JSON API (esp_http_server), plus one WebSocket for live
 * data. The contract between this server and the browser UI is
 * components/web/API.md: every endpoint, field, error code and limit is
 * defined there.
 *
 * Static UI: pre-compressed assets embedded in the app image (every file in
 * components/web/ui/, packed at build time), so UI and firmware always match
 * and update together.
 *
 * Request checks on every request: the Host header must name the device
 * (DNS rebinding; on the setup AP a foreign Host is the captive portal),
 * mutating calls need a JSON or upload content type and a matching Origin
 * when one is sent, and everything beyond the public status needs the admin
 * session cookie (SameSite=Strict). One admin account, no default password:
 * the first password is set over the device's own AP, stored as salted
 * PBKDF2-HMAC-SHA256 computed with PSA key derivation. Login attempts are
 * throttled.
 *
 * Every update is started by the user: the daily check fills the banner of
 * /api/status, and nothing is downloaded before the user's Update click (or
 * upload).
 *
 * Server: 7 sockets of the lwIP budget, 64 URI handlers, 8 KB stack in
 * internal RAM; uploads and downloads run on a worker task with an
 * internal-RAM stack (flash writes); large snapshots and JSON buffers live
 * in PSRAM. The WebSocket pushes 1 Hz status deltas, events, job progress
 * and the log to at most 4 clients, through bounded per-client queues that
 * never block a producer.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Load the admin password and the web settings, register event handlers, create the worker. */
esp_err_t web_init(void);

/** Start the HTTP server and register all URIs (after network_init: the request checks use its interfaces). */
esp_err_t web_start(void);

/** Stop the HTTP server (closes WebSocket clients). */
esp_err_t web_stop(void);

/**
 * Queue a complete JSON message for every connected WebSocket client
 * subscribed to `topic` ("status", "log", "progress"; the last two reach
 * admin clients only). Non-blocking and copied; dropped by the per-client
 * queue rules when a client is slow.
 * @return ESP_ERR_INVALID_ARG for an unknown topic, ESP_ERR_INVALID_STATE
 *         while the server is stopped.
 */
esp_err_t web_ws_publish(const char *topic, const char *json);

#ifdef __cplusplus
}
#endif
