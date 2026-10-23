/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Local NTRIP caster: the station serves its own RTCM to the owner's rover
 * on the home network ("Your rover" in the web UI), with no internet needed.
 *
 * Protocol: NTRIP 1.0 and 2.0 clients (GET /<mountpoint> with Basic
 * authentication) on TCP port CONFIG_LOCAL_CASTER_PORT (2101). NTRIP 2.0
 * over HTTP/1.1 gets chunked data, NTRIP 2.0 over HTTP/1.0 raw data with an
 * HTTP/1.0 status line, and a request without "Ntrip-Version: Ntrip/2.0" is
 * NTRIP 1.0 and gets "ICY 200 OK". GET / and any other mountpoint get the
 * source table (one STR record, no login needed, no position). One
 * mountpoint, one username and password, at most LOCAL_CASTER_MAX_CLIENTS
 * rovers at once (503 beyond). Off by default.
 *
 * Who may connect (checked at accept, before a byte is read; anything else
 * is closed at once without a reply): a peer whose address is private,
 * link-local or loopback (IPv4 10/8, 172.16/12, 192.168/16, 169.254/16,
 * 127/8; IPv6 fc00::/7, fe80::/10, ::1; an IPv4-mapped IPv6 address is
 * judged as its IPv4 address), that is not the station's IPv4 gateway (a
 * router that masquerades a port forward shows up as itself; the QEMU build
 * skips this rule because its host connections all come from its gateway),
 * and that does not come in over the setup hotspot: its local address is not
 * the hotspot's, and while the hotspot is up its IPv4 address is not in the
 * hotspot's subnet (the station takes IPv4 packets for any of its addresses
 * on either interface, so a hotspot client could reach the home-network
 * address; over IPv6 the station has link-local addresses only, which it
 * takes only on their own interface). 100.64/10 (carrier-grade NAT) does
 * not count as a home network.
 *
 * Connections: a connection that sends nothing within 2 s is closed, and
 * when all LOCAL_CASTER_MAX_CONNS are taken a new one takes the place of
 * one that cannot be a rover's (one lingering after its answer, one that
 * has sent nothing, the same client's earlier request, a request of a client
 * holding another connection), so idle connections cannot keep a rover out.
 *
 * Requests: one from a web browser (an Origin or Sec-Fetch-* header) gets
 * 403. That is defence in depth only: a browser sends neither header on
 * every request (no Origin on a plain cross-site GET, no Sec-Fetch-* to a
 * plain-HTTP address), and the login is what protects the stream. Wrong
 * logins are throttled per address and in total, as the web Login is (5
 * free, then 2^(n-4) s up to 300 s; 30 in a burst, then one per 120 s); a
 * throttled request gets 429 and its login is not checked. The addresses of
 * the last 4 successful logins (since boot) are exempt from the total, so
 * failures elsewhere never lock the owner's rover out.
 *
 * Data: one rtcm_hub consumer per streaming rover, attached after its login
 * and detached when its session ends, with lag limit
 * CONFIG_LOCAL_CASTER_MAX_LAG_MS: the sticky station messages first, then
 * whole epochs. A slow rover loses whole old epochs, and a stuck one (no byte
 * taken for LOCAL_CASTER_SEND_STALL_MS) is closed. Neither the producer, a
 * caster slot nor the web server ever waits for a rover. During a hold for
 * receiver work sessions stay connected and idle, and after it the hub
 * starts each one over at a whole epoch. With no RTCM (the receiver
 * stopped), sessions stay open, kept alive by TCP keepalive (10/5/3 s).
 *
 * Task: one task, created the first time the caster is turned on and never
 * deleted (its stack and buffers are in PSRAM; it does no flash work), runs
 * the listener and every connection on non-blocking sockets with select().
 * Sockets: one listener plus at most LOCAL_CASTER_MAX_CONNS connections.
 *
 * Shutdown: a lifecycle hook, registered after the casters' so they keep
 * their epoch drain, closes every session and the listener within 300 ms
 * before a planned restart. Safe mode skips init and start:
 * local_caster_get_status() reports LOCAL_CASTER_OFF, and
 * local_caster_get_config() and local_caster_set_config() return
 * ESP_ERR_INVALID_STATE.
 *
 * Settings: namespace "lcast", key "cfg" (one versioned blob; the password
 * is inside it, readable on the device and never returned by
 * local_caster_get_config()). Journal: DIAG_EV_ROVER_UP and
 * DIAG_EV_ROVER_DOWN (diag.h), at most 6 sessions an hour.
 *
 * Never logged: the Authorization header, the username or password a rover
 * sent, raw request lines, its User-Agent (the status has it), or anything a
 * rover sends after its request (such as its position in GGA sentences).
 * Refusal lines, and a rover's connect and end lines, are rate-limited.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOCAL_CASTER_MAX_CLIENTS        4u     /**< streaming rovers at once */
