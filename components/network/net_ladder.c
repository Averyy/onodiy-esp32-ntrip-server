/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "net_ladder.h"

#include <string.h>

static const uint32_t s_offset_s[NET_LADDER_RUNGS] = {
    NET_LADDER_RECONNECT_S,
    NET_LADDER_RENEW_S,
    NET_LADDER_RESTART_S,
    NET_LADDER_REBOOT_S,
};

_Static_assert(NET_LADDER_RECONNECT_S < NET_LADDER_RENEW_S && NET_LADDER_RENEW_S < NET_LADDER_RESTART_S &&
                   NET_LADDER_RESTART_S < NET_LADDER_REBOOT_S,
               "rungs are ordered");

void net_ladder_init(net_ladder_t *l)
{
    memset(l, 0, sizeof(*l));
}

uint8_t net_ladder_triggers(const net_ladder_in_t *in)
{
    if (!in->sta_configured) {
        return 0;
    }
    uint8_t t = 0;
    if (in->caster_down && in->rtcm_healthy) {
        t |= NET_TRIG_P;
    }
    if (in->assoc_no_ip_s >= NET_LADDER_ASSOC_NO_IP_S) {
        t |= NET_TRIG_A;
    }
    if (in->unreached_s >= NET_LADDER_UNREACHED_S) {
        t |= NET_TRIG_S;
    }
    return t;
}

static void start_pass(net_ladder_t *l, uint32_t now_s)
{
    l->phase = NET_LADDER_ACTIVE;
    l->next = 0;
    l->pass_start_s = now_s;
    l->probe_asked = false;
}

static void enter_hold(net_ladder_t *l, uint32_t now_s)
{
    l->phase = NET_LADDER_HOLD;
    l->hold_start_s = now_s;
    l->probe_asked = false;
}

/*
 * AP clients at the Wi-Fi restart or reboot rung: withheld (AP_IN_USE) for
 * at most NET_LADDER_AP_HOLD_MAX_S per episode, counted from the first such
 * withhold; after that the clients no longer hold the rung back
 * (AP_OVERRIDDEN, the rung runs if nothing else withholds it).
 */
static net_ladder_why_t ap_check(net_ladder_t *l, const net_ladder_in_t *in, uint32_t now_s)
{
    if (in->ap_clients == 0) {
        return NET_WHY_NONE;
    }
    if (!l->ap_held) {
        l->ap_held = true;
        l->ap_held_since_s = now_s;
    }
    if ((uint32_t)(now_s - l->ap_held_since_s) < NET_LADDER_AP_HOLD_MAX_S) {
        return NET_WHY_AP_IN_USE;
    }
    return NET_WHY_AP_OVERRIDDEN;
}

/* Why the reboot rung may not run, NET_WHY_NONE if it may (AP_OVERRIDDEN:
 * it may, past the AP-client limit). */
static net_ladder_why_t reboot_blocked(net_ladder_t *l, const net_ladder_in_t *in, net_probe_t p, uint32_t now_s)
{
    if ((net_ladder_triggers(in) & NET_TRIG_P) == 0) {
        return NET_WHY_NO_REBOOT_TRIGGER;   /* A and S never reboot */
    }
    if (p != NET_PROBE_NO_IP && p != NET_PROBE_DNS_FAILED) {
        return NET_WHY_NO_UPSTREAM;   /* nothing beyond the router (or no answer): not ours to fix */
    }
    if (in->journal_open && !in->journal_unknown) {
        return NET_WHY_JOURNAL_OPEN;
    }
    if (in->since_reboot_s != UINT32_MAX && in->since_reboot_s < in->min_reboot_interval_s) {
        return NET_WHY_INTERVAL;
    }
    if (in->reboot_inhibited) {
        return NET_WHY_INHIBITED;
    }
    return ap_check(l, in, now_s);
}

net_ladder_out_t net_ladder_tick(net_ladder_t *l, const net_ladder_in_t *in, uint32_t now_s)
{
    net_ladder_out_t out = { NET_LADDER_NONE, NET_RUNG_NONE, NET_WHY_NONE, 0 };
    uint8_t triggers = net_ladder_triggers(in);
    bool needed = triggers != 0;

    if (l->phase == NET_LADDER_IDLE) {
        if (needed) {
            net_ladder_init(l);   /* a new episode: the AP-client limit starts afresh */
            start_pass(l, now_s);
            out.act = NET_LADDER_PASS;
            out.triggers = triggers;
        }
        return out;
    }

    if (!needed) {
        if (!l->healthy) {
            l->healthy = true;
            l->healthy_since_s = now_s;
        }
        if ((uint32_t)(now_s - l->healthy_since_s) >= NET_LADDER_HEALTHY_S) {
            out.act = NET_LADDER_RESET;
            out.rung = (net_rung_t)l->last_rung;
            net_ladder_init(l);
        }
        return out;   /* the pass clock keeps running meanwhile */
    }
    l->healthy = false;

    if (l->phase == NET_LADDER_HOLD) {
        if ((uint32_t)(now_s - l->hold_start_s) >= NET_LADDER_HOLD_S) {
            start_pass(l, now_s);
            out.act = NET_LADDER_PASS;
            out.triggers = triggers;
        }
        return out;
    }

    uint32_t elapsed = now_s - l->pass_start_s;
    if (elapsed < s_offset_s[l->next]) {
        return out;
    }
    uint32_t due_age = elapsed - s_offset_s[l->next];

    /* A probe counts only if it finished after this rung became due. */
    net_probe_t p;
    if (in->probe_valid && (uint32_t)(now_s - in->probe_at_s) <= due_age) {
        p = in->probe;
    } else if (!l->probe_asked) {
        l->probe_asked = true;
        l->probe_asked_s = now_s;
        out.act = NET_LADDER_PROBE;
        return out;
    } else if ((uint32_t)(now_s - l->probe_asked_s) < NET_LADDER_PROBE_WAIT_S) {
        return out;
    } else {
        p = NET_PROBE_NO_UPSTREAM;   /* no answer in time */
    }
    l->probe_asked = false;

    net_rung_t rung = (net_rung_t)(l->next + 1);
    out.rung = rung;
    if (rung == NET_RUNG_REBOOT) {
        enter_hold(l, now_s);
    } else {
        l->next++;
    }

    if (p == NET_PROBE_OK) {
        out.act = NET_LADDER_SKIP;
        out.why = NET_WHY_UPSTREAM_OK;
        return out;
    }
    net_ladder_why_t why = NET_WHY_NONE;
    if (rung == NET_RUNG_RESTART_WIFI) {
        why = ap_check(l, in, now_s);
    } else if (rung == NET_RUNG_REBOOT) {
        why = reboot_blocked(l, in, p, now_s);
    }
    if (why != NET_WHY_NONE && why != NET_WHY_AP_OVERRIDDEN) {
        out.act = NET_LADDER_WITHHELD;
        out.why = why;
        return out;
    }
    out.act = NET_LADDER_ACT;
    out.why = why;   /* AP_OVERRIDDEN: runs with a client connected */
    l->last_rung = (uint8_t)rung;
    return out;
}
