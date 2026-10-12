/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Update banner offers and the daily check schedule.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "update_policy.h"

#define DAY_S     86400u
#define T0_UNIX   1790000000LL   /* 2026-09 */

static const char *const s_bad_list[] = { "v0.4.0", "v0.5.0-rc.1", NULL };

static bool is_bad(const char *v, void *ctx)
{
    const char *const *list = (const char *const *)ctx;
    for (size_t i = 0; list[i] != NULL; i++) {
        if (strcmp(list[i], v) == 0) {
            return true;
        }
    }
    return false;
}

static fw_app_offer_t offer(const char *running, const char *latest, const char *stage1)
{
    fw_app_offer_t o;
    fw_policy_app_offer(running, latest, is_bad, (void *)s_bad_list, stage1, &o);
    return o;
}

TEST_CASE("banner offers a newer release only", "[update_policy]")
{
    fw_app_offer_t o = offer("v0.2.0", "v0.3.0", "");
    TEST_ASSERT_TRUE(o.available);
    TEST_ASSERT_FALSE(o.rolled_back);
    TEST_ASSERT_FALSE(o.retry_after_stage1);

    TEST_ASSERT_FALSE(offer("v0.3.0", "v0.3.0", "").available);   /* same */
    TEST_ASSERT_FALSE(offer("v0.3.1", "v0.3.0", "").available);   /* no downgrades */
    TEST_ASSERT_TRUE(offer("v0.3.0-rc.2", "v0.3.0", "").available); /* release after its pre-release */
    TEST_ASSERT_FALSE(offer("v0.2.0", "", "").available);         /* nothing known yet */
    TEST_ASSERT_FALSE(offer("v0.2.0", NULL, "").available);
    TEST_ASSERT_FALSE(offer("v0.2.0", "garbage", "").available);
}

TEST_CASE("banner offers nothing to a dev build", "[update_policy]")
{
    TEST_ASSERT_FALSE(offer("v0.3.0-4-g47fc47b-dirty+dev", "v9.0.0", "").available);
    TEST_ASSERT_FALSE(offer("unknown+dev", "v9.0.0", "").available);
    TEST_ASSERT_FALSE(offer("", "v9.0.0", "").available);
}

TEST_CASE("banner never offers a rolled-back version, but says so", "[update_policy]")
{
    fw_app_offer_t o = offer("v0.3.0", "v0.4.0", "");
    TEST_ASSERT_FALSE(o.available);
    TEST_ASSERT_TRUE(o.rolled_back);
    o = offer("v0.3.0", "v0.5.0-rc.1", "");
    TEST_ASSERT_FALSE(o.available);
    TEST_ASSERT_TRUE(o.rolled_back);
    /* Not newer: neither offered nor reported. */
    o = offer("v0.6.0", "v0.4.0", "");
    TEST_ASSERT_FALSE(o.available);
    TEST_ASSERT_FALSE(o.rolled_back);
    /* No list at all. */
    fw_policy_app_offer("v0.3.0", "v0.4.0", NULL, NULL, "", &o);
    TEST_ASSERT_TRUE(o.available);
}

TEST_CASE("banner offers a stage-1 rollback again, flagged", "[update_policy]")
{
    fw_app_offer_t o = offer("v0.2.0", "v0.3.0", "v0.3.0");
    TEST_ASSERT_TRUE(o.available);
    TEST_ASSERT_TRUE(o.retry_after_stage1);
    o = offer("v0.2.0", "v0.3.1", "v0.3.0");   /* a newer release replaced it */
    TEST_ASSERT_TRUE(o.available);
    TEST_ASSERT_FALSE(o.retry_after_stage1);
}

TEST_CASE("the Update click installs newer releases only", "[update_policy]")
{
    TEST_ASSERT_TRUE(fw_policy_app_install_allowed("v0.2.0", "v0.3.0"));
    TEST_ASSERT_FALSE(fw_policy_app_install_allowed("v0.3.0", "v0.3.0"));
    TEST_ASSERT_FALSE(fw_policy_app_install_allowed("v0.3.0", "v0.2.9"));
    TEST_ASSERT_TRUE(fw_policy_app_install_allowed("v0.3.0-1-gabc+dev", "v0.2.0"));   /* dev build: any release */
    TEST_ASSERT_FALSE(fw_policy_app_install_allowed("v0.2.0", "v0.3.0+dev"));
    TEST_ASSERT_FALSE(fw_policy_app_install_allowed("v0.2.0", ""));
    /* A rolled-back version is the user's choice (the UI warns). */
    TEST_ASSERT_TRUE(fw_policy_app_install_allowed("v0.3.0", "v0.4.0"));
}

