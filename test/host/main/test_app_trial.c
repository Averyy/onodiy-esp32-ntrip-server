/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * App trial after an update: records, the rolled-back list, boot bookkeeping
 * and the running trial (confirm, verify window, crash watch).
 */
#include <stdio.h>
#include <string.h>

#include "app_trial.h"
#include "unity.h"

#define CONFIRM_S 60u
#define WINDOW_S  600u
#define WATCH_S   (24u * 3600u)
#define T0_UNIX   1790000000LL

static fw_trial_rec_t rec_with(const char *version, fw_trial_rec_stage_t stage)
{
    fw_trial_rec_t r;
    memset(&r, 0, sizeof(r));
    strlcpy(r.version, version, sizeof(r.version));
    r.stage = (uint8_t)stage;
    r.attempt = 1;
    r.baseline_known = true;
    return r;
}

static fw_trial_boot_in_t boot_in(const char *running, bool pending)
{
    fw_trial_boot_in_t in = {
        .running_version = running,
        .pending_verify = pending,
        .crash_reset = false,
        .prev_uptime_s = UINT32_MAX,
        .last_invalid_version = "",
        .crash_revert_count = 3,
    };
    return in;
}

/* ---- codecs ---------------------------------------------------------------------- */

TEST_CASE("trial record round-trips and rejects damage", "[app_trial]")
{
    fw_trial_rec_t r = rec_with("v0.3.0", FW_TRIAL_REC_WATCHING);
    r.attempt = 2;
    r.baseline = FW_BASELINE_STA_IP | FW_BASELINE_HUB_FED;
    r.streaming_slots = 0x05;
    r.installed_unix = T0_UNIX;
    r.watch_start_unix = T0_UNIX + 90;
    r.watch_uptime_s = 7200;
    r.confirm_uptime_s = 61;
    r.boots = 3;
    r.crashes = 1;
    uint8_t buf[FW_TRIAL_REC_MAX_BLOB];
    size_t n = fw_trial_rec_encode(&r, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    fw_trial_rec_t d;
    TEST_ASSERT_TRUE(fw_trial_rec_decode(buf, n, &d));
    TEST_ASSERT_EQUAL_MEMORY(&r, &d, sizeof(r));

    /* Truncated, empty, unknown stage, version 0: unreadable. */
    TEST_ASSERT_FALSE(fw_trial_rec_decode(buf, n - 1, &d));
    TEST_ASSERT_FALSE(fw_trial_rec_decode(buf, 0, &d));
    uint8_t bad[FW_TRIAL_REC_MAX_BLOB];
    memcpy(bad, buf, n);
    bad[1 + 1 + strlen(r.version)] = FW_TRIAL_REC_COUNT;
    TEST_ASSERT_FALSE(fw_trial_rec_decode(bad, n, &d));
    memcpy(bad, buf, n);
    bad[0] = 0;
    TEST_ASSERT_FALSE(fw_trial_rec_decode(bad, n, &d));

    /* A newer writer's version byte and trailing fields are accepted. */
    uint8_t newer[FW_TRIAL_REC_MAX_BLOB + 8];
    memcpy(newer, buf, n);
    newer[0] = 2;
    memset(&newer[n], 0xAB, 8);
    TEST_ASSERT_TRUE(fw_trial_rec_decode(newer, n + 8, &d));
    TEST_ASSERT_EQUAL_STRING("v0.3.0", d.version);
    TEST_ASSERT_EQUAL_UINT16(1, d.crashes);

    /* Too small a buffer: nothing encoded. */
    TEST_ASSERT_EQUAL_size_t(0, fw_trial_rec_encode(&r, buf, 10));
}

TEST_CASE("a re-install after a stage-1 rollback is attempt 2", "[app_trial]")
{
    fw_trial_rec_t prev = rec_with("v0.3.0", FW_TRIAL_REC_ROLLED_BACK);
    fw_trial_rec_t r;
    fw_trial_rec_new(&r, &prev, "v0.3.0", FW_BASELINE_STA_IP, 0x01, T0_UNIX);
    TEST_ASSERT_EQUAL_UINT8(FW_TRIAL_REC_PENDING, r.stage);
    TEST_ASSERT_EQUAL_UINT8(2, r.attempt);
    TEST_ASSERT_TRUE(r.baseline_known);
    TEST_ASSERT_EQUAL_UINT8(0x01, r.streaming_slots);
    TEST_ASSERT_EQUAL_INT64(T0_UNIX, r.installed_unix);

    fw_trial_rec_new(&r, &prev, "v0.3.1", 0, 0, 0);       /* another version */
    TEST_ASSERT_EQUAL_UINT8(1, r.attempt);
    prev.stage = FW_TRIAL_REC_REVERTED;                   /* reverted by its trial: a fresh attempt */
    fw_trial_rec_new(&r, &prev, "v0.3.0", 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(1, r.attempt);
    fw_trial_rec_new(&r, NULL, "v0.3.0", 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(1, r.attempt);
}

TEST_CASE("rolled-back list: no duplicates, oldest dropped, round-trips", "[app_trial]")
{
    fw_bad_versions_t b;
    memset(&b, 0, sizeof(b));
    TEST_ASSERT_FALSE(fw_bad_contains(&b, "v0.1.0"));
    TEST_ASSERT_TRUE(fw_bad_add(&b, "v0.1.0"));
    TEST_ASSERT_FALSE(fw_bad_add(&b, "v0.1.0"));
    TEST_ASSERT_FALSE(fw_bad_add(&b, ""));
    TEST_ASSERT_FALSE(fw_bad_add(&b, NULL));
    TEST_ASSERT_TRUE(fw_bad_contains(&b, "v0.1.0"));
    char v[16];
    for (int i = 2; i <= FW_BAD_VER_MAX + 1; i++) {
        snprintf(v, sizeof(v), "v0.%d.0", i);
        TEST_ASSERT_TRUE(fw_bad_add(&b, v));
    }
    TEST_ASSERT_EQUAL_UINT8(FW_BAD_VER_MAX, b.count);
    TEST_ASSERT_FALSE(fw_bad_contains(&b, "v0.1.0"));   /* the oldest went */
    TEST_ASSERT_TRUE(fw_bad_contains(&b, "v0.2.0"));
    snprintf(v, sizeof(v), "v0.%d.0", FW_BAD_VER_MAX + 1);
    TEST_ASSERT_TRUE(fw_bad_contains(&b, v));

    uint8_t buf[FW_BAD_VER_MAX_BLOB];
    size_t n = fw_bad_encode(&b, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    fw_bad_versions_t d;
    TEST_ASSERT_TRUE(fw_bad_decode(buf, n, &d));
    TEST_ASSERT_EQUAL_UINT8(b.count, d.count);
    for (uint8_t i = 0; i < b.count; i++) {
        TEST_ASSERT_EQUAL_STRING(b.v[i], d.v[i]);
    }
    TEST_ASSERT_FALSE(fw_bad_decode(buf, n - 1, &d));
    buf[1] = FW_BAD_VER_MAX + 1;
    TEST_ASSERT_FALSE(fw_bad_decode(buf, n, &d));
}

TEST_CASE("rolled-back list is handed out newest first, as much as fits", "[app_trial]")
{
    fw_bad_versions_t b;
    memset(&b, 0, sizeof(b));
    char out[FW_BAD_VER_MAX][32];
    TEST_ASSERT_EQUAL_size_t(0, fw_bad_newest_first(&b, out, FW_BAD_VER_MAX));
    TEST_ASSERT_EQUAL_size_t(0, fw_bad_newest_first(NULL, out, FW_BAD_VER_MAX));
    fw_bad_add(&b, "v0.2.0");
    fw_bad_add(&b, "v0.3.0");
    fw_bad_add(&b, "v0.4.1");
    TEST_ASSERT_EQUAL_size_t(3, fw_bad_newest_first(&b, out, FW_BAD_VER_MAX));
    TEST_ASSERT_EQUAL_STRING("v0.4.1", out[0]);
    TEST_ASSERT_EQUAL_STRING("v0.3.0", out[1]);
    TEST_ASSERT_EQUAL_STRING("v0.2.0", out[2]);
    TEST_ASSERT_EQUAL_size_t(2, fw_bad_newest_first(&b, out, 2));
    TEST_ASSERT_EQUAL_STRING("v0.3.0", out[1]);
    b.count = FW_BAD_VER_MAX + 5;   /* a damaged count never reads past the list */
    TEST_ASSERT_EQUAL_size_t(FW_BAD_VER_MAX, fw_bad_newest_first(&b, out, FW_BAD_VER_MAX));
}

/* ---- boot bookkeeping ---------------------------------------------------------------- */

TEST_CASE("trial boot: a new image starts stage 1", "[app_trial]")
{
    fw_trial_rec_t r = rec_with("v0.3.0", FW_TRIAL_REC_PENDING);
    r.baseline = FW_BASELINE_STA_IP;
    fw_trial_boot_in_t in = boot_in("v0.3.0", true);
    fw_trial_boot_out_t out;
    fw_trial_on_boot(&r, &in, &out);
    TEST_ASSERT_EQUAL(FW_TRIAL_RUN_CONFIRMING, out.run);
    TEST_ASSERT_FALSE(out.persist);
    TEST_ASSERT_FALSE(out.revert);
    TEST_ASSERT_TRUE(r.baseline_known);
    TEST_ASSERT_EQUAL_UINT8(FW_BASELINE_STA_IP, r.baseline);
    TEST_ASSERT_EQUAL_UINT16(1, r.boots);

    /* Activated without a record (cut before it was written): no baseline. */
    fw_trial_rec_t none;
    memset(&none, 0, sizeof(none));
    fw_trial_on_boot(&none, &in, &out);
    TEST_ASSERT_EQUAL(FW_TRIAL_RUN_CONFIRMING, out.run);
    TEST_ASSERT_EQUAL_STRING("v0.3.0", none.version);
    TEST_ASSERT_EQUAL_UINT8(FW_TRIAL_REC_PENDING, none.stage);
    TEST_ASSERT_FALSE(none.baseline_known);
}

TEST_CASE("trial boot: a stage-1 rollback is recorded, the second one lists the version", "[app_trial]")
{
    fw_trial_rec_t r = rec_with("v0.3.0", FW_TRIAL_REC_PENDING);
    fw_trial_boot_in_t in = boot_in("v0.2.0", false);
    in.last_invalid_version = "v0.3.0";
    fw_trial_boot_out_t out;
    fw_trial_on_boot(&r, &in, &out);
    TEST_ASSERT_EQUAL(FW_TRIAL_RUN_NONE, out.run);
    TEST_ASSERT_TRUE(out.stage1_rollback);
    TEST_ASSERT_FALSE(out.add_bad);
    TEST_ASSERT_TRUE(out.persist);
    TEST_ASSERT_EQUAL_UINT8(FW_TRIAL_REC_ROLLED_BACK, r.stage);

    /* Next boot of the old image: nothing new. */
    fw_trial_on_boot(&r, &in, &out);
    TEST_ASSERT_FALSE(out.stage1_rollback);
    TEST_ASSERT_FALSE(out.persist);
    TEST_ASSERT_EQUAL_UINT8(FW_TRIAL_REC_ROLLED_BACK, r.stage);

    /* The user installs it again and it rolls back again. */
    fw_trial_rec_t again;
    fw_trial_rec_new(&again, &r, "v0.3.0", 0, 0, 0);
    fw_trial_on_boot(&again, &in, &out);
    TEST_ASSERT_TRUE(out.stage1_rollback);
    TEST_ASSERT_TRUE(out.add_bad);
}

TEST_CASE("trial boot: an activation that never took effect is forgotten", "[app_trial]")
{
    fw_trial_rec_t r = rec_with("v0.3.0", FW_TRIAL_REC_PENDING);
    fw_trial_boot_in_t in = boot_in("v0.2.0", false);
    in.last_invalid_version = "v0.1.0";   /* an older story */
    fw_trial_boot_out_t out;
    fw_trial_on_boot(&r, &in, &out);
    TEST_ASSERT_FALSE(out.stage1_rollback);
    TEST_ASSERT_TRUE(out.persist);
    TEST_ASSERT_EQUAL_UINT8(FW_TRIAL_REC_NONE, r.stage);
    TEST_ASSERT_EQUAL_STRING("", r.version);
}

TEST_CASE("trial boot: confirmed but not advanced continues with stage 2", "[app_trial]")
{
    fw_trial_rec_t r = rec_with("v0.3.0", FW_TRIAL_REC_PENDING);
    fw_trial_boot_in_t in = boot_in("v0.3.0", false);
    in.prev_uptime_s = 75;
    fw_trial_boot_out_t out;
    fw_trial_on_boot(&r, &in, &out);
    TEST_ASSERT_EQUAL(FW_TRIAL_RUN_VERIFYING, out.run);
    TEST_ASSERT_EQUAL_UINT8(FW_TRIAL_REC_VERIFYING, r.stage);
    TEST_ASSERT_EQUAL_UINT32(75, r.watch_uptime_s);
    TEST_ASSERT_TRUE(out.persist);
}

TEST_CASE("trial boot: stage 2 survives power blips and counts crash resets", "[app_trial]")
{
    fw_trial_rec_t r = rec_with("v0.3.0", FW_TRIAL_REC_VERIFYING);
    r.confirm_uptime_s = 60;   /* the confirming boot ran stage 2 from uptime 60 */
    fw_trial_boot_in_t in = boot_in("v0.3.0", false);
    in.prev_uptime_s = 400;
    fw_trial_boot_out_t out;
    fw_trial_on_boot(&r, &in, &out);
    TEST_ASSERT_EQUAL(FW_TRIAL_RUN_VERIFYING, out.run);
    TEST_ASSERT_EQUAL_UINT32(340, r.watch_uptime_s);
    TEST_ASSERT_EQUAL_UINT32(0, r.confirm_uptime_s);
    TEST_ASSERT_EQUAL_UINT16(0, r.crashes);
    TEST_ASSERT_FALSE(out.revert);

    /* Power cut: the session's uptime is unknown and nothing is added. */
    r.stage = FW_TRIAL_REC_WATCHING;
    in.prev_uptime_s = UINT32_MAX;
    fw_trial_on_boot(&r, &in, &out);
    TEST_ASSERT_EQUAL(FW_TRIAL_RUN_WATCHING, out.run);
    TEST_ASSERT_EQUAL_UINT32(340, r.watch_uptime_s);

    /* Crash resets: the third reverts. */
    in.crash_reset = true;
    in.prev_uptime_s = 1000;
    fw_trial_on_boot(&r, &in, &out);
    TEST_ASSERT_FALSE(out.revert);
    fw_trial_on_boot(&r, &in, &out);
    TEST_ASSERT_FALSE(out.revert);
    fw_trial_on_boot(&r, &in, &out);
    TEST_ASSERT_TRUE(out.revert);
    TEST_ASSERT_EQUAL_UINT16(3, r.crashes);
    TEST_ASSERT_EQUAL_UINT32(3340, r.watch_uptime_s);
    TEST_ASSERT_EQUAL_UINT16(5, r.boots);
}

TEST_CASE("trial boot: the previous image finds the image it gave way to", "[app_trial]")
{
    /* The trial image was given up before its record said so. */
    fw_trial_rec_t r = rec_with("v0.3.0", FW_TRIAL_REC_WATCHING);
    fw_trial_boot_in_t in = boot_in("v0.2.0", false);
    in.last_invalid_version = "v0.3.0";
    fw_trial_boot_out_t out;
    fw_trial_on_boot(&r, &in, &out);
    TEST_ASSERT_EQUAL(FW_TRIAL_RUN_NONE, out.run);
    TEST_ASSERT_TRUE(out.add_bad);
    TEST_ASSERT_EQUAL_UINT8(FW_TRIAL_REC_REVERTED, r.stage);

    /* Replaced another way (no invalid slot names it): forgotten. */
    r = rec_with("v0.3.0", FW_TRIAL_REC_VERIFYING);
    in.last_invalid_version = "";
    fw_trial_on_boot(&r, &in, &out);
    TEST_ASSERT_FALSE(out.add_bad);
    TEST_ASSERT_EQUAL_UINT8(FW_TRIAL_REC_NONE, r.stage);

    /* Reverted record, seen by the previous image: kept for the UI. */
    r = rec_with("v0.3.0", FW_TRIAL_REC_REVERTED);
    fw_trial_on_boot(&r, &in, &out);
    TEST_ASSERT_FALSE(out.persist);
    TEST_ASSERT_EQUAL_UINT8(FW_TRIAL_REC_REVERTED, r.stage);
    /* ... until that version runs again. */
    fw_trial_boot_in_t back = boot_in("v0.3.0", false);
    fw_trial_on_boot(&r, &back, &out);
    TEST_ASSERT_TRUE(out.persist);
    TEST_ASSERT_EQUAL_UINT8(FW_TRIAL_REC_NONE, r.stage);
}

/* ---- running trial -------------------------------------------------------------------- */

static fw_trial_tick_in_t tick_in(fw_trial_run_t run, uint32_t uptime)
{
    fw_trial_tick_in_t in = {
        .run = run,
        .uptime_s = uptime,
        .init_complete = true,
        .confirm_s = CONFIRM_S,
        .window_start_s = 60,
        .verify_window_s = WINDOW_S,
        .watch_elapsed_s = 0,
        .watch_s = WATCH_S,
    };
    return in;
}

TEST_CASE("trial: confirmation needs init complete and the confirm time", "[app_trial]")
{
    fw_trial_rec_t r = rec_with("v0.3.0", FW_TRIAL_REC_PENDING);
    fw_trial_obs_t obs;
    memset(&obs, 0, sizeof(obs));
    fw_trial_tick_in_t in = tick_in(FW_TRIAL_RUN_CONFIRMING, 59);
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_NONE, fw_trial_tick(&in, &r, &obs));
    in.uptime_s = 60;
    in.init_complete = false;
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_NONE, fw_trial_tick(&in, &r, &obs));
    in.init_complete = true;
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_CONFIRM, fw_trial_tick(&in, &r, &obs));
}

TEST_CASE("trial: the baseline must be seen within the verify window", "[app_trial]")
{
    fw_trial_rec_t r = rec_with("v0.3.0", FW_TRIAL_REC_VERIFYING);
    r.baseline = FW_BASELINE_STA_IP | FW_BASELINE_HUB_FED;
    r.streaming_slots = 0x03;
    fw_trial_obs_t obs;
    memset(&obs, 0, sizeof(obs));
    fw_trial_tick_in_t in = tick_in(FW_TRIAL_RUN_VERIFYING, 100);
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_NONE, fw_trial_tick(&in, &r, &obs));

    /* Parts seen at different moments count. */
    obs.sta_online_seen = true;
    obs.hub_fed_seen = true;
    obs.streaming_seen = 0x01;
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_NONE, fw_trial_tick(&in, &r, &obs));
    obs.streaming_seen |= 0x02 | 0x04;   /* an extra slot does not matter */
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_VERIFIED, fw_trial_tick(&in, &r, &obs));

    /* A slot that never streams again: revert when the window ends. */
    obs.streaming_seen = 0x01;
    in.uptime_s = 60 + WINDOW_S - 1;
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_NONE, fw_trial_tick(&in, &r, &obs));
    in.uptime_s = 60 + WINDOW_S;
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_REVERT, fw_trial_tick(&in, &r, &obs));
}