#define LOCAL_CASTER_MAX_CONNS          6u     /**< open connections at once: rovers plus requests being read or answered */
#define LOCAL_CASTER_MOUNT_LEN          48u    /**< mountpoint buffer: 1-47 of [A-Za-z0-9._-] */
#define LOCAL_CASTER_USER_LEN           32u    /**< username buffer: 1-31 of [A-Za-z0-9._-] */
#define LOCAL_CASTER_PASS_LEN           64u    /**< password buffer: 8-63 bytes of 0x21-0x7E */
#define LOCAL_CASTER_PASS_MIN           8u
#define LOCAL_CASTER_ADDR_LEN           46u    /**< address text */
#define LOCAL_CASTER_AGENT_LEN          48u    /**< a rover's User-Agent, printable ASCII, cut to 47 */
#define LOCAL_CASTER_REQUEST_MAX        1024u  /**< longest request (line and headers) */
#define LOCAL_CASTER_REQUEST_TIMEOUT_MS 5000u  /**< the whole request must arrive within this, counted from the accept */
#define LOCAL_CASTER_FIRST_BYTE_MS      2000u  /**< a connection that sends nothing for this long is closed (no reply) */
#define LOCAL_CASTER_SEND_STALL_MS      10000u /**< a rover that takes no byte of pending data for this long is closed */
#define LOCAL_CASTER_DEFAULT_USER       "rover"

/** What the caster does now. */
typedef enum {
    LOCAL_CASTER_OFF = 0,          /**< turned off, safe mode, or not initialised */
    LOCAL_CASTER_WAIT_NETWORK,     /**< on, but the station has no IPv4 address on the home network */
    LOCAL_CASTER_LISTENING,        /**< on and accepting rovers */
    LOCAL_CASTER_ERROR,            /**< on, but the port could not be opened (listen_errno); tried again every 10 s */
} local_caster_state_t;

/** Why a rover's session ended. Stored in the journal (DIAG_EV_ROVER_DOWN): never renumbered. */
typedef enum {
    LOCAL_CASTER_END_NONE = 0,
    LOCAL_CASTER_END_CLOSED,       /**< the rover closed the connection */
    LOCAL_CASTER_END_STALLED,      /**< the rover took no data for LOCAL_CASTER_SEND_STALL_MS */
    LOCAL_CASTER_END_LOST,         /**< the connection failed (reset, keepalive, socket error) */
    LOCAL_CASTER_END_SETTINGS,     /**< turned off, or the mountpoint, username or password changed */
    LOCAL_CASTER_END_NETWORK,      /**< the station's IPv4 address went away or changed */
    LOCAL_CASTER_END_RESTART,      /**< a planned restart (the shutdown hook) */
} local_caster_end_t;

/** The settings (persisted as settings "lcast"/"cfg"). */
typedef struct {
    bool enabled;
    char mountpoint[LOCAL_CASTER_MOUNT_LEN]; /**< 1-47 of [A-Za-z0-9._-]; a rover's request matches it ignoring case */
    char username[LOCAL_CASTER_USER_LEN];    /**< 1-31 of [A-Za-z0-9._-] */
    char password[LOCAL_CASTER_PASS_LEN];    /**< input only (set_config): "" keeps the stored one, else 8-63 bytes of
                                                  0x21-0x7E; local_caster_get_config() always returns "" */
    bool password_set;                       /**< output only: a password is stored */
} local_caster_cfg_t;

/** One streaming rover. */
typedef struct {
    char addr[LOCAL_CASTER_ADDR_LEN];      /**< "192.168.2.50"; IPv6 in RFC 5952 form; an IPv4-mapped address as IPv4 */
    uint8_t ntrip_version;                 /**< 2 when its request said Ntrip-Version: Ntrip/2.0, else 1 */
    bool chunked;                          /**< its data goes in HTTP chunks (NTRIP 2.0 over HTTP/1.1) */
    uint32_t connected_s;                  /**< since its success reply */
    uint64_t bytes_sent;                   /**< RTCM bytes its socket took (no reply header, no chunk framing) */
    uint64_t epochs_dropped;               /**< whole epochs the hub dropped because this rover read too slowly */
    char agent[LOCAL_CASTER_AGENT_LEN];    /**< its User-Agent (printable ASCII, cut), "" = none */
} local_caster_client_t;