static void add_gnss(fw_manifest_t *m, const char *model, uint32_t build)
{
    fw_manifest_gnss_t *g = &m->gnss[m->gnss_count++];
    strlcpy(g->model, model, sizeof(g->model));
    snprintf(g->version, sizeof(g->version), "R4.10Build%u", (unsigned)build);
    g->build = build;
}

#define REL "v1.2.0"   /* a release build */

TEST_CASE("receiver offer: exact model, higher build only", "[update_policy]")
{
    fw_manifest_t m;
    memset(&m, 0, sizeof(m));
    add_gnss(&m, "UM980", 30000);
    add_gnss(&m, "UM982", 18975);
    add_gnss(&m, "UM982", 13495);
    add_gnss(&m, "UM982C", 40000);

    const fw_manifest_gnss_t *g = fw_policy_gnss_offer(&m, REL, "UM982", 13495);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_UINT32(18975, g->build);
    TEST_ASSERT_NULL(fw_policy_gnss_offer(&m, REL, "UM982", 18975));   /* same build */
    TEST_ASSERT_NULL(fw_policy_gnss_offer(&m, REL, "UM982", 20000));   /* never a downgrade */
    TEST_ASSERT_NULL(fw_policy_gnss_offer(&m, REL, "UM982", 0));       /* build unknown */
    TEST_ASSERT_NULL(fw_policy_gnss_offer(&m, REL, "", 1));            /* model unknown */
    TEST_ASSERT_NULL(fw_policy_gnss_offer(&m, REL, "um982", 1));       /* exact match */
    TEST_ASSERT_NULL(fw_policy_gnss_offer(&m, REL, "UM98", 1));
    g = fw_policy_gnss_offer(&m, REL, "UM982C", 1);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_UINT32(40000, g->build);
    TEST_ASSERT_NULL(fw_policy_gnss_offer(NULL, REL, "UM982", 1));
}

TEST_CASE("receiver offer: a dev build is offered nothing", "[update_policy]")
{
    fw_manifest_t m;
    memset(&m, 0, sizeof(m));
    add_gnss(&m, "UM982", 18975);
    TEST_ASSERT_NOT_NULL(fw_policy_gnss_offer(&m, REL, "UM982", 13495));
    TEST_ASSERT_NULL(fw_policy_gnss_offer(&m, "v0.3.0-4-g47fc47b-dirty+dev", "UM982", 13495));
    TEST_ASSERT_NULL(fw_policy_gnss_offer(&m, "unknown+dev", "UM982", 13495));
    TEST_ASSERT_NULL(fw_policy_gnss_offer(&m, "", "UM982", 13495));
    TEST_ASSERT_NULL(fw_policy_gnss_offer(&m, NULL, "UM982", 13495));
}

/* ---- schedule ---------------------------------------------------------------------- */

static fw_sched_t sched(uint32_t uptime, bool trusted, int64_t now, int64_t last_check)
{
    fw_sched_t s;
    memset(&s, 0, sizeof(s));
    s.uptime_s = uptime;
    s.clock_trusted = trusted;
    s.now_unix = now;
    s.last_check_unix = last_check;
    s.interval_s = DAY_S;
    s.first_check_s = 600;
    return s;
}

TEST_CASE("schedule: the first check of a boot waits for the jitter", "[update_policy]")
{
    /* Due long ago: at the jittered time, not before. */
    fw_sched_t s = sched(10, true, T0_UNIX, T0_UNIX - 3 * (int64_t)DAY_S);
    TEST_ASSERT_EQUAL_UINT32(590, fw_sched_wait_s(&s));
    s.uptime_s = 600;
    TEST_ASSERT_EQUAL_UINT32(0, fw_sched_wait_s(&s));
    /* Never checked. */
    s = sched(100, true, T0_UNIX, 0);
    TEST_ASSERT_EQUAL_UINT32(500, fw_sched_wait_s(&s));
}