TEST_CASE("trial: a receiver problem excuses the hub and the casters, at the end only", "[app_trial]")
{
    fw_trial_rec_t r = rec_with("v0.3.0", FW_TRIAL_REC_VERIFYING);
    r.baseline = FW_BASELINE_STA_IP | FW_BASELINE_HUB_FED;
    r.streaming_slots = 0x01;
    fw_trial_obs_t obs = { .sta_online_seen = true, .receiver_problem = true };

    /* Early on the station is still detecting: no verdict yet. */
    fw_trial_tick_in_t in = tick_in(FW_TRIAL_RUN_VERIFYING, 120);
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_NONE, fw_trial_tick(&in, &r, &obs));
    /* Window over, antenna unplugged or receiver gone: not the image's fault. */
    in.uptime_s = 60 + WINDOW_S;
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_VERIFIED, fw_trial_tick(&in, &r, &obs));
    TEST_ASSERT_FALSE(fw_trial_baseline_reached(&r, &obs));
    TEST_ASSERT_TRUE(fw_trial_baseline_excused(&r, &obs));

    /* The receiver delivers but the casters do not stream: the image. */
    obs.hub_fed_seen = true;
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_REVERT, fw_trial_tick(&in, &r, &obs));
    /* The station never got online: never excused. */
    obs.hub_fed_seen = false;
    obs.sta_online_seen = false;
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_REVERT, fw_trial_tick(&in, &r, &obs));
    /* No receiver problem reported: the hub not being fed is the image's. */
    obs.sta_online_seen = true;
    obs.receiver_problem = false;
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_REVERT, fw_trial_tick(&in, &r, &obs));
}

