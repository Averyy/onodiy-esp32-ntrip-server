/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "net_ladder.h"
#include "unity.h"

/* A small world around the ladder: probes finish after `probe_delay_s` with
 * the result `world` (never when probe_delay_s is UINT32_MAX), and every
 * decision is counted. Times start near the clock wrap. */
typedef struct {
    net_ladder_t l;
    net_ladder_in_t in;
    uint32_t now;
    net_probe_t world;
    uint32_t probe_delay_s;
    bool probe_pending;
    uint32_t probe_start_s;
    bool rebooted;
    uint32_t last_reboot_s;
    uint32_t acts[5];
    uint32_t act_at[5];         /* time of the last action per rung */
    uint32_t skips;
    uint32_t withheld;
    net_ladder_why_t last_why;
    net_rung_t last_held_rung;
    uint32_t resets;
    net_rung_t reset_rung;
    uint32_t probes;
    bool reject_dispatch;
    bool delay_execution;
    bool queued;
    uint32_t queued_generation;
    uint32_t dispatches;
} sim_t;

#define T0 (UINT32_MAX - 300u)

static void sim_init(sim_t *s, net_probe_t world)
{
    memset(s, 0, sizeof(*s));
    net_ladder_init(&s->l);
    s->now = T0;
    s->world = world;
    s->probe_delay_s = 2;
    s->in.caster_down = true;
    s->in.rtcm_healthy = true;
    s->in.sta_configured = true;
    s->in.has_ip = true;
    s->in.min_reboot_interval_s = 6 * 3600;
    s->in.since_reboot_s = UINT32_MAX;
}

static bool sim_execute(sim_t *s)
{
    if (!s->queued) {
        return false;
    }
    s->queued = false;
    net_ladder_out_t o;
    if (!net_ladder_begin(&s->l, s->queued_generation, &s->in, s->now, s->now, &o)) {
        return false;
    }
    TEST_ASSERT_TRUE(o.rung >= NET_RUNG_RECONNECT && o.rung <= NET_RUNG_REBOOT);
    s->acts[o.rung]++;
    s->act_at[o.rung] = s->now;
    if (o.rung == NET_RUNG_REBOOT) {
        s->rebooted = true;   /* the world keeps running: the reboot did not fix it */
        s->last_reboot_s = s->now;
    }
    return true;
}

static net_ladder_out_t sim_step(sim_t *s)
{
    if (s->probe_pending && s->probe_delay_s != UINT32_MAX && s->now - s->probe_start_s >= s->probe_delay_s) {
        s->probe_pending = false;
        s->in.probe_valid = true;
        s->in.probe = s->world;
        s->in.probe_at_s = s->now;
    }
    s->in.since_reboot_s = s->rebooted ? s->now - s->last_reboot_s : UINT32_MAX;
    net_ladder_out_t o = net_ladder_tick(&s->l, &s->in, s->now);
    switch (o.act) {
    case NET_LADDER_PROBE:
        s->probes++;
        if (!s->probe_pending) {
            s->probe_pending = true;
            s->probe_start_s = s->now;
        }
        break;
    case NET_LADDER_ACT: {
        s->dispatches++;
        bool accepted = !s->reject_dispatch && !s->queued;
        if (accepted) {
            s->queued = true;
            s->queued_generation = o.generation;
        }
        net_ladder_dispatched(&s->l, o.generation, accepted);
        break;
    }
    case NET_LADDER_SKIP:
        s->skips++;
        s->last_why = o.why;
        s->last_held_rung = o.rung;
        break;
    case NET_LADDER_WITHHELD:
        s->withheld++;
        s->last_why = o.why;
        s->last_held_rung = o.rung;
        break;
    case NET_LADDER_RESET:
        s->resets++;
        s->reset_rung = o.rung;
        break;
    default:
        break;
    }
    if (!s->delay_execution) {
        (void)sim_execute(s);
    }
    s->now++;
    return o;
}

static void sim_run(sim_t *s, uint32_t seconds)
{
    for (uint32_t i = 0; i < seconds; i++) {
        sim_step(s);
    }
}

static uint32_t total_acts(const sim_t *s)
{
    return s->acts[1] + s->acts[2] + s->acts[3] + s->acts[4];
}

