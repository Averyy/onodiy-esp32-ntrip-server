/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Web UI and HTTP/JSON API (esp_http_server), plus one WebSocket for live data.
 *
 * Static UI: pre-compressed assets embedded in the app image, so UI and
 * firmware always match and update together through OTA.
 *
 * API (JSON; mutating calls need an authenticated session):
 *   GET  /api/status              summary: versions, uptime, network, receiver,
 *                                 casters, health, alerts. With public status
 *                                 enabled and no session: slot names and
 *                                 states only (never host, mountpoint or user)
 *   GET  /api/gnss                receiver info, profile, base position
 *   PUT  /api/gnss/profile        edit base profile; POST /api/gnss/apply
 *   POST /api/gnss/position/lock  lock the surveyed position; DELETE re-surveys
 *   GET  /api/gnss/signals        bands/CNR per constellation, observed RTCM set and rates
 *   GET  /api/sky                 satellites in view (azimuth/elevation)
 *   GET  /api/casters             all slots (passwords never returned)
 *   PUT  /api/casters/{n}         edit slot n (empty password keeps the old one)
 *   GET  /api/wifi                station status; POST /api/wifi/scan (warns:
 *   PUT  /api/wifi                interrupts streaming); GET returns cached results
 *   GET  /api/update              versions, trial state; POST /api/update/check,
 *                                 /api/update/app, /api/update/gnss, /api/update/rollback
 *   POST /api/upload/app          raw .bin body -> OTA slot
 *   POST /api/upload/gnss         raw .pkg body -> pkg_store (then confirm flash)
 *   GET  /api/health, /api/availability, /api/diag/tasks
 *   GET  /api/logs?cursor=N[&boot=prev], /api/journal?cursor=N
 *   GET  /api/coredump            download (admin; may contain secrets); DELETE to erase
 *   POST /api/system/reboot, /api/system/factory-reset, /api/system/safe-mode/clear
 *                                 (refused with 409 while a reboot is inhibited)
 *   POST /api/session             log in (password -> session cookie);
 *   DELETE /api/session           log out; PUT /api/admin-password
 *   GET  /ws                      WebSocket: 1 Hz status deltas, log lines,
 *                                 update/flash progress
 * Setup AP only: captive-portal probe URLs redirect to the setup page; the
 * first-run wizard sets the admin password first, then Wi-Fi and Onocoy
 * credentials.
 *
 * Request checks: API requests whose Host header is not the device's IP or
 * hostname are rejected (DNS rebinding); mutating calls need the session
 * cookie (SameSite=Strict), a JSON or upload content type and a matching
 * Origin when one is sent. The admin password is stored as salted
 * PBKDF2-HMAC-SHA256 computed with PSA key derivation.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Prepare handlers and session store. */
esp_err_t web_init(void);

/** Start the HTTP server and register all URIs. */
esp_err_t web_start(void);

/** Stop the HTTP server (closes WebSocket clients). */
esp_err_t web_stop(void);

/**
 * Queue a JSON message for every connected WebSocket client subscribed to
 * `topic` ("status", "log", "progress"). Non-blocking; drops if queues are full.
 */
esp_err_t web_ws_publish(const char *topic, const char *json);

#ifdef __cplusplus
}
#endif
