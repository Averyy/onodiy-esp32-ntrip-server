/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Moving between the access points of one network (a mesh, or a router with
 * repeaters under one name): pure logic, no IDF or RTOS includes,
 * host-testable. network.c runs it from the net task and carries out what it
 * decides. Times are seconds of an injected monotonic clock, compared only as
 * (uint32_t)(now - then).
 *
 * The station joins the strongest access point of its network when it
 * connects. A move later starts in one of two ways:
 *
 *  - The access point asks (802.11v BSS transition management request). The
 *    Wi-Fi supplicant answers and moves by itself; the station only decides
 *    whether a request reaches it (net_roam_btm_pass). A request is dropped
 *    while something must not lose the link (a firmware update, a receiver
 *    flash, the station's own access point up, changed credentials on
 *    trial) and within NET_ROAM_BTM_GAP_S after the previous move, unless
 *    the access point says it will disconnect the station anyway
 *    (disassociation imminent, its BSS terminating, the whole network
 *    disassociating): staying then only trades a move for a drop.
 *
 *  - The station's own check, only while the signal is weak: the smoothed
 *    RSSI (net_roam_rssi, an average over about 20 s) at or below
 *    cfg.weak_dbm for NET_ROAM_WEAK_HOLD_S. It then scans for its network
 *    (scanning while connected stalls traffic for a second or two, so it is
 *    rare): first NET_ROAM_SETTLE_S after the association at the earliest,
 *    then every NET_ROAM_SCAN_MESH_S once another access point of the
 *    network was seen during this association, else every
 *    NET_ROAM_SCAN_SOLO_S (a plain network with one access point and a weak
 *    signal: four scans a day). When two scans in a row, NET_ROAM_CONFIRM_S
 *    apart, each find an access point at least cfg.gain_db stronger than
 *    the current one (its scan reading or the average, whichever is
 *    higher), the station reconnects: the connect scan joins the strongest
 *    access point, so it need not be the same one both times. Not within
 *    cfg.min_interval_s of the previous move (either kind, so an access
 *    point's choice stands for that long), and never while held. Anything
 *    that breaks the run (the signal recovers, a hold, the association
 *    ends) starts the count again.
 *
 * A strong signal never scans, and a network with one access point gets at
 * most one scan per NET_ROAM_SCAN_SOLO_S while its signal stays weak.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PROVISIONAL, to be confirmed on hardware. */
#define NET_ROAM_WEAK_HOLD_S 60u     /**< the average at or below weak_dbm this long before a scan */
#define NET_ROAM_SETTLE_S 120u       /**< no scan this soon after an association */
#define NET_ROAM_CONFIRM_S 30u       /**< the second scan after a candidate was found */
#define NET_ROAM_SCAN_MESH_S 600u    /**< between scans while weak, another access point of the network seen */
#define NET_ROAM_SCAN_SOLO_S 21600u  /**< between scans while weak, none seen in this association */
#define NET_ROAM_RETRY_S 60u         /**< a scan that could not run is tried again after this */
#define NET_ROAM_HITS 2u             /**< scans in a row that must find a clearly stronger access point */
#define NET_ROAM_BTM_GAP_S 300u      /**< an access point's plain request is dropped this soon after a move */

/* BSS transition management request mode bits (IEEE 802.11, 9.6.13.9). */
#define NET_BTM_DISASSOC_IMMINENT 0x04u
#define NET_BTM_BSS_TERMINATION 0x08u
#define NET_BTM_ESS_DISASSOC_IMMINENT 0x10u

/** Who started a move (journal records, logs). */
typedef enum {
    NET_ROAM_BY_AP = 1,          /**< the access point asked (BSS transition request) */
    NET_ROAM_BY_STATION = 2,     /**< the station found a clearly stronger access point */
} net_roam_by_t;

typedef struct {
    int8_t weak_dbm;             /**< the average at or below this is a weak signal */
    uint8_t gain_db;             /**< a candidate at least this much stronger than the current access point */
    uint32_t min_interval_s;     /**< no own move this soon after the previous move */
} net_roam_cfg_t;

/** One access point of the station's network found by a scan (same SSID, a security the station accepts). */
typedef struct {
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
} net_roam_bss_t;

typedef struct {
    net_roam_cfg_t cfg;
    bool assoc;                  /**< associated (an access point known) */
    uint32_t assoc_s;
    uint8_t bssid[6];            /**< the access point of this association */
    bool avg_ok;                 /**< avg16 holds a reading of this association */
    int32_t avg16;               /**< smoothed RSSI, 1/16 dBm */
    bool weak;
    uint32_t weak_since_s;
    bool scanning;               /**< a scan was asked for, its result not in yet */
    uint32_t scan_ref_s;         /**< the next scan is due scan_wait_s after this */
    uint32_t scan_wait_s;
    bool other_seen;             /**< another access point of the network seen in this association */
    uint8_t hits;                /**< scans in a row that found a clearly stronger access point */
    net_roam_bss_t cand;         /**< the strongest one the last of them found (valid while hits > 0) */
    bool moved;                  /**< a move started since boot */
    uint32_t moved_s;            /**< when the last one started */
} net_roam_t;

typedef enum {
    NET_ROAM_IDLE = 0,
    NET_ROAM_SCAN,               /**< scan for the network now, then net_roam_scan_done (or net_roam_scan_failed) */
    NET_ROAM_GO,                 /**< reconnect now (net_roam_t.cand is the access point found); counted as a move */
} net_roam_act_t;

void net_roam_init(net_roam_t *r, const net_roam_cfg_t *cfg);

/** The station associated with access point `bssid` (a connect, or the end of a move). */
void net_roam_associated(net_roam_t *r, const uint8_t bssid[6], uint32_t now_s);

/** The association ended (any reason). */
void net_roam_disassociated(net_roam_t *r);

/** A signal reading of the association (every few seconds); readings outside -127..-1 dBm are ignored. */
void net_roam_rssi(net_roam_t *r, int8_t rssi);

/** The smoothed signal in dBm (rounded toward minus infinity), or 0 without a reading. */
int8_t net_roam_avg_dbm(const net_roam_t *r);

/** A move the access point asked for has started (counts for both intervals). */
void net_roam_moved(net_roam_t *r, uint32_t now_s);

/**
 * Once a second. `held`: nothing may move the station now (see the file
 * comment), or it is not online on an established link.
 */
net_roam_act_t net_roam_tick(net_roam_t *r, bool held, uint32_t now_s);

/**
 * The result of the scan NET_ROAM_SCAN asked for: the network's access
 * points found (`n` entries, any order; the current one may be among them).
 */
void net_roam_scan_done(net_roam_t *r, const net_roam_bss_t *list, size_t n, uint32_t now_s);

/** The scan could not start or was abandoned: tried again NET_ROAM_RETRY_S later. */
void net_roam_scan_failed(net_roam_t *r, uint32_t now_s);

/** Whether an access point's plain transition request may reach the supplicant now. */
bool net_roam_btm_open(const net_roam_t *r, bool held, uint32_t now_s);

/**
 * Whether a transition request with `request_mode` goes to the supplicant:
 * always while open, else only when the access point disconnects the
 * station anyway (NET_BTM_DISASSOC_IMMINENT, NET_BTM_BSS_TERMINATION,
 * NET_BTM_ESS_DISASSOC_IMMINENT).
 */
bool net_roam_btm_pass(bool open, uint8_t request_mode);

/**
 * DIAG_EV_WIFI_ROAM fields. a: bits 0-7 the channel before, 8-15 after,
 * 16-23 who started it (net_roam_by_t), bit 24 the station stayed on the
 * same access point (an own move whose connect found no stronger one after
 * all). b: bits 0-7 the RSSI before, 8-15 after (as int8_t; 0 = unknown),
 * 16-30 seconds from the start of the move to the address on the new link
 * (capped at 32767).
 */
int32_t net_roam_rec_a(uint8_t ch_from, uint8_t ch_to, net_roam_by_t by, bool same);
int32_t net_roam_rec_b(int8_t rssi_from, int8_t rssi_to, uint32_t secs);

#ifdef __cplusplus
}
#endif