TEST_CASE("network ladder: refused dispatch retains its rung until one execution is admitted", "[net_ladder]")
{
    sim_t s;
    sim_init(&s, NET_PROBE_NO_IP);
    s.reject_dispatch = true;
    sim_run(&s, 70);
    uint32_t generation = s.l.generation;
    TEST_ASSERT_TRUE(s.l.pending);
    TEST_ASSERT_FALSE(s.l.queued);
    TEST_ASSERT_EQUAL_UINT8(0, s.l.next);
    TEST_ASSERT_EQUAL(NET_RUNG_NONE, s.l.last_rung);
    TEST_ASSERT_EQUAL_UINT32(0, total_acts(&s));
    /* Even after the next rung's time, a full queue cannot consume this
     * one or report that an action started. */
    sim_run(&s, 200);
    TEST_ASSERT_EQUAL_UINT32(generation, s.l.generation);
    TEST_ASSERT_EQUAL_UINT8(0, s.l.next);
    TEST_ASSERT_EQUAL_UINT32(0, total_acts(&s));
    s.reject_dispatch = false;
    s.delay_execution = true;
    TEST_ASSERT_EQUAL(NET_LADDER_ACT, sim_step(&s).act);
    TEST_ASSERT_TRUE(s.l.queued);
    uint32_t dispatches = s.dispatches;
    sim_run(&s, 3);
    TEST_ASSERT_EQUAL_UINT32(dispatches, s.dispatches);   /* no duplicate queued command */
    TEST_ASSERT_EQUAL_UINT32(0, total_acts(&s));
    TEST_ASSERT_TRUE(sim_execute(&s));
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RECONNECT]);
    TEST_ASSERT_EQUAL_UINT8(1, s.l.next);
    TEST_ASSERT_EQUAL(NET_RUNG_RECONNECT, s.l.last_rung);
    net_ladder_out_t out;
    TEST_ASSERT_FALSE(net_ladder_begin(&s.l, generation, &s.in, s.now, s.now, &out));
    /* The original pass schedule remains: the overdue second rung asks
     * for its own fresh probe rather than silently having been skipped. */
    TEST_ASSERT_EQUAL(NET_LADDER_PROBE, sim_step(&s).act);
}

TEST_CASE("network ladder: queue refusal after a probe timeout does not start another probe wait", "[net_ladder]")
{
    sim_t s;
    sim_init(&s, NET_PROBE_NO_IP);
    s.probe_delay_s = UINT32_MAX;
    s.reject_dispatch = true;
    sim_run(&s, 125);
    TEST_ASSERT_TRUE(s.l.pending);
    TEST_ASSERT_EQUAL_UINT32(1, s.probes);
    TEST_ASSERT_GREATER_THAN_UINT32(1, s.dispatches);
    s.reject_dispatch = false;
    TEST_ASSERT_EQUAL(NET_LADDER_ACT, sim_step(&s).act);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RECONNECT]);
    TEST_ASSERT_EQUAL_UINT32(1, s.probes);
}

TEST_CASE("network ladder: recovery cancels queued work and old tokens cannot act in a later episode", "[net_ladder]")
{
    sim_t s;
    sim_init(&s, NET_PROBE_NO_IP);
    s.delay_execution = true;
    sim_run(&s, 65);
    uint32_t old = s.queued_generation;
    TEST_ASSERT_TRUE(s.queued);
    s.in.caster_down = false;
    sim_step(&s);   /* cancel immediately, without waiting for the episode's 60 s reset */
    TEST_ASSERT_FALSE(s.l.pending);
    TEST_ASSERT_EQUAL_UINT8(0, s.l.next);
    TEST_ASSERT_EQUAL(NET_LADDER_ACTIVE, s.l.phase);
    TEST_ASSERT_FALSE(sim_execute(&s));
    TEST_ASSERT_EQUAL_UINT32(0, total_acts(&s));
    sim_run(&s, NET_LADDER_HEALTHY_S);
    TEST_ASSERT_EQUAL(NET_LADDER_IDLE, s.l.phase);
    s.in.caster_down = true;
    sim_run(&s, 65);
    TEST_ASSERT_TRUE(s.l.pending);
    TEST_ASSERT_NOT_EQUAL(old, s.queued_generation);
    uint32_t current = s.queued_generation;
    net_ladder_out_t out;
    TEST_ASSERT_FALSE(net_ladder_begin(&s.l, old, &s.in, s.now, s.now, &out));
    net_ladder_dispatched(&s.l, old, false);
    net_ladder_cancel(&s.l, old);
    TEST_ASSERT_TRUE(s.l.pending);
    TEST_ASSERT_TRUE(s.l.queued);
    TEST_ASSERT_EQUAL_UINT32(current, s.l.generation);
    TEST_ASSERT_TRUE(sim_execute(&s));
    TEST_ASSERT_EQUAL_UINT32(1, total_acts(&s));
}

TEST_CASE("network ladder: admission rejects stale health and newly recovered inputs before the next tick", "[net_ladder]")
{
    for (unsigned scenario = 0; scenario < 5; scenario++) {
        sim_t s;
        sim_init(&s, NET_PROBE_NO_IP);
        s.delay_execution = true;
        sim_run(&s, 65);
        uint32_t observed = s.now;
        if (scenario == 0) {
            observed -= NET_LADDER_INPUT_MAX_AGE_S + 1;
        } else if (scenario == 1) {
            s.in.caster_down = false;
        } else if (scenario == 2) {
            s.in.rtcm_healthy = false;
        } else if (scenario == 3) {
            s.in.sta_configured = false;
        } else {
            s.in.probe = NET_PROBE_OK;
            s.in.probe_at_s = s.now;
        }
        net_ladder_out_t out;
        TEST_ASSERT_FALSE(net_ladder_begin(&s.l, s.queued_generation, &s.in, observed, s.now, &out));
        TEST_ASSERT_FALSE(s.l.pending);
        TEST_ASSERT_EQUAL_UINT8(0, s.l.next);
        TEST_ASSERT_EQUAL(NET_RUNG_NONE, s.l.last_rung);
        TEST_ASSERT_EQUAL_UINT32(0, total_acts(&s));
    }
}

