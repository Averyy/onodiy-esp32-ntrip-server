/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Network recovery ladder (pure logic, no IDF or RTOS includes,
 * host-testable). network.c runs it once per second from
 * network_ladder_tick() and carries out what it decides.
 *
 * It runs while the station's network is the suspect: the protected caster
 * is not streaming although the GNSS path delivers RTCM, and the station has
 * credentials (an unconfigured station in setup is never "recovered").
 * Rungs, counted from the start of a pass (the start of the episode, or the
 * end of the previous hold):
 *
 *   +60 s   reconnect (Wi-Fi disconnect + connect)
 *   +180 s  renew DHCP and switch to the fallback DNS servers
 *   +360 s  restart Wi-Fi
 *   +600 s  reboot the ESP32 (only an ESP32 reset: the receiver keeps running)
 *
 * Every rung first asks for a fresh upstream probe (one that finished after
 * the rung became due; NET_LADDER_PROBE, waited for up to
 * NET_LADDER_PROBE_WAIT_S, after which it counts as "nothing beyond the
 * router") and acts on it:
 *  - upstream reachable: the network works and the fault is remote (the
 *    caster): the rung is skipped (SKIP, UPSTREAM_OK). Caster trouble is
 *    handled per slot, never by touching Wi-Fi or rebooting;
 *  - no IP, or DNS failing while a TCP connect to a public resolver works:
 *    the fault is on the station's side; every rung runs;
 *  - an IP but nothing beyond the router: the non-reboot rungs run (a new
 *    lease or another mesh node may help), the reboot is withheld
 *    (NO_UPSTREAM): it cannot fix the router or the ISP.
 * The reboot is also withheld while a receiver-flash journal is open (any
 * state, safe mode included; not when its state is only unknown because the
 * settings are unavailable), within the minimum interval since the last
 * network-ladder reboot (counted across resets), while a reboot inhibit is
 * held, and while a client is connected to the station's own AP (the
 * Wi-Fi restart is deferred then too: someone is using the setup page).
 * After the reboot rung (done, skipped or withheld) the ladder holds for
 * NET_LADDER_HOLD_S and then starts a new pass at the first rung, so a
 * persistent fault is retried without a reboot loop.
 * The ladder resets after NET_LADDER_HEALTHY_S without the condition; a
 * shorter healthy spell keeps the pass clock running, so a flapping caster
 * connection still escalates.
 *
 * Timings are PROVISIONAL, to be confirmed on hardware.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NET_LADDER_RUNGS        4
#define NET_LADDER_HEALTHY_S    60u
#define NET_LADDER_HOLD_S       600u
#define NET_LADDER_PROBE_WAIT_S 60u

/** Rung offsets from the start of a pass (s). */
#define NET_LADDER_RECONNECT_S  60u
#define NET_LADDER_RENEW_S      180u
#define NET_LADDER_RESTART_S    360u
#define NET_LADDER_REBOOT_S     600u

/** Upstream probe result (same values as network_probe_t). */
typedef enum {
    NET_PROBE_NONE = 0,       /**< never probed */
    NET_PROBE_OK,             /**< name resolved and a TCP connect went through */
    NET_PROBE_NO_IP,          /**< the station has no IP */
    NET_PROBE_DNS_FAILED,     /**< IP; name lookup failed but a TCP connect to a public resolver worked */
    NET_PROBE_NO_UPSTREAM,    /**< IP; nothing beyond the router answered */
} net_probe_t;

/** Rungs (also the journal step codes). */
typedef enum {
    NET_RUNG_NONE = 0,
    NET_RUNG_RECONNECT = 1,
    NET_RUNG_RENEW_DHCP = 2,
    NET_RUNG_RESTART_WIFI = 3,
    NET_RUNG_REBOOT = 4,
} net_rung_t;

typedef enum {
    NET_LADDER_NONE = 0,      /**< nothing to do this tick */
    NET_LADDER_PROBE,         /**< start an upstream probe (the rung waits for its result) */
    NET_LADDER_ACT,           /**< run `rung` now */
    NET_LADDER_SKIP,          /**< `rung` was due and is skipped (`why`) */
    NET_LADDER_WITHHELD,      /**< `rung` (reboot or Wi-Fi restart) is withheld (`why`) */
    NET_LADDER_RESET,         /**< healthy again: ladder back to idle (`rung` = last action started) */
} net_ladder_act_t;

/** Why a rung was skipped or withheld (also the journal reason codes). */
typedef enum {
    NET_WHY_NONE = 0,
    NET_WHY_UPSTREAM_OK = 1,  /**< the probe reached the internet: the fault is remote */
    NET_WHY_NO_UPSTREAM = 2,  /**< an IP but nothing beyond the router: a reboot cannot help */
    NET_WHY_JOURNAL_OPEN = 3, /**< a receiver-flash journal is open */
    NET_WHY_INTERVAL = 4,     /**< the last network-ladder reboot is too recent */
    NET_WHY_INHIBITED = 5,    /**< a reboot inhibit is held */
    NET_WHY_AP_IN_USE = 6,    /**< a client is connected to the station's AP */
} net_ladder_why_t;

typedef enum {
    NET_LADDER_IDLE = 0,
    NET_LADDER_ACTIVE,
    NET_LADDER_HOLD,
} net_ladder_phase_t;

typedef struct {
    net_ladder_phase_t phase;
    uint8_t next;             /**< index of the next rung (0 = reconnect ... 3 = reboot) */
    uint8_t last_rung;        /**< last rung acted on (net_rung_t) */
    uint32_t pass_start_s;
    uint32_t hold_start_s;
    bool healthy;
    uint32_t healthy_since_s;
    bool probe_asked;
    uint32_t probe_asked_s;
} net_ladder_t;

typedef struct {
    bool caster_down;         /**< protected caster configured and not streaming */
    bool rtcm_healthy;        /**< the GNSS path delivers RTCM */
    bool sta_configured;      /**< station credentials present */
    bool probe_valid;         /**< probe/probe_at_s hold a finished probe */
    net_probe_t probe;
    uint32_t probe_at_s;      /**< when that probe finished */
    bool journal_open;        /**< receiver-flash journal open (any state) */
    bool journal_unknown;     /**< journal state unknown (settings unavailable): does not withhold */
    bool reboot_inhibited;
    uint32_t ap_clients;
    uint32_t since_reboot_s;  /**< since the last network-ladder reboot, UINT32_MAX = none known */
    uint32_t min_reboot_interval_s;
} net_ladder_in_t;

typedef struct {
    net_ladder_act_t act;
    net_rung_t rung;
    net_ladder_why_t why;
} net_ladder_out_t;

void net_ladder_init(net_ladder_t *l);

/** One decision per call (call once per second). */
net_ladder_out_t net_ladder_tick(net_ladder_t *l, const net_ladder_in_t *in, uint32_t now_s);

#ifdef __cplusplus
}
#endif
