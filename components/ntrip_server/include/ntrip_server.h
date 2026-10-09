/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Multi-caster NTRIP server (source side: pushes RTCM to casters).
 *
 * One task per caster slot, created once at boot and never deleted (enable,
 * disable and config changes are handled by the slot's state machine, so
 * months of reconfiguring never fragment the heap). Each slot attaches its
 * own rtcm_hub consumer, connects (TCP or TLS), sends the source request
 * (NTRIP 1.0 "SOURCE" or NTRIP 2.0 HTTP POST), waits for the caster's OK and
 * then streams whole RTCM3 frames. Request and transmit buffers are
 * allocated once per slot. Slots are fully independent: one caster's outage,
 * auth failure or slowness never affects another.
 *
 * Bounded blocking: every blocking call has a limit (connect and TLS
 * handshake via esp_tls_cfg_t.timeout_ms, SO_SNDTIMEO/SO_RCVTIMEO on the
 * socket, lwIP's resolver gives up after ~7 s per DNS server). Slot tasks are
 * NOT subscribed to the task watchdog: a slow caster must never reboot the
 * station. Instead each slot stamps its progress; the health monitor
 * restarts only the slot that overruns its per-state budget
 * (CONNECTING <= CONFIG_NTRIP_CONNECT_BUDGET_S, a single send <= the send
 * timeout + 5 s) via ntrip_server_restart_slot().
 *
 * Clean shutdown: a lifecycle shutdown hook closes every session (TLS
 * close_notify / shutdown(SHUT_WR), bounded) before any planned reboot, so
 * the caster does not hold a stale source session. While streaming, the
 * socket is also polled for EOF so a caster-side close is seen at once.
 *
 * Rejections: NTRIP 1.0 has no standard "mountpoint in use" reply and
 * casters word errors differently, so a rejection right after a reboot or
 * disconnect may just be the caster still holding the previous session. A
 * rejection is treated as transient (retry every CONFIG_NTRIP_AUTH_RETRY_S)
 * when the same credentials have worked before, or within
 * CONFIG_NTRIP_AUTH_TRANSIENT_WINDOW_S of a disconnect or boot. Only after
 * that window does the slot show AUTH_FAILED, and even then it keeps
 * retrying every CONFIG_NTRIP_AUTH_FAILED_RETRY_S.
 *
 * Protected slot: the Onocoy slot is "protected": its availability drives
 * the daily downtime budget and the network recovery ladder, and its
 * reconnect backoff is capped at CONFIG_NTRIP_PROTECTED_MAX_BACKOFF_S.
 *
 * Pause: while the station holds the receiver for a short command session,
 * slots stay connected but idle; only a pause longer than
 * CONFIG_NTRIP_PAUSE_DISCONNECT_S (or a receiver flash) disconnects them.
 *
 * Logging: request lines carry the password (NTRIP 1.0) and the Onocoy
 * mountpoint is the credential username, so requests and replies are only
 * ever logged redacted (host and slot name; never mountpoint, user or
 * password).
 *
 * Onocoy: servers.onocoy.com:2101, NTRIP 1.0, mountpoint = credential
 * username, password = credential password.
 *
 * Safe mode skips ntrip_server_init() and _start(): no slot runs. Then
 * ntrip_server_get_status() still works (every slot DISABLED) and
 * ntrip_server_slot_count() is valid, but the config calls return
 * ESP_ERR_INVALID_STATE, so nothing is written over stored slots that were
 * never loaded.
 *
 * Status: PROVISIONAL. Request formats, reply parsing, reconnect/backoff and
 * caster error handling are still to be designed in detail. The config and
 * status structures are expected to hold.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(NTRIP_EVENT);

/** NTRIP_EVENT ids; event data is the slot index (size_t). */
typedef enum {
    NTRIP_EVENT_STATE_CHANGED,
} ntrip_event_t;

typedef enum {
    NTRIP_PROTO_V1 = 1,   /**< NTRIP 1.0: SOURCE <password> /<mount> */
    NTRIP_PROTO_V2 = 2,   /**< NTRIP 2.0: HTTP/1.1 POST with Basic auth */
} ntrip_proto_t;

/** One caster slot (persisted as one blob in settings namespace "cst<N>"). */
typedef struct {
    bool enabled;
    bool protected_slot;   /**< counts toward the daily availability budget (Onocoy) */
    char name[24];         /**< UI label */
    char host[64];
    uint16_t port;
    char mountpoint[48];
    char username[48];     /**< NTRIP 2.0 only */
    char password[64];     /**< secret: never returned to the web UI */
    bool password_set;     /**< output only: a password is stored */
    bool clear_password;   /**< input only (set_config): remove the stored password */
    ntrip_proto_t proto;
    bool tls;
} ntrip_caster_cfg_t;

typedef enum {
    NTRIP_STATE_DISABLED = 0,
    NTRIP_STATE_WAIT_NETWORK,   /**< no IP yet */
    NTRIP_STATE_WAIT_DATA,      /**< connected network, but no RTCM to send */
    NTRIP_STATE_CONNECTING,     /**< DNS, TCP/TLS connect, request, reply */
    NTRIP_STATE_STREAMING,
    NTRIP_STATE_BACKOFF,        /**< waiting before the next attempt */
    NTRIP_STATE_AUTH_FAILED,    /**< rejected past the transient window; still retried slowly */
    NTRIP_STATE_PAUSED,         /**< station paused output (receiver config/flash) */
} ntrip_state_t;

/** Live status of one slot. */
typedef struct {
    ntrip_state_t state;
    uint32_t state_age_s;       /**< seconds in the current state */
    uint32_t progress_age_ms;   /**< since the slot task last made progress (liveness) */
    uint32_t connects;          /**< successful connections */
    uint32_t failures;          /**< failed attempts */
    uint32_t disconnects;       /**< drops after streaming */
    uint32_t restarts;          /**< forced by the health monitor */
    uint64_t bytes_sent;
    uint64_t frames_dropped;    /**< lost while streaming (hub slow-consumer policy) */
    uint32_t connected_s;       /**< seconds in the current session */
    uint32_t next_retry_s;      /**< seconds until the next attempt (BACKOFF/AUTH_FAILED) */
    int last_status_code;       /**< last caster HTTP/ICY status, 0 = none */
    esp_err_t last_err;
} ntrip_caster_status_t;

/** Load slot configs from settings, create the slot tasks (idle), register the shutdown hook. */
esp_err_t ntrip_server_init(void);

/** Let enabled slots connect. */
esp_err_t ntrip_server_start(void);

/** Close all sessions (slots stay created, idle). */
esp_err_t ntrip_server_stop(void);

/** Number of slots (CONFIG_NTRIP_MAX_CASTERS). */
size_t ntrip_server_slot_count(void);

/**
 * Read a slot's config.
 * @param with_secret false blanks the password (for anything shown to users);
 *                    password_set still tells whether one is stored.
 * @return ESP_ERR_INVALID_STATE before ntrip_server_init() (safe mode).
 */
esp_err_t ntrip_server_get_config(size_t slot, ntrip_caster_cfg_t *out, bool with_secret);

/**
 * Validate, persist (one blob, replaced as a unit) and apply a slot config;
 * only that slot reconnects.
 *
 * Password: a non-empty one replaces the stored one; clear_password removes
 * it. An empty password keeps the stored one (the UI never sees it, so it
 * cannot send it back) only while the host stays the same: pointed at
 * another host, the slot never sends the old caster's password there.
 * @return ESP_ERR_INVALID_STATE before ntrip_server_init() (safe mode).
 */
esp_err_t ntrip_server_set_config(size_t slot, const ntrip_caster_cfg_t *cfg);

/** Live status of a slot. Works before ntrip_server_init() (every slot DISABLED). */
esp_err_t ntrip_server_get_status(size_t slot, ntrip_caster_status_t *out);

/** Drop and reconnect one slot (health monitor, after a budget overrun). */
esp_err_t ntrip_server_restart_slot(size_t slot);

/**
 * Pause/resume all slots for receiver work. Paused slots stay connected and
 * idle for up to CONFIG_NTRIP_PAUSE_DISCONNECT_S, then disconnect cleanly;
 * `disconnect_now` (receiver flash) skips the idle phase.
 */
esp_err_t ntrip_server_set_paused(bool paused, bool disconnect_now);

/** Fill a slot config with the Onocoy endpoint (credentials left empty, protected). */
void ntrip_server_onocoy_defaults(ntrip_caster_cfg_t *out);

#ifdef __cplusplus
}
#endif