TEST_CASE("network ladder: queued restart and reboot recheck their current protections", "[net_ladder]")
{
    for (unsigned scenario = 0; scenario < 5; scenario++) {
        sim_t s;
        sim_init(&s, NET_PROBE_NO_IP);
        sim_run(&s, scenario == 0 ? 300 : 500);
        s.delay_execution = true;
        sim_run(&s, scenario == 0 ? 70 : 110);
        TEST_ASSERT_TRUE(s.queued);
        uint8_t next = s.l.next;
        uint8_t last = s.l.last_rung;
        if (scenario == 0) {
            s.in.ap_clients = 1;
        } else if (scenario == 1) {
            s.in.reboot_inhibited = true;
        } else if (scenario == 2) {
            s.in.journal_open = true;
        } else if (scenario == 3) {
            s.in.caster_down = false;
            s.in.assoc_no_ip_s = NET_LADDER_ASSOC_NO_IP_S;   /* A alone cannot reboot */
        } else {
            s.in.probe = NET_PROBE_NO_UPSTREAM;
            s.in.probe_at_s = s.now;
        }
        TEST_ASSERT_FALSE(sim_execute(&s));
        TEST_ASSERT_EQUAL_UINT8(next, s.l.next);   /* refused execution does not consume the rung */
        TEST_ASSERT_EQUAL_UINT8(last, s.l.last_rung);
        TEST_ASSERT_EQUAL(NET_LADDER_ACTIVE, s.l.phase);   /* no reboot hold before admission */
        TEST_ASSERT_EQUAL_UINT32(0, s.acts[scenario == 0 ? NET_RUNG_RESTART_WIFI : NET_RUNG_REBOOT]);
        /* Normal policy then records the current withhold on the next tick. */
        TEST_ASSERT_EQUAL(NET_LADDER_WITHHELD, sim_step(&s).act);
    }
}

TEST_CASE("network ladder: cancellation generations wrap safely and late dispatch acknowledgements do nothing", "[net_ladder]")
{
    sim_t s;
    sim_init(&s, NET_PROBE_NO_IP);
    s.l.generation = UINT32_MAX;
    TEST_ASSERT_EQUAL(NET_LADDER_PASS, net_ladder_tick(&s.l, &s.in, s.now).act);
    s.now += NET_LADDER_RECONNECT_S;
    s.in.probe_valid = true;
    s.in.probe = NET_PROBE_NO_IP;
    s.in.probe_at_s = s.now;
    net_ladder_out_t offer = net_ladder_tick(&s.l, &s.in, s.now);
    TEST_ASSERT_EQUAL(NET_LADDER_ACT, offer.act);
    TEST_ASSERT_EQUAL_UINT32(1, offer.generation);   /* zero is never an automatic token */
    net_ladder_out_t out;
    TEST_ASSERT_FALSE(net_ladder_begin(&s.l, 0, &s.in, s.now, s.now, &out));
    TEST_ASSERT_TRUE(s.l.pending);
    net_ladder_cancel(&s.l, offer.generation);
    TEST_ASSERT_EQUAL_UINT8(0, s.l.next);
    offer = net_ladder_tick(&s.l, &s.in, s.now);
    TEST_ASSERT_EQUAL_UINT32(2, offer.generation);
    /* A queue consumer may run before xQueueSend returns to its caller. */
    TEST_ASSERT_TRUE(net_ladder_begin(&s.l, offer.generation, &s.in,
                                     s.now - NET_LADDER_INPUT_MAX_AGE_S, s.now, &out));
    net_ladder_dispatched(&s.l, offer.generation, true);
    TEST_ASSERT_FALSE(s.l.pending);
    TEST_ASSERT_FALSE(s.l.queued);
    TEST_ASSERT_EQUAL_UINT8(1, s.l.next);
    TEST_ASSERT_FALSE(net_ladder_begin(&s.l, offer.generation, &s.in, s.now, s.now, &out));
}

TEST_CASE("network ladder stays idle unless the network is the suspect", "[net_ladder]")
{
    sim_t s;
    sim_init(&s, NET_PROBE_NO_IP);
    s.in.caster_down = false;
    sim_run(&s, 3000);
    TEST_ASSERT_EQUAL_UINT32(0, total_acts(&s) + s.probes + s.skips + s.withheld);

    sim_init(&s, NET_PROBE_NO_IP);
    s.in.rtcm_healthy = false;   /* the GNSS path explains the caster */
    sim_run(&s, 3000);
    TEST_ASSERT_EQUAL_UINT32(0, total_acts(&s) + s.probes);

    sim_init(&s, NET_PROBE_NO_IP);
    s.in.sta_configured = false; /* setup AP: never "recovered" */
    sim_run(&s, 3000);
    TEST_ASSERT_EQUAL_UINT32(0, total_acts(&s) + s.probes);
    TEST_ASSERT_EQUAL(NET_LADDER_IDLE, s.l.phase);
}