TEST_CASE("trial: an unknown or empty baseline passes at once", "[app_trial]")
{
    fw_trial_obs_t obs;
    memset(&obs, 0, sizeof(obs));
    fw_trial_tick_in_t in = tick_in(FW_TRIAL_RUN_VERIFYING, 61);
    fw_trial_rec_t r = rec_with("v0.3.0", FW_TRIAL_REC_VERIFYING);
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_VERIFIED, fw_trial_tick(&in, &r, &obs));   /* nothing ran before the update */
    r.baseline = FW_BASELINE_STA_IP;
    r.baseline_known = false;
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_VERIFIED, fw_trial_tick(&in, &r, &obs));
}

TEST_CASE("trial: the crash watch ends after its time", "[app_trial]")
{
    fw_trial_rec_t r = rec_with("v0.3.0", FW_TRIAL_REC_WATCHING);
    fw_trial_obs_t obs;
    memset(&obs, 0, sizeof(obs));
    fw_trial_tick_in_t in = tick_in(FW_TRIAL_RUN_WATCHING, 5000);
    in.watch_elapsed_s = WATCH_S - 1;
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_NONE, fw_trial_tick(&in, &r, &obs));
    in.watch_elapsed_s = WATCH_S;
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_DONE, fw_trial_tick(&in, &r, &obs));
    in.run = FW_TRIAL_RUN_NONE;
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_NONE, fw_trial_tick(&in, &r, &obs));
}

