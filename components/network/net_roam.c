/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "net_roam.h"

#include <string.h>

#include "net_core.h"

/* Weight of a new reading in the average: 1/4, so with a reading every 5 s
 * the average follows a change within about 20 s. */
#define AVG_SHIFT 2

void net_roam_init(net_roam_t *r, const net_roam_cfg_t *cfg)
{
    memset(r, 0, sizeof(*r));
    r->cfg = *cfg;
}

void net_roam_associated(net_roam_t *r, const uint8_t bssid[6], uint32_t now_s)
{
    r->assoc = true;
    r->assoc_s = now_s;
    memcpy(r->bssid, bssid, sizeof(r->bssid));
    r->avg_ok = false;
    r->avg16 = 0;
    r->weak = false;
    r->scanning = false;
    r->scan_ref_s = now_s;
    r->scan_wait_s = 0;   /* NET_ROAM_SETTLE_S and the weak run hold the first scan back */
    r->other_seen = false;
    r->hits = 0;
}

void net_roam_disassociated(net_roam_t *r)
{
    r->assoc = false;
    r->avg_ok = false;
    r->weak = false;
    r->scanning = false;
    r->hits = 0;
}

void net_roam_rssi(net_roam_t *r, int8_t rssi)
{
    if (!r->assoc || !net_rssi_known(rssi)) {
        return;
    }
    int32_t v = (int32_t)rssi * 16;
    if (!r->avg_ok) {
        r->avg16 = v;
        r->avg_ok = true;
    } else {
        r->avg16 += (v - r->avg16) / (1 << AVG_SHIFT);
    }
}

int8_t net_roam_avg_dbm(const net_roam_t *r)
{
    if (!r->avg_ok) {
        return 0;
    }
    int32_t a = r->avg16;
    return (int8_t)(a >= 0 ? a / 16 : -((-a + 15) / 16));
}

void net_roam_moved(net_roam_t *r, uint32_t now_s)
{
    r->moved = true;
    r->moved_s = now_s;
    r->hits = 0;
}

net_roam_act_t net_roam_tick(net_roam_t *r, bool held, uint32_t now_s)
{
    if (!r->assoc) {
        return NET_ROAM_IDLE;
    }
    bool weak = r->avg_ok && r->avg16 <= (int32_t)r->cfg.weak_dbm * 16;
    if (weak && !r->weak) {
        r->weak_since_s = now_s;
    }
    r->weak = weak;
    bool may = !held && weak && (uint32_t)(now_s - r->weak_since_s) >= NET_ROAM_WEAK_HOLD_S &&
               (uint32_t)(now_s - r->assoc_s) >= NET_ROAM_SETTLE_S &&
               (!r->moved || (uint32_t)(now_s - r->moved_s) >= r->cfg.min_interval_s);
    if (!may) {
        r->hits = 0;   /* the run is broken: a candidate has to be found twice again */
        return NET_ROAM_IDLE;
    }
    if (r->scanning) {
        return NET_ROAM_IDLE;
    }
    if (r->hits >= NET_ROAM_HITS) {
        r->hits = 0;
        r->moved = true;
        r->moved_s = now_s;
        return NET_ROAM_GO;
    }
    if ((uint32_t)(now_s - r->scan_ref_s) >= r->scan_wait_s) {
        r->scanning = true;
        return NET_ROAM_SCAN;
    }
    return NET_ROAM_IDLE;
}

void net_roam_scan_done(net_roam_t *r, const net_roam_bss_t *list, size_t n, uint32_t now_s)
{
    r->scanning = false;
    if (!r->assoc) {
        return;
    }
    r->scan_ref_s = now_s;
    bool base_ok = r->avg_ok;
    int32_t base16 = r->avg16;
    const net_roam_bss_t *best = NULL;
    for (size_t i = 0; list != NULL && i < n; i++) {
        const net_roam_bss_t *e = &list[i];
        if (!net_rssi_known(e->rssi)) {
            continue;
        }
        if (memcmp(e->bssid, r->bssid, sizeof(r->bssid)) == 0) {
            /* The current access point's own reading in this scan: the higher of the two is the baseline. */
            if (!base_ok || (int32_t)e->rssi * 16 > base16) {
                base16 = (int32_t)e->rssi * 16;
                base_ok = true;
            }
            continue;
        }
        r->other_seen = true;
        if (best == NULL || e->rssi > best->rssi) {
            best = e;
        }
    }
    if (best != NULL && base_ok && (int32_t)best->rssi * 16 >= base16 + (int32_t)r->cfg.gain_db * 16) {
        /* Found in this scan: the reconnect joins the strongest access point,
         * so it need not be the same one as in the scan before. */
        if (r->hits < UINT8_MAX) {
            r->hits++;
        }
        r->cand = *best;
        r->scan_wait_s = r->hits >= NET_ROAM_HITS ? 0 : NET_ROAM_CONFIRM_S;
        return;
    }
    r->hits = 0;
    r->scan_wait_s = r->other_seen ? NET_ROAM_SCAN_MESH_S : NET_ROAM_SCAN_SOLO_S;
}

void net_roam_scan_failed(net_roam_t *r, uint32_t now_s)
{
    r->scanning = false;
    r->scan_ref_s = now_s;
    r->scan_wait_s = NET_ROAM_RETRY_S;
}

bool net_roam_btm_open(const net_roam_t *r, bool held, uint32_t now_s)
{
    return !held && (!r->moved || (uint32_t)(now_s - r->moved_s) >= NET_ROAM_BTM_GAP_S);
}

bool net_roam_btm_pass(bool open, uint8_t request_mode)
{
    return open ||
           (request_mode & (NET_BTM_DISASSOC_IMMINENT | NET_BTM_BSS_TERMINATION | NET_BTM_ESS_DISASSOC_IMMINENT)) != 0;
}

int32_t net_roam_rec_a(uint8_t ch_from, uint8_t ch_to, net_roam_by_t by, bool same)
{
    uint32_t a = (uint32_t)ch_from | ((uint32_t)ch_to << 8) | (((uint32_t)by & 0xFFu) << 16) | (same ? 1u << 24 : 0u);
    return (int32_t)a;
}

int32_t net_roam_rec_b(int8_t rssi_from, int8_t rssi_to, uint32_t secs)
{
    uint8_t from = net_rssi_known(rssi_from) ? (uint8_t)rssi_from : 0u;
    uint8_t to = net_rssi_known(rssi_to) ? (uint8_t)rssi_to : 0u;
    uint32_t s = secs > 32767u ? 32767u : secs;
    return (int32_t)((uint32_t)from | ((uint32_t)to << 8) | (s << 16));
}