TEST_CASE("network ladder escalates on its own side: 60 s, 3, 6, 10 min, across the clock wrap", "[net_ladder]")
{
    sim_t s;
    sim_init(&s, NET_PROBE_NO_IP);
    sim_run(&s, 700);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RECONNECT]);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RENEW_DHCP]);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RESTART_WIFI]);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_REBOOT]);
    /* Each rung runs once the probe it asked for (2 s) is in. */
    TEST_ASSERT_EQUAL_UINT32(T0 + 60 + 2, s.act_at[NET_RUNG_RECONNECT]);
    TEST_ASSERT_EQUAL_UINT32(T0 + 180 + 2, s.act_at[NET_RUNG_RENEW_DHCP]);
    TEST_ASSERT_EQUAL_UINT32(T0 + 360 + 2, s.act_at[NET_RUNG_RESTART_WIFI]);
    TEST_ASSERT_EQUAL_UINT32(T0 + 600 + 2, s.act_at[NET_RUNG_REBOOT]);
    TEST_ASSERT_EQUAL_UINT32(4, s.probes);
    TEST_ASSERT_EQUAL(NET_LADDER_HOLD, s.l.phase);
    TEST_ASSERT_EQUAL_UINT8(NET_RUNG_REBOOT, s.l.last_rung);
}

TEST_CASE("network ladder: a reachable internet means a remote fault, nothing is touched", "[net_ladder]")
{
    sim_t s;
    sim_init(&s, NET_PROBE_OK);
    sim_run(&s, 700);
    TEST_ASSERT_EQUAL_UINT32(0, total_acts(&s));
    TEST_ASSERT_EQUAL_UINT32(4, s.skips);
    TEST_ASSERT_EQUAL(NET_WHY_UPSTREAM_OK, s.last_why);
    TEST_ASSERT_EQUAL(NET_LADDER_HOLD, s.l.phase);

    /* The hold ends 10 min after the reboot rung (T0 + 602); a new pass
     * starts there and its first rung is skipped again 62 s later. */
    sim_run(&s, (T0 + 602 + NET_LADDER_HOLD_S + 60 + 2) - s.now + 1);
    TEST_ASSERT_EQUAL_UINT32(5, s.skips);
    TEST_ASSERT_EQUAL(NET_RUNG_RECONNECT, s.last_held_rung);
}

TEST_CASE("network ladder: IP without upstream runs the Wi-Fi rungs but never reboots", "[net_ladder]")
{
    sim_t s;
    sim_init(&s, NET_PROBE_NO_UPSTREAM);
    sim_run(&s, 700);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RECONNECT]);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RENEW_DHCP]);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RESTART_WIFI]);
    TEST_ASSERT_EQUAL_UINT32(0, s.acts[NET_RUNG_REBOOT]);
    TEST_ASSERT_EQUAL_UINT32(1, s.withheld);
    TEST_ASSERT_EQUAL(NET_WHY_NO_UPSTREAM, s.last_why);
    TEST_ASSERT_EQUAL(NET_RUNG_REBOOT, s.last_held_rung);

    /* A failing name lookup with a reachable resolver is on our side: reboot allowed. */
    sim_init(&s, NET_PROBE_DNS_FAILED);
    sim_run(&s, 700);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_REBOOT]);
}

TEST_CASE("network ladder: reboot withheld by an open flash journal, not by an unknown one", "[net_ladder]")
{
    sim_t s;
    sim_init(&s, NET_PROBE_NO_IP);
    s.in.journal_open = true;
    sim_run(&s, 700);
    TEST_ASSERT_EQUAL_UINT32(3, total_acts(&s));   /* the Wi-Fi rungs still run */
    TEST_ASSERT_EQUAL_UINT32(0, s.acts[NET_RUNG_REBOOT]);
    TEST_ASSERT_EQUAL(NET_WHY_JOURNAL_OPEN, s.last_why);

    /* Settings unavailable: the journal state is only unknown. */
    sim_init(&s, NET_PROBE_NO_IP);
    s.in.journal_open = true;
    s.in.journal_unknown = true;
    sim_run(&s, 700);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_REBOOT]);

    sim_init(&s, NET_PROBE_NO_IP);
    s.in.journal_unknown = true;
    sim_run(&s, 700);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_REBOOT]);
}

TEST_CASE("network ladder: reboot withheld while inhibited or while the AP is in use", "[net_ladder]")
{
    sim_t s;
    sim_init(&s, NET_PROBE_NO_IP);
    s.in.reboot_inhibited = true;
    sim_run(&s, 700);
    TEST_ASSERT_EQUAL_UINT32(0, s.acts[NET_RUNG_REBOOT]);
    TEST_ASSERT_EQUAL(NET_WHY_INHIBITED, s.last_why);

    /* Someone on the setup page: no Wi-Fi restart and no reboot. */
    sim_init(&s, NET_PROBE_NO_IP);
    s.in.ap_clients = 1;
    sim_run(&s, 700);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RECONNECT]);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RENEW_DHCP]);
    TEST_ASSERT_EQUAL_UINT32(0, s.acts[NET_RUNG_RESTART_WIFI]);
    TEST_ASSERT_EQUAL_UINT32(0, s.acts[NET_RUNG_REBOOT]);
    TEST_ASSERT_EQUAL_UINT32(2, s.withheld);
    TEST_ASSERT_EQUAL(NET_WHY_AP_IN_USE, s.last_why);
}