/** Live status. Counters run since boot. */
typedef struct {
    local_caster_state_t state;
    uint16_t port;                    /**< CONFIG_LOCAL_CASTER_PORT */
    uint8_t max_clients;              /**< LOCAL_CASTER_MAX_CLIENTS */
    uint8_t client_count;             /**< rovers streaming now: clients[0 .. client_count), oldest first */
    local_caster_client_t clients[LOCAL_CASTER_MAX_CLIENTS];
    uint32_t sessions;                /**< sessions started */
    uint64_t bytes_sent;              /**< RTCM bytes sent, every session */
    uint32_t refused_not_local;       /**< closed at once: not a local address, the gateway, over the setup hotspot or
                                           from its subnet */
    uint32_t refused_login;           /**< 401: wrong or missing username or password */
    uint32_t refused_throttled;       /**< 429: too many wrong logins (from that address, or in total) */
    uint32_t refused_full;            /**< 503 (LOCAL_CASTER_MAX_CLIENTS rovers streaming, or no free hub consumer),
                                           or closed at once: every connection a rover or an answer being written */
    uint32_t refused_bad_request;     /**< 400 or 403: not an NTRIP GET, too long, not complete in time, from a web
                                           browser; or closed without a reply: nothing sent within
                                           LOCAL_CASTER_FIRST_BYTE_MS */
    uint32_t stalled;                 /**< sessions closed because the rover took no data for LOCAL_CASTER_SEND_STALL_MS */
    int listen_errno;                 /**< LOCAL_CASTER_ERROR: errno of the failed socket, bind or listen
                                           (ENOMEM: no task or no memory); 0 otherwise */
} local_caster_status_t;

/**
 * Load the stored config and register the shutdown hook. No task and no
 * socket yet. Nothing stored, or a record that does not decode: off, the
 * default mountpoint, LOCAL_CASTER_DEFAULT_USER, no password. Call after
 * rtcm_hub_init() and ntrip_server_init() (hook order) and before
 * local_caster_start().
 */
esp_err_t local_caster_init(void);

/** Create the task if the config is on (otherwise the first set_config that turns it on does). */
esp_err_t local_caster_start(void);

/**
 * The config. password is always "" (password_set says whether one is
 * stored). With nothing stored, the mountpoint is the station's hostname
 * (settings sys/hostname, else settings_default_hostname), read now.
 * @return ESP_ERR_INVALID_STATE before local_caster_init() (safe mode),
 *         ESP_ERR_INVALID_ARG for NULL.
 */
esp_err_t local_caster_get_config(local_caster_cfg_t *out);

/**
 * The stored password, for the web page's Show button (the only reader).
 * "" when none is stored. The caller wipes its copy.
 * @return ESP_OK; ESP_ERR_INVALID_ARG for NULL or cap 0;
 *         ESP_ERR_INVALID_STATE before local_caster_init() (safe mode).
 */
esp_err_t local_caster_get_password(char *out, size_t cap);

/**
 * Check, store (one blob, replaced as a unit) and apply. An empty password
 * keeps the stored one. Sessions end (LOCAL_CASTER_END_SETTINGS) when the
 * caster is turned off or the mountpoint, username or password changes.
 * Turning it on creates the task the first time. The caller's own copy of
 * the password is the caller's to wipe; this function wipes its own copies.
 * @param field, rule  optional: with ESP_ERR_INVALID_ARG, the failing field
 *        ("mountpoint", "username", "password") and its rule in lowercase
 *        words (static strings, as local_caster_check_config() gives them).
 * @return ESP_OK; ESP_ERR_INVALID_ARG (a field; nothing stored);
 *         ESP_ERR_INVALID_STATE before local_caster_init() (safe mode);
 *         ESP_ERR_NO_MEM when the task could not be created (the config is
 *         stored, and the state is LOCAL_CASTER_ERROR with listen_errno
 *         ENOMEM); a settings error when the write failed (nothing changed).
 */
esp_err_t local_caster_set_config(const local_caster_cfg_t *cfg, const char **field, const char **rule);

/**
 * Live status, from any task at any time (before init and in safe mode:
 * LOCAL_CASTER_OFF and zeros, with port and max_clients filled in).
 * connected_s is computed at the call.
 */
void local_caster_get_status(local_caster_status_t *out);

/**
 * The rules local_caster_set_config() applies. Pure; needs no init.
 * password_stored: a password is already stored (an empty cfg->password
 * keeps it). Rules and their words:
 *   mountpoint  "1 to 47 letters, digits, dots, hyphens or underscores"
 *   username    "1 to 31 letters, digits, dots, hyphens or underscores"
 *   password    "8 to 63 characters, letters, digits and symbols, no spaces"
 *   password    "needed to turn this on" (enabled, with none given or stored)
 * @return ESP_OK, or ESP_ERR_INVALID_ARG with *field and *rule set (each
 *         optional).
 */
esp_err_t local_caster_check_config(const local_caster_cfg_t *cfg, bool password_stored, const char **field,
                                    const char **rule);

#ifdef __cplusplus
}
#endif