TEST_CASE("trial: stage-2 time from uptime and from the clock", "[app_trial]")
{
    fw_trial_rec_t r = rec_with("v0.3.0", FW_TRIAL_REC_WATCHING);
    r.watch_uptime_s = 3600;
    /* Earlier boots plus this boot since stage 2 began. */
    TEST_ASSERT_EQUAL_UINT32(3600 + 940, fw_trial_watch_elapsed_s(&r, 1000, 60, false, 0));
    TEST_ASSERT_EQUAL_UINT32(3600, fw_trial_watch_elapsed_s(&r, 10, 60, false, 0));
    /* A power-off of 5 h shows only on the clock. */
    r.watch_start_unix = T0_UNIX;
    TEST_ASSERT_EQUAL_UINT32(5 * 3600 + 3600 + 940,
                             fw_trial_watch_elapsed_s(&r, 1000, 60, true, T0_UNIX + 5 * 3600 + 3600 + 940));
    /* A clock behind the uptime count never shortens it. */
    TEST_ASSERT_EQUAL_UINT32(4540, fw_trial_watch_elapsed_s(&r, 1000, 60, true, T0_UNIX + 10));
    TEST_ASSERT_EQUAL_UINT32(4540, fw_trial_watch_elapsed_s(&r, 1000, 60, true, T0_UNIX - 10));
}