TEST_CASE("network ladder: minimum reboot interval over a three-day outage", "[net_ladder]")
{
    sim_t s;
    sim_init(&s, NET_PROBE_NO_IP);
    /* A network-ladder reboot happened an hour before this boot. */
    s.rebooted = true;
    s.last_reboot_s = T0 - 3600;
    uint32_t reboots = 0;
    uint32_t prev = 0;
    bool have_prev = false;
    uint32_t min_gap = UINT32_MAX;
    for (uint32_t i = 0; i < 3 * 86400; i++) {
        uint32_t before = s.acts[NET_RUNG_REBOOT];
        sim_step(&s);
        if (s.acts[NET_RUNG_REBOOT] != before) {
            reboots++;
            if (have_prev && s.last_reboot_s - prev < min_gap) {
                min_gap = s.last_reboot_s - prev;
            }
            prev = s.last_reboot_s;
            have_prev = true;
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(6 * 3600, min_gap);
    /* First allowed 5 h in, then one per 6-6.3 h (pass length 20 min). */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(11, reboots);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(12, reboots);
    TEST_ASSERT_EQUAL(NET_WHY_INTERVAL, s.last_why);
    /* Passes repeat every 20 min: three Wi-Fi actions per pass, bounded. */
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(3 * 24 * 3, s.acts[NET_RUNG_RECONNECT]);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(3 * 24 * 3 - 1, s.acts[NET_RUNG_RECONNECT]);
}

TEST_CASE("network ladder: a probe that never answers counts as no upstream", "[net_ladder]")
{
    sim_t s;
    sim_init(&s, NET_PROBE_NO_IP);
    s.probe_delay_s = UINT32_MAX;
    sim_run(&s, 200);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RECONNECT]);
    TEST_ASSERT_EQUAL_UINT32(T0 + 60 + NET_LADDER_PROBE_WAIT_S, s.act_at[NET_RUNG_RECONNECT]);
    sim_run(&s, 700);
    TEST_ASSERT_EQUAL_UINT32(0, s.acts[NET_RUNG_REBOOT]);
    TEST_ASSERT_EQUAL(NET_WHY_NO_UPSTREAM, s.last_why);
}

TEST_CASE("network ladder: only a probe finished after the rung became due counts", "[net_ladder]")
{
    sim_t s;
    sim_init(&s, NET_PROBE_NO_IP);
    /* An old "upstream OK" from before the episode. */
    s.in.probe_valid = true;
    s.in.probe = NET_PROBE_OK;
    s.in.probe_at_s = T0 - 5;
    sim_run(&s, 60);
    TEST_ASSERT_EQUAL_UINT32(0, s.probes);
    net_ladder_out_t o = sim_step(&s);   /* rung 1 due now */
    TEST_ASSERT_EQUAL(NET_LADDER_PROBE, o.act);
    /* Asked once, then waits. */
    o = sim_step(&s);
    TEST_ASSERT_EQUAL(NET_LADDER_NONE, o.act);
    sim_run(&s, 2);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RECONNECT]);
    TEST_ASSERT_EQUAL_UINT32(1, s.probes);
}

TEST_CASE("network ladder: resets after 60 s healthy, a shorter spell keeps escalating", "[net_ladder]")
{
    sim_t s;
    sim_init(&s, NET_PROBE_NO_IP);
    sim_run(&s, 100);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RECONNECT]);

    /* Streams again for 59 s, then drops: the pass clock kept running. */
    s.in.caster_down = false;
    sim_run(&s, 59);
    TEST_ASSERT_EQUAL_UINT32(0, s.resets);
    s.in.caster_down = true;
    sim_run(&s, 30);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RENEW_DHCP]);
    TEST_ASSERT_EQUAL_UINT32(T0 + 180 + 2, s.act_at[NET_RUNG_RENEW_DHCP]);

    /* 60 s healthy: back to idle, journalled with the last rung started. */
    s.in.caster_down = false;
    sim_run(&s, 60);
    TEST_ASSERT_EQUAL_UINT32(0, s.resets);
    sim_run(&s, 1);
    TEST_ASSERT_EQUAL_UINT32(1, s.resets);
    TEST_ASSERT_EQUAL(NET_RUNG_RENEW_DHCP, s.reset_rung);
    TEST_ASSERT_EQUAL(NET_LADDER_IDLE, s.l.phase);

    /* A new episode starts from the first rung. */
    s.in.caster_down = true;
    uint32_t start = s.now;
    sim_run(&s, 70);
    TEST_ASSERT_EQUAL_UINT32(2, s.acts[NET_RUNG_RECONNECT]);
    TEST_ASSERT_EQUAL_UINT32(start + 60 + 2, s.act_at[NET_RUNG_RECONNECT]);

    /* RTCM going away also ends it (the GNSS ladder owns that case). */
    s.in.rtcm_healthy = false;
    sim_run(&s, 61);
    TEST_ASSERT_EQUAL_UINT32(2, s.resets);
    TEST_ASSERT_EQUAL(NET_RUNG_RECONNECT, s.reset_rung);

    /* A reset before any action reports no rung. */
    s.in.rtcm_healthy = true;
    sim_run(&s, 30);
    s.in.rtcm_healthy = false;
    sim_run(&s, 61);
    TEST_ASSERT_EQUAL_UINT32(3, s.resets);
    TEST_ASSERT_EQUAL(NET_RUNG_NONE, s.reset_rung);
}

