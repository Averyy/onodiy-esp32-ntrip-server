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
 *                                 casters, health, alerts (health's and the
 *                                 station's), and the update banner
 *                                 update: {app_available, app_version,
 *                                 gnss_available, gnss_build, gnss_blocked_reason,
 *                                 gnss_blocked_left_s}. With public status
 *                                 enabled and no session: slot names and
 *                                 states only (never host, mountpoint or user)
 *   GET  /api/gnss                receiver info, profile, base position
 *   PUT  /api/gnss/profile        edit base profile; POST /api/gnss/apply
 *   POST /api/gnss/position/lock  lock the surveyed position; DELETE re-surveys
 *   GET  /api/gnss/signals        bands/CNR per constellation, observed RTCM set and rates
 *   GET  /api/gnss/flash          open receiver-flash journal, last 4 KB of
 *                                 receiver text, the actions allowed now
 *   POST /api/gnss/flash/retry, /api/gnss/flash/restart-receiver,
 *        /api/gnss/flash/clear    body {"confirm": true}, each with its warning;
 *                                 only clear in safe mode (and nothing when the
 *                                 settings are unavailable); retry refused
 *                                 during an app trial or an app update;
 *                                 restart-receiver also for a receiver found at
 *                                 its loader without an update session
 *   GET  /api/sky                 satellites in view (azimuth/elevation)
 *   GET  /api/casters             all slots (passwords never returned); status
 *                                 per the caster status fields
 *   PUT  /api/casters/{n}         edit slot n (empty password keeps the old one)
 *   GET  /api/wifi                station status; POST /api/wifi/scan (warns:
 *   PUT  /api/wifi                interrupts streaming); GET returns cached results
 *   GET  /api/update              versions, trial state, rollback availability,
 *                                 rolled-back versions
 *   POST /api/update/check        fetch the manifest now (never installs)
 *   POST /api/update/app          body {"version": "<offered>"}: the user clicked
 *                                 Update (409 if the manifest changed, during a
 *                                 receiver-flash lockout, or while another
 *                                 update runs)
 *   POST /api/update/gnss         body {"build": <offered>, "cross_layout_ack":
 *                                 false} (409 for every blocked reason: app trial
 *                                 or app update running, journal open, receiver
 *                                 not streaming, unsupported model, safe mode):
 *                                 the user confirmed the receiver update
 *                                 dialog (expected downtime, today's remaining
 *                                 Onocoy budget, "do not power off or plug into
 *                                 the receiver's USB port", "the station profile
 *                                 will be re-applied")
 *   POST /api/update/rollback
 *   POST /api/upload/app          raw .bin body -> OTA slot
 *   POST /api/upload/gnss         raw .pkg body -> pkg_store; returns the package
 *                                 facts (SHA-256, size, model string, layout);
 *                                 refused (409) in safe mode and for every
 *                                 blocked reason above, before anything is stored
 *   POST /api/upload/gnss/confirm body {"sha256": ..., "expected_fw": ...,
 *                                 "cross_layout_ack": ...}: starts the flash
 *                                 (station_request_flash repeats every check)
 *   DELETE /api/upload/gnss       cancel: releases the staged package (also
 *                                 released when unconfirmed after 15 min)
 *   GET  /api/health, /api/availability, /api/diag/tasks
 *   GET  /api/logs?cursor=N[&boot=prev], /api/journal?cursor=N
 *   GET  /api/coredump            download (admin; may contain secrets); DELETE to erase
 *   POST /api/system/reboot, /api/system/factory-reset, /api/system/safe-mode/clear
 *                                 (refused with 409 while a reboot is inhibited)
 *   POST /api/session             log in (password -> session cookie);
 *   DELETE /api/session           log out; PUT /api/admin-password
 *   GET  /ws                      WebSocket: 1 Hz status deltas, log lines,
 *                                 update/flash progress
 *
 * Update banner: shown on every page of an admin session when the daily
 * check (or "check now") found a newer app or receiver build: "App vX /
 * receiver build Y available - Update / Later". "Later" hides it in that
 * browser until a newer version appears (browser-side state only). Nothing is
 * downloaded before the user clicks Update.
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