TEST_CASE("schedule: a reboot adds no check (once a day by the trusted clock)", "[update_policy]")
{
    /* Checked 2 h ago, then rebooted: next one 22 h from now. */
    fw_sched_t s = sched(700, true, T0_UNIX, T0_UNIX - 7200);
    TEST_ASSERT_EQUAL_UINT32(DAY_S - 7200, fw_sched_wait_s(&s));
    /* Due in 100 s, but the boot's first check waits for the jitter. */
    s = sched(10, true, T0_UNIX, T0_UNIX - (int64_t)DAY_S + 100);
    TEST_ASSERT_EQUAL_UINT32(590, fw_sched_wait_s(&s));
}

TEST_CASE("schedule: a stored time from the future is not trusted", "[update_policy]")
{
    fw_sched_t s = sched(900, true, T0_UNIX, T0_UNIX + 5 * (int64_t)DAY_S);
    TEST_ASSERT_EQUAL_UINT32(0, fw_sched_wait_s(&s));
}

TEST_CASE("schedule: without a trusted clock the first check waits at most 1 h", "[update_policy]")
{
    fw_sched_t s = sched(100, false, 0, T0_UNIX - 7200);
    TEST_ASSERT_EQUAL_UINT32(FW_SCHED_NO_CLOCK_WAIT_S - 100, fw_sched_wait_s(&s));
    s.uptime_s = FW_SCHED_NO_CLOCK_WAIT_S;
    TEST_ASSERT_EQUAL_UINT32(0, fw_sched_wait_s(&s));
    /* Clock arrives at 20 min: the stored time decides again. */
    s = sched(1200, true, T0_UNIX, T0_UNIX - 7200);
    TEST_ASSERT_EQUAL_UINT32(DAY_S - 7200, fw_sched_wait_s(&s));
    /* After a check without a clock: by uptime. */
    s = sched(3700, false, 0, 0);
    s.have_success = true;
    s.last_success_s = 3600;
    s.last_result = FW_CHECK_OK;
    s.last_attempt_s = 3600;
    TEST_ASSERT_EQUAL_UINT32(DAY_S - 100, fw_sched_wait_s(&s));
}

TEST_CASE("schedule: retries depend on how the check failed", "[update_policy]")
{
    /* No answer: retry after an hour. */
    fw_sched_t s = sched(4000, true, T0_UNIX, T0_UNIX - 3 * (int64_t)DAY_S);
    s.last_result = FW_CHECK_NO_RESPONSE;
    s.last_attempt_s = 4000;
    TEST_ASSERT_EQUAL_UINT32(FW_SCHED_RETRY_S, fw_sched_wait_s(&s));
    /* An answer without a usable manifest: a full interval. */
    s.last_result = FW_CHECK_UNUSABLE;
    TEST_ASSERT_EQUAL_UINT32(DAY_S, fw_sched_wait_s(&s));
    s.uptime_s = 4000 + DAY_S;
    TEST_ASSERT_EQUAL_UINT32(0, fw_sched_wait_s(&s));
}

TEST_CASE("schedule: a clock jumping back does not add checks", "[update_policy]")
{
    /* Checked at uptime 1000 (stored T0); the clock then jumps back a day. */
    fw_sched_t s = sched(2000, true, T0_UNIX - (int64_t)DAY_S, T0_UNIX);
    s.have_success = true;
    s.last_success_s = 1000;
    s.last_result = FW_CHECK_OK;
    s.last_attempt_s = 1000;
    TEST_ASSERT_EQUAL_UINT32(DAY_S - 1000, fw_sched_wait_s(&s));
}