/* ---- triggers beyond the protected caster ------------------------------------- */

/* Count pass starts and remember their triggers. */
static uint32_t run_counting_passes(sim_t *s, uint32_t seconds, uint8_t *last_triggers)
{
    uint32_t passes = 0;
    for (uint32_t i = 0; i < seconds; i++) {
        net_ladder_out_t o = sim_step(s);
        if (o.act == NET_LADDER_PASS) {
            passes++;
            *last_triggers = o.triggers;
        }
    }
    return passes;
}

TEST_CASE("network ladder: associated without an IP runs the Wi-Fi rungs, never the reboot", "[net_ladder]")
{
    sim_t s;
    sim_init(&s, NET_PROBE_NO_IP);
    s.in.caster_down = false;              /* no protected trigger at all */
    s.in.assoc_no_ip_s = NET_LADDER_ASSOC_NO_IP_S - 1;
    sim_run(&s, 300);
    TEST_ASSERT_EQUAL(NET_LADDER_IDLE, s.l.phase);

    s.in.assoc_no_ip_s = NET_LADDER_ASSOC_NO_IP_S;
    uint8_t trig = 0;
    uint32_t passes = run_counting_passes(&s, 700, &trig);
    TEST_ASSERT_EQUAL_UINT32(1, passes);
    TEST_ASSERT_EQUAL_UINT8(NET_TRIG_A, trig);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RECONNECT]);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RENEW_DHCP]);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RESTART_WIFI]);
    TEST_ASSERT_EQUAL_UINT32(0, s.acts[NET_RUNG_REBOOT]);
    TEST_ASSERT_EQUAL(NET_WHY_NO_REBOOT_TRIGGER, s.last_why);
    TEST_ASSERT_EQUAL(NET_RUNG_REBOOT, s.last_held_rung);
    TEST_ASSERT_EQUAL(NET_LADDER_HOLD, s.l.phase);

    /* the hold, then a new pass repeats rungs 1-3 */
    passes = run_counting_passes(&s, NET_LADDER_HOLD_S + 400, &trig);
    TEST_ASSERT_EQUAL_UINT32(1, passes);
    TEST_ASSERT_EQUAL_UINT32(2, s.acts[NET_RUNG_RESTART_WIFI]);
    TEST_ASSERT_EQUAL_UINT32(0, s.acts[NET_RUNG_REBOOT]);
}

TEST_CASE("network ladder: a slot that never reaches its caster runs the Wi-Fi rungs, never the reboot", "[net_ladder]")
{
    sim_t s;
    sim_init(&s, NET_PROBE_DNS_FAILED);    /* would allow a reboot for the protected trigger */
    s.in.caster_down = false;
    s.in.unreached_s = NET_LADDER_UNREACHED_S - 1;
    sim_run(&s, 300);
    TEST_ASSERT_EQUAL(NET_LADDER_IDLE, s.l.phase);

    s.in.unreached_s = NET_LADDER_UNREACHED_S;
    uint8_t trig = 0;
    TEST_ASSERT_EQUAL_UINT32(1, run_counting_passes(&s, 700, &trig));
    TEST_ASSERT_EQUAL_UINT8(NET_TRIG_S, trig);
    TEST_ASSERT_EQUAL_UINT32(3, total_acts(&s));
    TEST_ASSERT_EQUAL_UINT32(0, s.acts[NET_RUNG_REBOOT]);
    TEST_ASSERT_EQUAL(NET_WHY_NO_REBOOT_TRIGGER, s.last_why);

    /* the run ends (a refused connect, a success): reset after 60 s */
    s.in.unreached_s = 0;
    sim_run(&s, NET_LADDER_HEALTHY_S + 1);
    TEST_ASSERT_EQUAL(NET_LADDER_IDLE, s.l.phase);

    /* RTCM does not matter for A and S, credentials do */
    sim_init(&s, NET_PROBE_NO_IP);
    s.in.caster_down = false;
    s.in.rtcm_healthy = false;
    s.in.unreached_s = 600;
    sim_run(&s, 100);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RECONNECT]);
    sim_init(&s, NET_PROBE_NO_IP);
    s.in.sta_configured = false;
    s.in.assoc_no_ip_s = 600;
    s.in.unreached_s = 600;
    sim_run(&s, 700);
    TEST_ASSERT_EQUAL_UINT32(0, total_acts(&s) + s.probes);
}

