/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Network recovery ladder (pure logic, no IDF or RTOS includes,
 * host-testable). network.c runs it once per second from
 * network_ladder_tick() and carries out what it decides.
 *
 * It runs while the station has credentials (an unconfigured station in
 * setup is never "recovered") and the station's network is the suspect, by
 * any of three triggers:
 *   P  the protected caster is enabled and not streaming (nor paused)
 *      although the GNSS path delivers RTCM;
 *   A  the station has been associated for NET_LADDER_ASSOC_NO_IP_S without
 *      an IP address (DHCP never answers, or the lease went and nothing
 *      came back while the link stayed up);
 *   S  an enabled caster slot (any caster) has failed for
 *      NET_LADDER_UNREACHED_S in a row with failures that never reached the
 *      caster: name lookups, and TCP connects that timed out or failed
 *      locally (a refused connect reached the caster's host and ends the
 *      run). Slots wait for fresh RTCM before connecting, so S implies a
 *      healthy receiver.
 * A station with no enabled slot is covered by A alone. Every pass start is
 * reported (NET_LADDER_PASS) with the triggers that hold then.
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
 * Only trigger P can reboot: at the reboot rung P must hold (at that rung,
 * not only at the pass start); a pass started or kept alive by A or S alone
 * withholds the reboot (NO_REBOOT_TRIGGER) and enters the hold, and the next
 * pass repeats the reconnect, renew and restart rungs.
 * The reboot is also withheld while a receiver-flash journal is open (any
 * state, safe mode included; not when its state is only unknown because the
 * settings are unavailable), within the minimum interval since the last
 * network-ladder reboot (counted across resets), while a reboot inhibit is
 * held, and while a client is connected to the station's own AP (the
 * Wi-Fi restart is deferred then too: someone is using the setup page).
 * The AP-client withhold has a limit: per episode (from the ladder's start
 * until it resets) it holds the Wi-Fi restart and the reboot back for at
 * most NET_LADDER_AP_HOLD_MAX_S from the first time it did; after that they
 * run even with a client connected (the most likely client is the owner's
 * phone, which stays on the remembered setup network without internet),
 * and the record carries AP_OVERRIDDEN.
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
#define NET_LADDER_ASSOC_NO_IP_S 60u     /**< trigger A: associated this long without an IP */
#define NET_LADDER_UNREACHED_S  180u     /**< trigger S: a slot's failures did not reach its caster for this long */
#define NET_LADDER_AP_HOLD_MAX_S 1800u   /**< AP clients hold the restart and reboot back at most this long per episode */

/** Triggers (bit flags; also the reason byte of the pass-start record). */
#define NET_TRIG_P (1u << 0)   /**< protected caster down while RTCM is healthy */
#define NET_TRIG_A (1u << 1)   /**< associated without an IP */
#define NET_TRIG_S (1u << 2)   /**< a slot's failures do not reach its caster */

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
    NET_LADDER_PASS,          /**< a pass starts (`triggers` = what holds now) */
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
    NET_WHY_NO_REBOOT_TRIGGER = 7, /**< reboot rung: the pass runs for triggers A or S only, which never reboot */
    NET_WHY_AP_OVERRIDDEN = 8,/**< the rung runs although a client is connected: the AP withhold's limit is over */
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
    bool ap_held;             /**< AP clients withheld a rung in this episode */
    uint32_t ap_held_since_s; /**< ... the first time */
} net_ladder_t;

typedef struct {
    bool caster_down;         /**< protected caster configured and not streaming */
    bool rtcm_healthy;        /**< the GNSS path delivers RTCM */
    uint32_t assoc_no_ip_s;   /**< associated without an IP for this long (0 = not associated, or has an IP) */
    uint32_t unreached_s;     /**< longest run of an enabled slot's failures that did not reach its caster */
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
    net_ladder_why_t why;     /**< SKIP/WITHHELD: why; ACT: NONE, or AP_OVERRIDDEN */
    uint8_t triggers;         /**< PASS: NET_TRIG_* holding at the pass start */
} net_ladder_out_t;

/** The triggers that hold for these inputs (NET_TRIG_*; 0 without credentials). */
uint8_t net_ladder_triggers(const net_ladder_in_t *in);

void net_ladder_init(net_ladder_t *l);

/** One decision per call (call once per second). */
net_ladder_out_t net_ladder_tick(net_ladder_t *l, const net_ladder_in_t *in, uint32_t now_s);

#ifdef __cplusplus
}
#endif