TEST_CASE("trial: time left until a receiver update is possible", "[app_trial]")
{
    TEST_ASSERT_EQUAL_UINT32(40 + WATCH_S, fw_trial_left_s(FW_TRIAL_RUN_CONFIRMING, 20, CONFIRM_S, 0, WATCH_S));
    TEST_ASSERT_EQUAL_UINT32(WATCH_S, fw_trial_left_s(FW_TRIAL_RUN_CONFIRMING, 90, CONFIRM_S, 0, WATCH_S));
    TEST_ASSERT_EQUAL_UINT32(WATCH_S - 500, fw_trial_left_s(FW_TRIAL_RUN_VERIFYING, 600, CONFIRM_S, 500, WATCH_S));
    TEST_ASSERT_EQUAL_UINT32(0, fw_trial_left_s(FW_TRIAL_RUN_WATCHING, 600, CONFIRM_S, WATCH_S + 5, WATCH_S));
    TEST_ASSERT_EQUAL_UINT32(0, fw_trial_left_s(FW_TRIAL_RUN_NONE, 600, CONFIRM_S, 0, WATCH_S));
}

TEST_CASE("trial: a whole update over two days with a power cut and a crash", "[app_trial]")
{
    /* Install record written by the old image. */
    fw_trial_rec_t r;
    fw_trial_rec_new(&r, NULL, "v0.4.0", FW_BASELINE_STA_IP | FW_BASELINE_HUB_FED, 0x01, T0_UNIX);
    fw_trial_boot_out_t out;
    fw_trial_obs_t obs;
    memset(&obs, 0, sizeof(obs));

    /* Boot 1: stage 1, confirmed at 60 s, baseline at 200 s, power cut at 6 h. */
    fw_trial_boot_in_t in = boot_in("v0.4.0", true);
    fw_trial_on_boot(&r, &in, &out);
    TEST_ASSERT_EQUAL(FW_TRIAL_RUN_CONFIRMING, out.run);
    fw_trial_tick_in_t t = tick_in(FW_TRIAL_RUN_CONFIRMING, 60);
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_CONFIRM, fw_trial_tick(&t, &r, &obs));
    r.stage = FW_TRIAL_REC_VERIFYING;
    r.confirm_uptime_s = 60;
    t.run = FW_TRIAL_RUN_VERIFYING;
    t.uptime_s = 200;
    obs.sta_online_seen = obs.hub_fed_seen = true;
    obs.streaming_seen = 0x01;
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_VERIFIED, fw_trial_tick(&t, &r, &obs));
    r.stage = FW_TRIAL_REC_WATCHING;
    t.run = FW_TRIAL_RUN_WATCHING;
    t.uptime_s = 6 * 3600;
    t.watch_elapsed_s = fw_trial_watch_elapsed_s(&r, t.uptime_s, 60, false, 0);
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_NONE, fw_trial_tick(&t, &r, &obs));

    /* Boot 2 after the power cut: nothing added (uptime unknown), no clock. */
    in = boot_in("v0.4.0", false);
    fw_trial_on_boot(&r, &in, &out);
    TEST_ASSERT_EQUAL(FW_TRIAL_RUN_WATCHING, out.run);
    TEST_ASSERT_EQUAL_UINT32(0, r.watch_uptime_s);
    /* Runs 20 h, then a panic. */
    t.uptime_s = 20 * 3600;
    t.watch_elapsed_s = fw_trial_watch_elapsed_s(&r, t.uptime_s, 0, false, 0);
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_NONE, fw_trial_tick(&t, &r, &obs));

    /* Boot 3 after the crash: 20 h counted, one crash, keeps watching. */
    in.crash_reset = true;
    in.prev_uptime_s = 20 * 3600;
    fw_trial_on_boot(&r, &in, &out);
    TEST_ASSERT_EQUAL(FW_TRIAL_RUN_WATCHING, out.run);
    TEST_ASSERT_FALSE(out.revert);
    TEST_ASSERT_EQUAL_UINT16(1, r.crashes);
    TEST_ASSERT_EQUAL_UINT32(20 * 3600, r.watch_uptime_s);
    t.uptime_s = 4 * 3600 - 1;
    t.watch_elapsed_s = fw_trial_watch_elapsed_s(&r, t.uptime_s, 0, false, 0);
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_NONE, fw_trial_tick(&t, &r, &obs));
    t.uptime_s = 4 * 3600;
    t.watch_elapsed_s = fw_trial_watch_elapsed_s(&r, t.uptime_s, 0, false, 0);
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_DONE, fw_trial_tick(&t, &r, &obs));
}