TEST_CASE("network ladder: its own reconnect does not end an episode of A or S", "[net_ladder]")
{
    /* A: the router's DHCP server is off. Every Wi-Fi rung drops the
     * association; it is back 3 s later (all-channel scan), and A needs
     * another 60 s after that. */
    sim_t s;
    sim_init(&s, NET_PROBE_NO_IP);
    s.in.caster_down = false;
    s.in.has_ip = false;
    uint32_t assoc_at = s.now - NET_LADDER_ASSOC_NO_IP_S;
    uint32_t back_at = 0;
    bool down = false;
    for (uint32_t i = 0; i < 700; i++) {
        if (down && (int32_t)(s.now - back_at) >= 0) {
            down = false;
            assoc_at = s.now;
        }
        s.in.assoc_no_ip_s = down ? 0 : s.now - assoc_at;
        uint32_t before = total_acts(&s);
        sim_step(&s);
        if (total_acts(&s) != before) {
            down = true;
            back_at = s.now + 3;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(0, s.resets);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RECONNECT]);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RENEW_DHCP]);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RESTART_WIFI]);
    TEST_ASSERT_EQUAL_UINT32(0, s.acts[NET_RUNG_REBOOT]);
    TEST_ASSERT_EQUAL(NET_LADDER_HOLD, s.l.phase);

    /* S: the router's DNS is broken. A rung takes the address for 5 s, which
     * ends every slot's unreached run; the slots fail again from 2 s after
     * the address is back, and S needs 180 s of that. */
    sim_init(&s, NET_PROBE_DNS_FAILED);
    s.in.caster_down = false;
    uint32_t run_from = s.now - NET_LADDER_UNREACHED_S;
    down = false;
    for (uint32_t i = 0; i < 1000; i++) {
        if (down && (int32_t)(s.now - back_at) >= 0) {
            down = false;
            run_from = s.now + 2;
        }
        s.in.has_ip = !down;
        s.in.unreached_s = (down || (int32_t)(s.now - run_from) < 0) ? 0 : s.now - run_from;   /* across the wrap */
        uint32_t before = total_acts(&s);
        sim_step(&s);
        if (total_acts(&s) != before) {
            down = true;
            back_at = s.now + 5;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(0, s.resets);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RECONNECT]);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RENEW_DHCP]);   /* the fallback DNS, the actual remedy */
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RESTART_WIFI]);
    TEST_ASSERT_EQUAL_UINT32(0, s.acts[NET_RUNG_REBOOT]);

    /* Healthy again: an address and no slot in an unreached run for 60 s. */
    s.in.has_ip = true;
    s.in.unreached_s = 0;
    sim_run(&s, NET_LADDER_HEALTHY_S + 1);
    TEST_ASSERT_EQUAL_UINT32(1, s.resets);
    TEST_ASSERT_EQUAL(NET_LADDER_IDLE, s.l.phase);
}

TEST_CASE("network ladder: S alone with the internet reachable holds without new passes", "[net_ladder]")
{
    /* A secondary caster's host name does not resolve (or its host is gone):
     * the first pass skips every rung, then the ladder only looks again. */
    sim_t s;
    sim_init(&s, NET_PROBE_OK);
    s.in.caster_down = false;
    s.in.unreached_s = 3600;
    uint8_t trig = 0;
    uint32_t passes = run_counting_passes(&s, 6 * 3600, &trig);
    TEST_ASSERT_EQUAL_UINT32(1, passes);
    TEST_ASSERT_EQUAL_UINT8(NET_TRIG_S, trig);
    TEST_ASSERT_EQUAL_UINT32(4, s.skips);
    TEST_ASSERT_EQUAL_UINT32(0, total_acts(&s));
    TEST_ASSERT_EQUAL(NET_LADDER_HOLD, s.l.phase);
    TEST_ASSERT_GREATER_THAN_UINT32(20, s.probes);   /* one probe per hold */

    /* The internet goes away: the next hold end starts a pass that acts. */
    s.world = NET_PROBE_DNS_FAILED;
    passes = run_counting_passes(&s, NET_LADDER_HOLD_S + 100, &trig);
    TEST_ASSERT_EQUAL_UINT32(1, passes);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RECONNECT]);

    /* With another trigger as well, the passes run as before. */
    sim_init(&s, NET_PROBE_OK);
    s.in.unreached_s = 3600;   /* and the protected caster down */
    passes = run_counting_passes(&s, 2 * (NET_LADDER_REBOOT_S + NET_LADDER_HOLD_S) + 100, &trig);
    TEST_ASSERT_EQUAL_UINT32(3, passes);
}

TEST_CASE("network ladder: only the protected trigger at the reboot rung reboots", "[net_ladder]")
{
    /* P alone reboots as before, and its pass start says so */
    sim_t s;
    sim_init(&s, NET_PROBE_NO_IP);
    uint8_t trig = 0;
    run_counting_passes(&s, 700, &trig);
    TEST_ASSERT_EQUAL_UINT8(NET_TRIG_P, trig);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_REBOOT]);

    /* P at the pass start, gone by the reboot rung while A keeps the pass alive: withheld */
    sim_init(&s, NET_PROBE_NO_IP);
    s.in.assoc_no_ip_s = 120;
    run_counting_passes(&s, 400, &trig);
    TEST_ASSERT_EQUAL_UINT8(NET_TRIG_P | NET_TRIG_A, trig);
    s.in.caster_down = false;
    sim_run(&s, 300);
    TEST_ASSERT_EQUAL_UINT32(0, s.acts[NET_RUNG_REBOOT]);
    TEST_ASSERT_EQUAL(NET_WHY_NO_REBOOT_TRIGGER, s.last_why);

    /* a reachable internet skips the rungs for every trigger */
    sim_init(&s, NET_PROBE_OK);
    s.in.caster_down = false;
    s.in.assoc_no_ip_s = 120;
    s.in.unreached_s = 300;
    sim_run(&s, 700);
    TEST_ASSERT_EQUAL_UINT32(0, total_acts(&s));
    TEST_ASSERT_EQUAL_UINT32(4, s.skips);
    TEST_ASSERT_EQUAL(NET_WHY_UPSTREAM_OK, s.last_why);
}

