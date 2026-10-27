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

static const uint32_t scan_wait_s[NET_ROAM_SCAN_STEPS] = {
    NET_ROAM_SCAN_MESH_S, 1800u, 3600u, 10800u, NET_ROAM_SCAN_SOLO_S, 43200u, NET_ROAM_SCAN_MAX_S,
};
#define SOLO_STEP 4u

void net_roam_init(net_roam_t *r, const net_roam_cfg_t *cfg)
{
    memset(r, 0, sizeof(*r));
    r->cfg = *cfg;
}

void net_roam_associated(net_roam_t *r, const uint8_t bssid[6], uint32_t now_s)
{
    r->assoc = true;
    r->assoc_s = now_s;
    r->generation++;
    memcpy(r->bssid, bssid, sizeof(r->bssid));
    r->avg_ok = false;
    r->avg16 = 0;
    r->weak = false;
    r->scanning = false;
    r->scan_ref_s = now_s;
    r->scan_wait_s = 0;   /* NET_ROAM_SETTLE_S and the weak run hold the first scan back */
    r->negative_step = 0;
    r->ref_ok = false;
    r->change = 0;
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

/* Keep short threshold crossings separate from a real change in conditions.
 * Nothing here overrides a hold, the settle time, or the post-move interval. */
static void signal_change(net_roam_t *r, bool weak, uint32_t now_s)
{
    if (!r->avg_ok) {
        return;
    }
    if (!r->ref_ok) {
        r->ref16 = r->avg16;
        r->ref_ok = true;
    }
    int32_t delta = r->avg16 - r->ref16;
    int8_t change = delta >= NET_ROAM_CHANGE_DB * 16 ? 1
                    : weak && delta <= -NET_ROAM_CHANGE_DB * 16 ? -1 : 0;
    if (change != r->change) {
        r->change = change;
        r->change_since_s = now_s;
    }
    if (change == 0 || r->scanning || (uint32_t)(now_s - r->change_since_s) < NET_ROAM_CHANGE_HOLD_S) {
        return;
    }
    r->ref16 = r->avg16;
    r->change = 0;
    if (change < 0) {
        r->negative_step = 0;
        /* Shorten the existing deadline; do not keep moving it forward on
         * every weak tick. A confirmation or failure retry keeps its wait. */
        if (r->scan_wait_s > NET_ROAM_SCAN_MESH_S) {
            r->scan_wait_s = NET_ROAM_SCAN_MESH_S;
        }
    }
}

net_roam_act_t net_roam_tick(net_roam_t *r, bool held, uint32_t now_s)
{
    if (!r->assoc) {
        return NET_ROAM_IDLE;
    }
    bool weak = r->avg_ok && r->avg16 <= (int32_t)r->cfg.weak_dbm * 16;
    signal_change(r, weak, now_s);
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
    if (r->avg_ok) {
        r->ref_ok = true;
        r->ref16 = r->avg16;
        r->change = 0;
    }
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
    unsigned step = r->negative_step;
    if (!r->other_seen && step < SOLO_STEP) {
        step = SOLO_STEP;
    }
    r->scan_wait_s = scan_wait_s[step];
    r->negative_step = step + 1u < NET_ROAM_SCAN_STEPS ? (uint8_t)(step + 1u) : (uint8_t)step;
}

void net_roam_scan_failed(net_roam_t *r, uint32_t now_s)
{
    r->scanning = false;
    r->hits = 0;   /* a failed confirmation cannot preserve an arbitrarily old first hit */
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

int32_t net_roam_rec_a(uint8_t ch_from, uint8_t ch_to, net_roam_by_t by, bool same, bool kept)
{
    uint32_t a = (uint32_t)ch_from | ((uint32_t)ch_to << 8) | (((uint32_t)by & 0xFFu) << 16) | (same ? 1u << 24 : 0u) |
                 (kept ? 1u << 25 : 0u);
    return (int32_t)a;
}

int32_t net_roam_rec_b(int8_t rssi_from, int8_t rssi_to, uint32_t secs)
{
    uint8_t from = net_rssi_known(rssi_from) ? (uint8_t)rssi_from : 0u;
    uint8_t to = net_rssi_known(rssi_to) ? (uint8_t)rssi_to : 0u;
    uint32_t s = secs > 32767u ? 32767u : secs;
    return (int32_t)((uint32_t)from | ((uint32_t)to << 8) | (s << 16));
}

/* ---- keeping the address across a move ------------------------------------------------ */

void net_hold_init(net_hold_t *h)
{
    memset(h, 0, sizeof(*h));
}

bool net_hold_lease_ok(bool bound, uint32_t left_s)
{
    return bound && left_s >= NET_HOLD_LEASE_MIN_S;
}

void net_hold_gate(net_hold_t *h, bool open, uint32_t ip)
{
    h->open = open && ip != 0;
    h->ip = h->open ? ip : 0;
    if (!h->open) {
        h->armed = false;
    }
}

bool net_hold_arm(net_hold_t *h, uint32_t now_ms)
{
    h->armed = h->open && h->state == NET_HOLD_IDLE;
    h->armed_ms = now_ms;
    return h->armed;
}

bool net_hold_disconnect(net_hold_t *h, uint16_t reason, uint32_t now_ms)
{
    bool armed = h->armed && (uint32_t)(now_ms - h->armed_ms) <= NET_HOLD_ARM_MS;
    bool keep = h->state == NET_HOLD_IDLE && h->open && (armed || reason == NET_REASON_ROAMING);
    uint32_t ip = h->ip;
    /* Whatever happens to this one, the next is kept only once the net task opens the gate (or arms) again. */
    h->open = false;
    h->ip = 0;
    h->armed = false;
    h->verifying = false;
    if (keep) {
        h->state = NET_HOLD_AWAY;
        h->state_ms = now_ms;
        h->kept_ip = ip;
    } else {
        h->state = NET_HOLD_IDLE;   /* a hold running ends here: the move failed */
    }
    return keep;
}

bool net_hold_connected(net_hold_t *h, uint32_t now_ms)
{
    if (h->state == NET_HOLD_AWAY) {
        h->away_ms = now_ms - h->state_ms;
        h->state = NET_HOLD_BACK;
        h->state_ms = now_ms;
        h->verifying = false;
        return true;
    }
    return h->state == NET_HOLD_BACK;   /* still held: the interface is up with its address */
}

void net_hold_verify(net_hold_t *h, uint32_t now_ms)
{
    if (h->state == NET_HOLD_BACK) {
        h->verifying = true;
        h->verify_ms = now_ms;
    }
}

net_hold_out_t net_hold_check(net_hold_t *h, bool dhcp_bound, uint32_t ip_now, uint32_t now_ms)
{
    net_hold_out_t out = NET_HOLD_WAIT;
    if (h->state == NET_HOLD_AWAY) {
        if ((uint32_t)(now_ms - h->state_ms) >= NET_HOLD_AWAY_S * 1000u) {
            out = NET_HOLD_EXPIRED;
        }
    } else if (h->state == NET_HOLD_BACK) {
        /* Until DHCP was asked, its state is the one from before the move: only the address counts. */
        uint32_t from = h->verifying ? h->verify_ms : h->state_ms;
        if (ip_now != h->kept_ip) {
            out = NET_HOLD_LOST;
        } else if (h->verifying && dhcp_bound) {
            out = NET_HOLD_CONFIRMED;
        } else if ((uint32_t)(now_ms - from) >= NET_HOLD_VERIFY_S * 1000u) {
            out = NET_HOLD_UNCONFIRMED;
        }
    }
    if (out != NET_HOLD_WAIT) {
        h->state = NET_HOLD_IDLE;
        h->verifying = false;
    }
    return out;
}

net_hold_state_t net_hold_end(net_hold_t *h)
{
    net_hold_state_t was = (net_hold_state_t)h->state;
    h->state = NET_HOLD_IDLE;
    h->verifying = false;
    h->armed = false;
    h->open = false;
    h->ip = 0;
    return was;
}