TEST_CASE("schedule: one check a day over a week, reboots included", "[update_policy]")
{
    const uint32_t step = 60;
    int64_t persisted = 0;
    int checks = 0;
    int64_t last_at = 0;
    uint32_t boot_start = 0;   /* simulated time of this boot's start */
    fw_sched_t s = sched(0, false, 0, 0);
    for (uint32_t t = 0; t <= 7u * DAY_S; t += step) {
        /* Reboots: day 1.5 (user), day 3.2 and 3.25 (ladder), day 5 (update). */
        if (t == DAY_S + DAY_S / 2 || t == 3u * DAY_S + DAY_S / 5 || t == 3u * DAY_S + DAY_S / 4 || t == 5u * DAY_S) {
            boot_start = t;
            s = sched(0, false, 0, persisted);
        }
        s.uptime_s = t - boot_start;
        s.clock_trusted = s.uptime_s >= 120;   /* SNTP after 2 min */
        s.now_unix = s.clock_trusted ? T0_UNIX + t : 0;
        s.last_check_unix = persisted;
        if (fw_sched_wait_s(&s) == 0) {
            int64_t now = T0_UNIX + t;
            if (checks > 0) {
                TEST_ASSERT_TRUE_MESSAGE(now - last_at >= (int64_t)DAY_S, "two checks within a day");
            }
            checks++;
            last_at = now;
            s.last_result = FW_CHECK_OK;
            s.last_attempt_s = s.uptime_s;
            s.have_success = true;
            s.last_success_s = s.uptime_s;
            if (s.clock_trusted) {
                persisted = now;
            }
        }
    }
    /* The first check 10 min after power-on, then every 24 h (+ a step). */
    TEST_ASSERT_EQUAL_INT(7, checks);
}

TEST_CASE("schedule: jitter is 5-15 min and stable per MAC", "[update_policy]")
{
    uint8_t mac[6] = { 0x24, 0x6f, 0x28, 0x00, 0x00, 0x00 };
    uint32_t first = fw_sched_jitter_s(mac);
    TEST_ASSERT_EQUAL_UINT32(first, fw_sched_jitter_s(mac));
    uint32_t lo = UINT32_MAX;
    uint32_t hi = 0;
    for (unsigned i = 0; i < 2000; i++) {
        mac[4] = (uint8_t)(i >> 8);
        mac[5] = (uint8_t)i;
        uint32_t j = fw_sched_jitter_s(mac);
        TEST_ASSERT_TRUE(j >= FW_SCHED_JITTER_MIN_S && j <= FW_SCHED_JITTER_MIN_S + FW_SCHED_JITTER_SPAN_S);
        lo = j < lo ? j : lo;
        hi = j > hi ? j : hi;
    }
    /* Neighbouring MACs spread over the whole window. */
    TEST_ASSERT_TRUE(lo < 330);
    TEST_ASSERT_TRUE(hi > 870);
}

TEST_CASE("check outcome: a missing release is told apart from other answers", "[update_policy]")
{
    TEST_ASSERT_EQUAL(FW_OUTCOME_NO_RELEASE, fw_policy_check_outcome(FW_HTTP_BAD_ANSWER, 404, false));
    TEST_ASSERT_EQUAL(FW_OUTCOME_HTTP_ERROR, fw_policy_check_outcome(FW_HTTP_BAD_ANSWER, 500, false));
    TEST_ASSERT_EQUAL(FW_OUTCOME_HTTP_ERROR, fw_policy_check_outcome(FW_HTTP_BAD_ANSWER, 302, false));   /* too many redirects */
    TEST_ASSERT_EQUAL(FW_OUTCOME_BAD_MANIFEST, fw_policy_check_outcome(FW_HTTP_BAD_ANSWER, 200, false));  /* too large */
    TEST_ASSERT_EQUAL(FW_OUTCOME_BAD_MANIFEST, fw_policy_check_outcome(FW_HTTP_OK, 200, false));
    TEST_ASSERT_EQUAL(FW_OUTCOME_OK, fw_policy_check_outcome(FW_HTTP_OK, 200, true));
    TEST_ASSERT_EQUAL(FW_OUTCOME_OK, fw_policy_check_outcome(FW_HTTP_NOT_MODIFIED, 304, false));
    TEST_ASSERT_EQUAL(FW_OUTCOME_NO_RESPONSE, fw_policy_check_outcome(FW_HTTP_NO_RESPONSE, 0, false));
    TEST_ASSERT_EQUAL(FW_OUTCOME_NO_RESPONSE, fw_policy_check_outcome(FW_HTTP_LOCAL_ERROR, 0, false));
    TEST_ASSERT_EQUAL(FW_OUTCOME_UNTRUSTED_REDIRECT, fw_policy_check_outcome(FW_HTTP_UNTRUSTED, 302, false));
}