TEST_CASE("trial: a reinstall of the identical image is stage 1 only", "[app_trial]")
{
    /* Installed as the image already running (same app ELF SHA-256, the user asked for it). */
    fw_trial_rec_t prev;
    memset(&prev, 0, sizeof(prev));
    fw_trial_rec_t r;
    fw_trial_rec_new(&r, &prev, "v0.3.0", FW_BASELINE_STA_IP | FW_BASELINE_HUB_FED, 0x01, T0_UNIX);
    r.same_image = true;

    /* The flag survives the record's storage. */
    uint8_t buf[FW_TRIAL_REC_MAX_BLOB];
    size_t n = fw_trial_rec_encode(&r, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    fw_trial_rec_t d;
    TEST_ASSERT_TRUE(fw_trial_rec_decode(buf, n, &d));
    TEST_ASSERT_TRUE(d.same_image);
    TEST_ASSERT_TRUE(d.baseline_known);

    /* First boot: stage 1 as for any image (the bootloader still guards it). */
    fw_trial_boot_in_t in = boot_in("v0.3.0", true);
    fw_trial_boot_out_t out;
    fw_trial_on_boot(&d, &in, &out);
    TEST_ASSERT_EQUAL(FW_TRIAL_RUN_CONFIRMING, out.run);
    fw_trial_obs_t obs;
    memset(&obs, 0, sizeof(obs));
    fw_trial_tick_in_t t = tick_in(FW_TRIAL_RUN_CONFIRMING, CONFIRM_S);
    t.init_complete = true;
    TEST_ASSERT_EQUAL(FW_TRIAL_ACT_CONFIRM, fw_trial_tick(&t, &d, &obs));
    /* Confirmed: no verify window and no crash watch follow. */
    TEST_ASSERT_EQUAL(FW_TRIAL_REC_NONE, fw_trial_after_confirm(&d));

    /* Confirmed but the record not advanced (a power cut): over, not stage 2. */
    fw_trial_boot_in_t later = boot_in("v0.3.0", false);
    later.prev_uptime_s = 75;
    fw_trial_on_boot(&d, &later, &out);
    TEST_ASSERT_EQUAL(FW_TRIAL_RUN_NONE, out.run);
    TEST_ASSERT_EQUAL_UINT8(FW_TRIAL_REC_NONE, d.stage);
    TEST_ASSERT_TRUE(out.persist);

    /* Any other install keeps the two-stage trial. */
    fw_trial_rec_new(&r, &prev, "v0.4.0", FW_BASELINE_STA_IP, 0x01, T0_UNIX);
    TEST_ASSERT_FALSE(r.same_image);
    TEST_ASSERT_EQUAL(FW_TRIAL_REC_VERIFYING, fw_trial_after_confirm(&r));
}