TEST_CASE("network ladder: AP clients hold the restart and reboot back for at most 30 min per episode", "[net_ladder]")
{
    sim_t s;
    sim_init(&s, NET_PROBE_NO_IP);
    s.in.ap_clients = 1;
    /* first pass: restart withheld at +362 s (the 30 min start here), reboot withheld at +602 s */
    sim_run(&s, 700);
    TEST_ASSERT_EQUAL_UINT32(0, s.acts[NET_RUNG_RESTART_WIFI]);
    TEST_ASSERT_EQUAL_UINT32(0, s.acts[NET_RUNG_REBOOT]);
    TEST_ASSERT_EQUAL(NET_WHY_AP_IN_USE, s.last_why);
    TEST_ASSERT_TRUE(s.l.ap_held);
    uint32_t held_at = s.l.ap_held_since_s;
    TEST_ASSERT_EQUAL_UINT32(T0 + 360 + 2, held_at);

    /* the timer survives the hold and the next pass */
    sim_run(&s, NET_LADDER_HOLD_S + 400);
    TEST_ASSERT_EQUAL_UINT32(held_at, s.l.ap_held_since_s);
    TEST_ASSERT_EQUAL_UINT32(0, s.acts[NET_RUNG_RESTART_WIFI]);

    /* from 30 min on the clients no longer hold anything back */
    uint32_t until = held_at + NET_LADDER_AP_HOLD_MAX_S;
    net_ladder_why_t act_why = NET_WHY_NONE;
    while (s.acts[NET_RUNG_RESTART_WIFI] == 0 && s.now - T0 < 4 * 3600) {
        net_ladder_out_t o = sim_step(&s);
        if (o.act == NET_LADDER_ACT && o.rung == NET_RUNG_RESTART_WIFI) {
            act_why = o.why;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RESTART_WIFI]);
    TEST_ASSERT_TRUE(s.act_at[NET_RUNG_RESTART_WIFI] - until < 0x80000000u);   /* not before the limit */
    TEST_ASSERT_EQUAL(NET_WHY_AP_OVERRIDDEN, act_why);
    while (s.acts[NET_RUNG_REBOOT] == 0 && s.now - T0 < 4 * 3600) {
        net_ladder_out_t o = sim_step(&s);
        if (o.act == NET_LADDER_ACT && o.rung == NET_RUNG_REBOOT) {
            act_why = o.why;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_REBOOT]);
    TEST_ASSERT_EQUAL(NET_WHY_AP_OVERRIDDEN, act_why);

    /* a ladder reset clears it: the next episode withholds again */
    s.in.caster_down = false;
    sim_run(&s, NET_LADDER_HEALTHY_S + 1);
    TEST_ASSERT_EQUAL(NET_LADDER_IDLE, s.l.phase);
    TEST_ASSERT_FALSE(s.l.ap_held);
    s.in.caster_down = true;
    s.rebooted = false;
    sim_run(&s, 700);
    TEST_ASSERT_EQUAL_UINT32(1, s.acts[NET_RUNG_RESTART_WIFI]);
    TEST_ASSERT_EQUAL(NET_WHY_AP_IN_USE, s.last_why);
}

TEST_CASE("network ladder: the pass-start text names every trigger combination", "[net_ladder]")
{
    static const char *const want[8] = {
        "none",
        "protected caster down",
        "associated without IP",
        "protected caster down, associated without IP",
        "caster not reached",
        "protected caster down, caster not reached",
        "associated without IP, caster not reached",
        "protected caster down, associated without IP, caster not reached",
    };
    for (uint8_t t = 0; t < 8; t++) {
        char buf[80];
        size_t n = net_ladder_trigger_text(t, buf, sizeof(buf));
        TEST_ASSERT_EQUAL_STRING(want[t], buf);
        TEST_ASSERT_EQUAL(strlen(want[t]), n);
        TEST_ASSERT_NOT_EQUAL(' ', buf[0]);   /* no leading space */
    }
    /* Bits outside P, A and S are not triggers. */
    char buf[80];
    net_ladder_trigger_text(0xF8, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("none", buf);
    /* A short buffer is cut and stays terminated. */
    char small[10];
    TEST_ASSERT_EQUAL(9, net_ladder_trigger_text(NET_TRIG_P | NET_TRIG_S, small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("protected", small);
    TEST_ASSERT_EQUAL(0, net_ladder_trigger_text(NET_TRIG_P, small, 0));
}
