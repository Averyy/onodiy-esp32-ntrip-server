/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Configuration planner and base-mode / base-position rules.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "um98x_base.h"
#include "um98x_plan.h"
#include "um98x_profile.h"
#include "unity.h"

#define STATION_ID 1234u

static void survey_profile(um98x_base_profile_t *p)
{
    um98x_profile_default(p, STATION_ID);
}

static void fixed_profile(um98x_base_profile_t *p)
{
    um98x_profile_default(p, STATION_ID);
    p->base_mode = UM98X_BASE_FIXED;
    p->fixed_lat_deg = 43.6532;
    p->fixed_lon_deg = -79.3832;
    p->fixed_height_m = 112.5;
}

/* Read-back that matches the profile completely. */
static void matching_readback(const um98x_base_profile_t *p, um98x_readback_t *rb)
{
    memset(rb, 0, sizeof(*rb));
    rb->config_ok = true;
    rb->config.signal_group_known = true;
    rb->config.signal_group_master = p->signal_group_master;
    rb->config.signal_group_slave = p->signal_group_slave;
    rb->mode_ok = true;
    um98x_mode_command(p, rb->mode_body, sizeof(rb->mode_body));
}

static void plan_text(const um98x_base_profile_t *p, const um98x_plan_t *plan, char *out, size_t cap)
{
    out[0] = '\0';
    for (size_t i = 0; i < plan->count; i++) {
        char cmd[UM98X_CMD_MAX];
        TEST_ASSERT_NOT_EQUAL(0, um98x_step_command(p, &plan->step[i], cmd, sizeof(cmd)));
        size_t n = strlen(out);
        snprintf(out + n, cap - n, "%s%s", n ? "|" : "", cmd);
    }
}

TEST_CASE("plan: nothing differs -> RTCM outputs and one save only", "[um98x_plan]")
{
    um98x_base_profile_t p;
    um98x_readback_t rb;
    um98x_plan_t plan;
    char text[512];
    survey_profile(&p);
    matching_readback(&p, &rb);
    TEST_ASSERT_TRUE(um98x_plan_build(&p, &rb, 115200, false, &plan));
    plan_text(&p, &plan, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("RTCM1005 COM1 1|RTCM1033 COM1 1|RTCM1077 COM1 1|RTCM1087 COM1 1|RTCM1097 COM1 1|"
                             "RTCM1117 COM1 1|RTCM1127 COM1 1|RTCM1137 COM1 1|RTCM1230 COM1 1|SAVECONFIG",
                             text);
    for (size_t i = 0; i < plan.count; i++) {
        bool optional = plan.step[i].kind == UM98X_STEP_RTCM && (plan.step[i].msg == 1230 || plan.step[i].msg == 1137);
        TEST_ASSERT_EQUAL(!optional, plan.step[i].fatal);
    }
}

TEST_CASE("plan: optional messages are 1230 and the NavIC MSMs, nothing else", "[um98x_plan]")
{
    /* A receiver firmware without them refuses them; the rest of the profile still applies. */
    TEST_ASSERT_TRUE(um98x_rtcm_optional(1230));
    for (uint16_t m = 1131; m <= 1137; m++) {
        TEST_ASSERT_TRUE(um98x_rtcm_optional(m));
    }
    static const uint16_t fatal[] = { 1005, 1006, 1033, 1077, 1087, 1097, 1117, 1127, 1130, 1138, 1019, 1229, 1231 };
    for (size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++) {
        TEST_ASSERT_FALSE(um98x_rtcm_optional(fatal[i]));
    }

    /* The restore plan marks them the same way. */
    um98x_base_profile_t p;
    um98x_plan_t plan;
    survey_profile(&p);
    TEST_ASSERT_TRUE(um98x_plan_rtcm_only(&p, &plan));
    TEST_ASSERT_EQUAL_UINT8(p.rtcm_count, plan.count);
    for (size_t i = 0; i < plan.count; i++) {
        TEST_ASSERT_EQUAL(!um98x_rtcm_optional(plan.step[i].msg), plan.step[i].fatal);
    }
}

TEST_CASE("plan: signal group first, then baud, base mode, outputs, save", "[um98x_plan]")
{
    um98x_base_profile_t p;
    um98x_readback_t rb;
    um98x_plan_t plan;
    char text[512];
    survey_profile(&p);
    p.com1_baud = 460800;
    p.sat_view_output = true;
    memset(&rb, 0, sizeof(rb));   /* nothing could be read back */
    TEST_ASSERT_TRUE(um98x_plan_build(&p, &rb, 115200, false, &plan));
    plan_text(&p, &plan, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("CONFIG SIGNALGROUP 3 6|CONFIG COM1 460800|MODE BASE 1234 TIME 600 0|"
                             "RTCM1005 COM1 1|RTCM1033 COM1 1|RTCM1077 COM1 1|RTCM1087 COM1 1|RTCM1097 COM1 1|"
                             "RTCM1117 COM1 1|RTCM1127 COM1 1|RTCM1137 COM1 1|RTCM1230 COM1 1|GPGSV COM1 10|SAVECONFIG",
                             text);
    TEST_ASSERT_FALSE(plan.step[plan.count - 2].fatal);   /* satellite view is optional */

    /* Never the user's ports, comma forms or a factory reset. */
    TEST_ASSERT_NULL(strstr(text, "COM2"));
    TEST_ASSERT_NULL(strstr(text, "COM3"));
    TEST_ASSERT_NULL(strchr(text, ','));
    TEST_ASSERT_NULL(strstr(text, "FRESET"));
}

TEST_CASE("plan: each step only when its read-back differs", "[um98x_plan]")
{
    um98x_base_profile_t p;
    um98x_readback_t rb;
    um98x_plan_t plan;
    survey_profile(&p);

    /* Signal group 4 5 (the receiver's factory value) -> sent. */
    matching_readback(&p, &rb);
    rb.config.signal_group_master = 4;
    rb.config.signal_group_slave = 5;
    um98x_plan_build(&p, &rb, 115200, false, &plan);
    TEST_ASSERT_EQUAL(UM98X_STEP_SIGNALGROUP, plan.step[0].kind);
    TEST_ASSERT_FALSE(um98x_signal_group_matches(&p, &rb));

    /* Signal group unknown in the dump -> treated as different. */
    matching_readback(&p, &rb);
    rb.config.signal_group_known = false;
    um98x_plan_build(&p, &rb, 115200, false, &plan);
    TEST_ASSERT_EQUAL(UM98X_STEP_SIGNALGROUP, plan.step[0].kind);

    /* Base mode read-back differs, unread, or a lock/re-survey forces it. */
    matching_readback(&p, &rb);
    strcpy(rb.mode_body, "MODE ROVER");
    um98x_plan_build(&p, &rb, 115200, false, &plan);
    TEST_ASSERT_EQUAL(UM98X_STEP_MODE, plan.step[0].kind);
    matching_readback(&p, &rb);
    rb.mode_ok = false;
    um98x_plan_build(&p, &rb, 115200, false, &plan);
    TEST_ASSERT_EQUAL(UM98X_STEP_MODE, plan.step[0].kind);
    matching_readback(&p, &rb);
    um98x_plan_build(&p, &rb, 115200, true, &plan);
    TEST_ASSERT_EQUAL(UM98X_STEP_MODE, plan.step[0].kind);
    um98x_plan_build(&p, &rb, 115200, false, &plan);
    TEST_ASSERT_EQUAL(UM98X_STEP_RTCM, plan.step[0].kind);

    /* Baud: only when the link runs at another baud. */
    um98x_plan_build(&p, &rb, 460800, false, &plan);
    TEST_ASSERT_EQUAL(UM98X_STEP_BAUD, plan.step[0].kind);

    /* Invalid profile: no plan. */
    p.station_id = 5000;
    TEST_ASSERT_FALSE(um98x_plan_build(&p, &rb, 115200, false, &plan));
    TEST_ASSERT_EQUAL_UINT8(0, plan.count);
}

TEST_CASE("plan: restoring output sends RTCM only", "[um98x_plan]")
{
    um98x_base_profile_t p;
    um98x_plan_t plan;
    survey_profile(&p);
    TEST_ASSERT_TRUE(um98x_plan_rtcm_only(&p, &plan));
    TEST_ASSERT_EQUAL_UINT8(p.rtcm_count, plan.count);
    for (size_t i = 0; i < plan.count; i++) {
        TEST_ASSERT_EQUAL(UM98X_STEP_RTCM, plan.step[i].kind);
    }
}

TEST_CASE("plan: base-mode commands, fixed positions as ECEF with 4 decimals", "[um98x_plan]")
{
    um98x_base_profile_t p;
    char cmd[UM98X_MODE_CMD_MAX];
    survey_profile(&p);
    TEST_ASSERT_NOT_EQUAL(0, um98x_mode_command(&p, cmd, sizeof(cmd)));
    TEST_ASSERT_EQUAL_STRING("MODE BASE 1234 TIME 600 0", cmd);

    fixed_profile(&p);
    TEST_ASSERT_NOT_EQUAL(0, um98x_mode_command(&p, cmd, sizeof(cmd)));
    double xyz[3];
    um98x_llh_to_ecef(p.fixed_lat_deg, p.fixed_lon_deg, p.fixed_height_m, xyz);
    char want[UM98X_MODE_CMD_MAX];
    snprintf(want, sizeof(want), "MODE BASE 1234 %.4f %.4f %.4f", xyz[0], xyz[1], xyz[2]);
    TEST_ASSERT_EQUAL_STRING(want, cmd);
    const char *dot = strrchr(cmd, '.');
    TEST_ASSERT_EQUAL_size_t(4, strlen(dot + 1));

    /* Longest ECEF form fits. */
    p.fixed_lat_deg = -0.0001;
    p.fixed_lon_deg = -179.9999;
    p.fixed_height_m = 9999.0;
    p.station_id = 4095;
    TEST_ASSERT_NOT_EQUAL(0, um98x_mode_command(&p, cmd, sizeof(cmd)));
    TEST_ASSERT_EQUAL(0, um98x_mode_command(&p, cmd, 10));
    TEST_ASSERT_EQUAL_STRING("", cmd);
    p.fixed_lat_deg = NAN;
    TEST_ASSERT_EQUAL(0, um98x_mode_command(&p, cmd, sizeof(cmd)));
}

TEST_CASE("base: survey read-back comparison", "[um98x_base]")
{
    um98x_base_profile_t p;
    survey_profile(&p);
    TEST_ASSERT_TRUE(um98x_mode_matches(&p, "MODE BASE 1234 TIME 600 0"));
    TEST_ASSERT_TRUE(um98x_mode_matches(&p, "mode base time 600 0"));   /* no ID echoed */
    TEST_ASSERT_TRUE(um98x_mode_matches(&p, "MODE BASE 1234 TIME 600"));
    TEST_ASSERT_TRUE(um98x_mode_matches(&p, "MODE BASE 1234 TIME 600 0.0"));
    TEST_ASSERT_FALSE(um98x_mode_matches(&p, "MODE BASE 1234 TIME 60 0"));
    TEST_ASSERT_FALSE(um98x_mode_matches(&p, "MODE BASE 1235 TIME 600 0"));   /* another ID */
    TEST_ASSERT_FALSE(um98x_mode_matches(&p, "MODE BASE 1234 TIME 600 5"));   /* reuses a position */
    TEST_ASSERT_FALSE(um98x_mode_matches(&p, "MODE BASE 1234"));
    TEST_ASSERT_FALSE(um98x_mode_matches(&p, "MODE BASE 1234 TIME"));
    TEST_ASSERT_FALSE(um98x_mode_matches(&p, "MODE ROVER"));
    TEST_ASSERT_FALSE(um98x_mode_matches(&p, ""));
    TEST_ASSERT_FALSE(um98x_mode_matches(&p, NULL));
    TEST_ASSERT_TRUE(um98x_mode_is_survey("MODE BASE 1234 TIME 600 0"));
    TEST_ASSERT_FALSE(um98x_mode_is_survey("MODE BASE 1234 1 2 3"));
}

TEST_CASE("base: fixed read-back within 0.5 mm of the ECEF position", "[um98x_base]")
{
    um98x_base_profile_t p;
    fixed_profile(&p);
    double xyz[3];
    um98x_llh_to_ecef(p.fixed_lat_deg, p.fixed_lon_deg, p.fixed_height_m, xyz);
    char body[160];

    snprintf(body, sizeof(body), "MODE BASE 1234 %.4f %.4f %.4f", xyz[0], xyz[1], xyz[2]);
    TEST_ASSERT_TRUE(um98x_mode_matches(&p, body));
    TEST_ASSERT_TRUE(um98x_mode_is_fixed_base(body));
    snprintf(body, sizeof(body), "MODE BASE %.4f %.4f %.4f", xyz[0], xyz[1], xyz[2]);   /* no ID echoed */
    TEST_ASSERT_TRUE(um98x_mode_matches(&p, body));
    snprintf(body, sizeof(body), "MODE BASE 1234 %.4f %.4f %.4f", xyz[0] + 0.0004, xyz[1] - 0.0004, xyz[2]);
    TEST_ASSERT_TRUE(um98x_mode_matches(&p, body));
    snprintf(body, sizeof(body), "MODE BASE 1234 %.4f %.4f %.4f", xyz[0], xyz[1], xyz[2] + 0.0007);
    TEST_ASSERT_FALSE(um98x_mode_matches(&p, body));
    snprintf(body, sizeof(body), "MODE BASE 99 %.4f %.4f %.4f", xyz[0], xyz[1], xyz[2]);
    TEST_ASSERT_FALSE(um98x_mode_matches(&p, body));

    /* The same position as latitude/longitude/height never matches. */
    snprintf(body, sizeof(body), "MODE BASE 1234 %.9f %.9f %.4f", p.fixed_lat_deg, p.fixed_lon_deg,
             p.fixed_height_m);
    TEST_ASSERT_FALSE(um98x_mode_matches(&p, body));
    /* A survey read-back against a fixed profile, and junk. */
    TEST_ASSERT_FALSE(um98x_mode_matches(&p, "MODE BASE 1234 TIME 600 0"));
    snprintf(body, sizeof(body), "MODE BASE 1234 %.4f %.4f %.4f EXTRA", xyz[0], xyz[1], xyz[2]);
    TEST_ASSERT_FALSE(um98x_mode_matches(&p, body));
    TEST_ASSERT_FALSE(um98x_mode_matches(&p, "MODE BASE 1234 1e6 2e6 3e6"));
    TEST_ASSERT_FALSE(um98x_mode_is_fixed_base("MODE ROVER"));
}

TEST_CASE("plan: detection candidates, short and full list", "[um98x_plan]")
{
    uint32_t c[UM98X_DETECT_MAX];
    size_t n = um98x_detect_candidates(115200, false, c, UM98X_DETECT_MAX);
    TEST_ASSERT_EQUAL_size_t(4, n);
    const uint32_t s1[] = { 115200, 460800, 921600, 230400 };
    TEST_ASSERT_EQUAL_UINT32_ARRAY(s1, c, 4);

    n = um98x_detect_candidates(57600, false, c, UM98X_DETECT_MAX);
    const uint32_t s2[] = { 57600, 115200, 460800, 921600, 230400 };
    TEST_ASSERT_EQUAL_size_t(5, n);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(s2, c, 5);

    n = um98x_detect_candidates(460800, true, c, UM98X_DETECT_MAX);
    const uint32_t f[] = { 460800, 115200, 921600, 230400, 57600, 38400, 19200, 9600 };
    TEST_ASSERT_EQUAL_size_t(8, n);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(f, c, 8);

    /* Nothing stored (or an unsupported value): the list starts at 115200. */
    n = um98x_detect_candidates(0, false, c, UM98X_DETECT_MAX);
    TEST_ASSERT_EQUAL_UINT32(115200, c[0]);
    TEST_ASSERT_EQUAL_size_t(4, n);
    n = um98x_detect_candidates(12345, true, c, 3);
    TEST_ASSERT_EQUAL_size_t(3, n);
}

TEST_CASE("plan: only the exact model UM982 is configured", "[um98x_plan]")
{
    TEST_ASSERT_TRUE(um98x_model_supported("UM982"));
    TEST_ASSERT_FALSE(um98x_model_supported("UM982C"));
    TEST_ASSERT_FALSE(um98x_model_supported("UMD982"));
    TEST_ASSERT_FALSE(um98x_model_supported("UM980"));
    TEST_ASSERT_FALSE(um98x_model_supported("um982"));
    TEST_ASSERT_FALSE(um98x_model_supported(""));
    TEST_ASSERT_FALSE(um98x_model_supported(NULL));
}

TEST_CASE("base: survey window, completion by read-back or by a stable 1005", "[um98x_base]")
{
    const int64_t t0 = (int64_t)UINT32_MAX + 777;
    um98x_survey_t s;
    memset(&s, 0, sizeof(s));
    TEST_ASSERT_FALSE(um98x_survey_window_open(&s, t0));

    um98x_survey_start(&s, t0, 600);
    TEST_ASSERT_TRUE(um98x_survey_window_open(&s, t0));
    TEST_ASSERT_EQUAL_UINT32(720, um98x_survey_window_left_s(&s, t0));
    um98x_survey_on_mode(&s, "MODE BASE 1234 TIME 600 0");
    TEST_ASSERT_FALSE(um98x_survey_done(&s, t0 + 1000));
    um98x_survey_on_mode(&s, "MODE BASE 1234 918634.1234 -4346125.5678 4561963.9012");
    TEST_ASSERT_TRUE(um98x_survey_done(&s, t0 + 1000));
    TEST_ASSERT_FALSE(um98x_survey_window_open(&s, t0 + 1000));

    /* Fallback: survey time passed and 60 frames within 1 mm. */
    um98x_survey_start(&s, t0, 600);
    int64_t p[3] = { 9186341234, -43461255678, 45619639012 };
    for (int i = 0; i < 59; i++) {
        p[0] += (i % 2) ? 3 : -3;   /* jitter of 0.3 mm */
        um98x_survey_on_arp(&s, p);
    }
    TEST_ASSERT_FALSE(um98x_survey_done(&s, t0 + 700000));   /* 59 frames */
    um98x_survey_on_arp(&s, p);
    TEST_ASSERT_FALSE(um98x_survey_done(&s, t0 + 599999));   /* too early */
    TEST_ASSERT_TRUE(um98x_survey_done(&s, t0 + 600000));

    /* A jump of more than 1 mm starts the run over. */
    p[2] += 11;
    um98x_survey_on_arp(&s, p);
    TEST_ASSERT_EQUAL_UINT32(1, s.stable_frames);
    TEST_ASSERT_FALSE(um98x_survey_done(&s, t0 + 600000));

    /* The window closes survey_s + 120 s after its start. */
    TEST_ASSERT_TRUE(um98x_survey_window_open(&s, t0 + 719999));
    TEST_ASSERT_EQUAL_UINT32(1, um98x_survey_window_left_s(&s, t0 + 719999));
    TEST_ASSERT_FALSE(um98x_survey_window_open(&s, t0 + 720000));
    TEST_ASSERT_EQUAL_UINT32(0, um98x_survey_window_left_s(&s, t0 + 720000));
}

TEST_CASE("base: lock the 1005 position, verify it, re-survey", "[um98x_base]")
{
    um98x_base_profile_t cur;
    um98x_base_profile_t next;
    um98x_base_position_t pos;
    survey_profile(&cur);
    double xyz[3];
    um98x_llh_to_ecef(43.6532, -79.3832, 112.5, xyz);
    int64_t arp[3] = { llround(xyz[0] * 1e4), llround(xyz[1] * 1e4), llround(xyz[2] * 1e4) };

    TEST_ASSERT_TRUE(um98x_lock_profile(&cur, arp, 1760000000, &next, &pos));
    TEST_ASSERT_EQUAL(UM98X_BASE_FIXED, next.base_mode);
    TEST_ASSERT_EQUAL(UM98X_POS_SURVEY, pos.source);
    TEST_ASSERT_EQUAL_INT64(1760000000, pos.locked_unix);
    TEST_ASSERT_DOUBLE_WITHIN(1e-8, 43.6532, next.fixed_lat_deg);
    TEST_ASSERT_DOUBLE_WITHIN(1e-8, -79.3832, next.fixed_lon_deg);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 112.5, next.fixed_height_m);
    TEST_ASSERT_TRUE(um98x_profile_valid(&next));
    TEST_ASSERT_TRUE(um98x_position_valid(&pos));
    TEST_ASSERT_EQUAL_UINT16(STATION_ID, next.station_id);
    TEST_ASSERT_EQUAL_UINT8(cur.rtcm_count, next.rtcm_count);

    /* What goes to the receiver is the same point within 0.1 mm. */
    char cmd[UM98X_MODE_CMD_MAX];
    um98x_mode_command(&next, cmd, sizeof(cmd));
    double sent[3];
    unsigned id;
    TEST_ASSERT_EQUAL_INT(4, sscanf(cmd, "MODE BASE %u %lf %lf %lf", &id, &sent[0], &sent[1], &sent[2]));
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_DOUBLE_WITHIN(0.00015, (double)arp[i] * 1e-4, sent[i]);
    }

    /* Verify: the next 1005 within 5 mm per axis. */
    TEST_ASSERT_TRUE(um98x_arp_near_profile(&next, arp, UM98X_LOCK_VERIFY_TOL_M));
    int64_t moved[3] = { arp[0] + 49, arp[1] - 49, arp[2] };
    TEST_ASSERT_TRUE(um98x_arp_near_profile(&next, moved, UM98X_LOCK_VERIFY_TOL_M));
    moved[2] += 51;
    TEST_ASSERT_FALSE(um98x_arp_near_profile(&next, moved, UM98X_LOCK_VERIFY_TOL_M));
    TEST_ASSERT_FALSE(um98x_arp_near_profile(&cur, arp, UM98X_LOCK_VERIFY_TOL_M));   /* survey profile */

    /* No usable point: nothing changes. */
    int64_t centre[3] = { 0, 0, 0 };
    memset(&pos, 0, sizeof(pos));
    TEST_ASSERT_FALSE(um98x_lock_profile(&cur, centre, 1, &next, &pos));
    TEST_ASSERT_EQUAL(UM98X_POS_NONE, pos.source);

    /* Re-survey: back to survey-in with the default time when none is kept. */
    um98x_base_profile_t again;
    TEST_ASSERT_TRUE(um98x_lock_profile(&cur, arp, 1, &next, &pos));
    next.survey_in_s = 0;   /* a stored fixed profile has no survey time */
    um98x_resurvey_profile(&next, &again);
    TEST_ASSERT_EQUAL(UM98X_BASE_SURVEY_IN, again.base_mode);
    TEST_ASSERT_EQUAL_UINT16(UM98X_DEFAULT_SURVEY_S, again.survey_in_s);
    TEST_ASSERT_TRUE(um98x_profile_valid(&again));
    TEST_ASSERT_EQUAL_UINT32(um98x_profile_hash(&cur), um98x_profile_hash(&again));

    /* Position record from a 1005 point. */
    um98x_base_position_t now;
    TEST_ASSERT_TRUE(um98x_arp_to_position(arp, &now));
    TEST_ASSERT_EQUAL_INT64(0, now.locked_unix);
    TEST_ASSERT_FALSE(um98x_arp_to_position(centre, &now));
}

/* ---- antenna descriptor and B1C/B2a ---------------------------------------------- */

#define PLAN_IGS_NAME "TRM59800.00     SCIS"

static void antenna_readback(um98x_readback_t *rb, const char *model, const char *serial, uint8_t id)
{
    rb->config.rx.antenna_known = true;
    snprintf(rb->config.rx.antenna_model, sizeof(rb->config.rx.antenna_model), "%s", model);
    snprintf(rb->config.rx.antenna_serial, sizeof(rb->config.rx.antenna_serial), "%s", serial);
    rb->config.rx.antenna_setup_id = id;
}

static int step_index(const um98x_plan_t *plan, um98x_step_kind_t kind)
{
    for (size_t i = 0; i < plan->count; i++) {
        if (plan->step[i].kind == kind) {
            return (int)i;
        }
    }
    return -1;
}

TEST_CASE("plan: the antenna descriptor only with a model that the receiver does not hold", "[um98x_plan]")
{
    um98x_base_profile_t p;
    um98x_readback_t rb;
    um98x_plan_t plan;
    char text[768];

    /* No model in the profile: never sent, whatever the receiver holds. */
    survey_profile(&p);
    matching_readback(&p, &rb);
    antenna_readback(&rb, "SOMETHING ELSE", "", 0);
    um98x_plan_build(&p, &rb, 115200, false, &plan);
    TEST_ASSERT_EQUAL_INT(-1, step_index(&plan, UM98X_STEP_ANTENNA));
    TEST_ASSERT_TRUE(um98x_antenna_matches(&p, &rb));

    /* A model the read-back does not show: sent after the base mode, before
     * the outputs; not fatal. */
    snprintf(p.antenna_model, sizeof(p.antenna_model), "%s", PLAN_IGS_NAME);
    snprintf(p.antenna_serial, sizeof(p.antenna_serial), "5000112233");
    p.antenna_setup_id = 3;
    rb.mode_ok = false;   /* base mode sent as well */
    um98x_plan_build(&p, &rb, 115200, false, &plan);
    plan_text(&p, &plan, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("MODE BASE 1234 TIME 600 0|"
                             "CONFIG BASEANTENNAMODEL \"TRM59800.00     SCIS\" \"5000112233\" 3 USER|"
                             "RTCM1005 COM1 1|RTCM1033 COM1 1|RTCM1077 COM1 1|RTCM1087 COM1 1|RTCM1097 COM1 1|"
                             "RTCM1117 COM1 1|RTCM1127 COM1 1|RTCM1137 COM1 1|RTCM1230 COM1 1|SAVECONFIG",
                             text);
    TEST_ASSERT_FALSE(plan.step[step_index(&plan, UM98X_STEP_ANTENNA)].fatal);

    /* The same descriptor (trailing spaces aside): not sent. */
    matching_readback(&p, &rb);
    antenna_readback(&rb, PLAN_IGS_NAME "  ", "5000112233 ", 3);
    um98x_plan_build(&p, &rb, 115200, false, &plan);
    TEST_ASSERT_EQUAL_INT(-1, step_index(&plan, UM98X_STEP_ANTENNA));

    /* As build 25164 stores it, runs of spaces collapsed (the bench's
     * read-back, CONFIG BASEANTENNAMODEL "TRM59800.00 SCIS" ...): the same
     * name, not sent again. */
    antenna_readback(&rb, "TRM59800.00 SCIS", "5000112233", 3);
    TEST_ASSERT_TRUE(um98x_antenna_matches(&p, &rb));
    um98x_plan_build(&p, &rb, 115200, false, &plan);
    TEST_ASSERT_EQUAL_INT(-1, step_index(&plan, UM98X_STEP_ANTENNA));

    /* Name, serial or setup ID different, or the read-back unknown: sent. */
    antenna_readback(&rb, "TRM59800.00     NONE", "5000112233", 3);
    um98x_plan_build(&p, &rb, 115200, false, &plan);
    TEST_ASSERT_EQUAL_INT(0, step_index(&plan, UM98X_STEP_ANTENNA));
    antenna_readback(&rb, PLAN_IGS_NAME, "5000112234", 3);
    um98x_plan_build(&p, &rb, 115200, false, &plan);
    TEST_ASSERT_EQUAL_INT(0, step_index(&plan, UM98X_STEP_ANTENNA));
    antenna_readback(&rb, PLAN_IGS_NAME, "5000112233", 4);
    um98x_plan_build(&p, &rb, 115200, false, &plan);
    TEST_ASSERT_EQUAL_INT(0, step_index(&plan, UM98X_STEP_ANTENNA));
    rb.config.rx.antenna_known = false;
    um98x_plan_build(&p, &rb, 115200, false, &plan);
    TEST_ASSERT_EQUAL_INT(0, step_index(&plan, UM98X_STEP_ANTENNA));
    rb.config_ok = false;
    um98x_plan_build(&p, &rb, 115200, false, &plan);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, step_index(&plan, UM98X_STEP_ANTENNA));

    /* The longest strings keep the command under 128 bytes. */
    memset(p.antenna_model, 'M', 31);
    p.antenna_model[31] = '\0';
    memset(p.antenna_serial, 'S', 31);
    p.antenna_serial[31] = '\0';
    p.antenna_setup_id = 255;
    um98x_step_t st = { UM98X_STEP_ANTENNA, false, 0, 0 };
    char cmd[UM98X_CMD_MAX];
    size_t n = um98x_step_command(&p, &st, cmd, sizeof(cmd));
    TEST_ASSERT_NOT_EQUAL(0, n);
    TEST_ASSERT_LESS_THAN_size_t(UM98X_CMD_MAX - 4u, n);   /* with CR LF before and after */
    TEST_ASSERT_TRUE(um98x_cmd_normalise(cmd, text, sizeof(text)));
}

TEST_CASE("plan: B1C/B2a in RTCM is turned on only when it reads back off", "[um98x_plan]")
{
    um98x_base_profile_t p;
    um98x_readback_t rb;
    um98x_plan_t plan;
    char text[768];
    survey_profile(&p);

    matching_readback(&p, &rb);
    rb.config.rx.b1cb2a = UM98X_SETTING_OFF;
    um98x_plan_build(&p, &rb, 115200, false, &plan);
    plan_text(&p, &plan, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("CONFIG RTCMB1CB2A ENABLE|RTCM1005 COM1 1|RTCM1033 COM1 1|RTCM1077 COM1 1|"
                             "RTCM1087 COM1 1|RTCM1097 COM1 1|RTCM1117 COM1 1|RTCM1127 COM1 1|RTCM1137 COM1 1|"
                             "RTCM1230 COM1 1|SAVECONFIG",
                             text);
    TEST_ASSERT_FALSE(plan.step[0].fatal);

    /* On or unknown: never sent. */
    rb.config.rx.b1cb2a = UM98X_SETTING_ON;
    um98x_plan_build(&p, &rb, 115200, false, &plan);
    TEST_ASSERT_EQUAL_INT(-1, step_index(&plan, UM98X_STEP_B1CB2A));
    rb.config.rx.b1cb2a = UM98X_SETTING_UNKNOWN;
    um98x_plan_build(&p, &rb, 115200, false, &plan);
    TEST_ASSERT_EQUAL_INT(-1, step_index(&plan, UM98X_STEP_B1CB2A));
    rb.config.rx.b1cb2a = UM98X_SETTING_OFF;
    rb.config_ok = false;   /* no dump at all */
    um98x_plan_build(&p, &rb, 115200, false, &plan);
    TEST_ASSERT_EQUAL_INT(-1, step_index(&plan, UM98X_STEP_B1CB2A));

    /* Order with everything: signal group, baud, base mode, antenna, B1C/B2a, outputs, save. */
    snprintf(p.antenna_model, sizeof(p.antenna_model), "%s", PLAN_IGS_NAME);
    p.com1_baud = 460800;
    matching_readback(&p, &rb);
    rb.config.signal_group_master = 4;
    rb.mode_ok = false;
    rb.config.rx.b1cb2a = UM98X_SETTING_OFF;
    um98x_plan_build(&p, &rb, 115200, false, &plan);
    TEST_ASSERT_EQUAL_INT(0, step_index(&plan, UM98X_STEP_SIGNALGROUP));
    TEST_ASSERT_EQUAL_INT(1, step_index(&plan, UM98X_STEP_BAUD));
    TEST_ASSERT_EQUAL_INT(2, step_index(&plan, UM98X_STEP_MODE));
    TEST_ASSERT_EQUAL_INT(3, step_index(&plan, UM98X_STEP_ANTENNA));
    TEST_ASSERT_EQUAL_INT(4, step_index(&plan, UM98X_STEP_B1CB2A));
    TEST_ASSERT_EQUAL_INT(5, step_index(&plan, UM98X_STEP_RTCM));

    /* Neither belongs to the RTCM-only restore. */
    TEST_ASSERT_TRUE(um98x_plan_rtcm_only(&p, &plan));
    TEST_ASSERT_EQUAL_INT(-1, step_index(&plan, UM98X_STEP_B1CB2A));
    TEST_ASSERT_EQUAL_INT(-1, step_index(&plan, UM98X_STEP_ANTENNA));
}

TEST_CASE("plan: satellite-in-view output for the live view, never saved", "[um98x_plan]")
{
    char cmd[UM98X_CMD_MAX];
    TEST_ASSERT_NOT_EQUAL(0, um98x_sat_view_command(true, 5, cmd, sizeof(cmd)));
    TEST_ASSERT_EQUAL_STRING("GPGSV COM1 5", cmd);
    TEST_ASSERT_NOT_EQUAL(0, um98x_sat_view_command(false, 0, cmd, sizeof(cmd)));
    TEST_ASSERT_EQUAL_STRING("UNLOG COM1 GPGSV", cmd);
    TEST_ASSERT_EQUAL_size_t(0, um98x_sat_view_command(true, 0, cmd, sizeof(cmd)));
    TEST_ASSERT_EQUAL_size_t(0, um98x_sat_view_command(true, UM98X_SAT_VIEW_PERIOD_MAX + 1, cmd, sizeof(cmd)));
    TEST_ASSERT_EQUAL_size_t(0, um98x_sat_view_command(true, 5, cmd, 4));
}

/* ---- apply preview ------------------------------------------------------------------- */

static um98x_preview_step_t s_prev[UM98X_PREVIEW_MAX];

/* "K:W:S text" per step (K q/w, W a/d/o for always/if differs/if off, S s/-), joined with '|'. */
static void preview_text(const um98x_preview_step_t *s, size_t n, char *out, size_t cap)
{
    static const char kinds[] = { 'q', 'w' };
    static const char whens[] = { 'a', 'd', 'o' };
    out[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        size_t len = strlen(out);
        TEST_ASSERT_LESS_THAN(sizeof(kinds), s[i].kind);
        TEST_ASSERT_LESS_THAN(sizeof(whens), s[i].when);
        snprintf(out + len, cap - len, "%s%c%c%c %s", len ? "|" : "", kinds[s[i].kind], whens[s[i].when],
                 s[i].saved ? 's' : '-', s[i].text);
    }
}

TEST_CASE("plan preview: the default profile's exact list", "[um98x_plan]")
{
    um98x_base_profile_t p;
    char text[2048];
    survey_profile(&p);
    size_t n = um98x_plan_preview(&p, 115200, s_prev, UM98X_PREVIEW_MAX);
    preview_text(s_prev, n, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("wa- UNLOG COM1|qa- VERSIONA|qa- CONFIG|qa- MODE|"
                             "wds CONFIG SIGNALGROUP 3 6|qa- ANT1GROUPA|wd- MODE BASE 1234 TIME 600 0|"
                             "wo- CONFIG RTCMB1CB2A ENABLE|"
                             "wa- RTCM1005 COM1 1|wa- RTCM1033 COM1 1|wa- RTCM1077 COM1 1|wa- RTCM1087 COM1 1|"
                             "wa- RTCM1097 COM1 1|wa- RTCM1117 COM1 1|wa- RTCM1127 COM1 1|wa- RTCM1137 COM1 1|"
                             "wa- RTCM1230 COM1 1|"
                             "was SAVECONFIG",
                             text);
    TEST_ASSERT_EQUAL_size_t(18, n);

    /* Not for an invalid profile; a short buffer never holds a cut list's SAVECONFIG out of place. */
    p.survey_in_s = 0;
    TEST_ASSERT_EQUAL_size_t(0, um98x_plan_preview(&p, 115200, s_prev, UM98X_PREVIEW_MAX));
    survey_profile(&p);
    TEST_ASSERT_EQUAL_size_t(0, um98x_plan_preview(NULL, 115200, s_prev, UM98X_PREVIEW_MAX));
    TEST_ASSERT_EQUAL_size_t(3, um98x_plan_preview(&p, 115200, s_prev, 3));
    TEST_ASSERT_EQUAL_STRING("CONFIG", s_prev[2].text);
}

TEST_CASE("plan preview: fixed position, antenna model, satellite view, baud change", "[um98x_plan]")
{
    um98x_base_profile_t p;
    char text[2048];
    fixed_profile(&p);
    p.com1_baud = 460800;
    p.sat_view_output = true;
    snprintf(p.antenna_model, sizeof(p.antenna_model), "TRM59800.00     SCIS");
    snprintf(p.antenna_serial, sizeof(p.antenna_serial), "5000123");
    p.antenna_setup_id = 2;
    p.rtcm_count = 2;
    size_t n = um98x_plan_preview(&p, 115200, s_prev, UM98X_PREVIEW_MAX);
    char mode[UM98X_MODE_CMD_MAX];
    TEST_ASSERT_NOT_EQUAL(0, um98x_mode_command(&p, mode, sizeof(mode)));
    char want[2048];
    snprintf(want, sizeof(want),
             "wa- UNLOG COM1|qa- VERSIONA|qa- CONFIG|qa- MODE|wds CONFIG SIGNALGROUP 3 6|qa- ANT1GROUPA|"
             "wd- CONFIG COM1 460800|"
             "wd- %s|wd- CONFIG BASEANTENNAMODEL \"TRM59800.00     SCIS\" \"5000123\" 2 USER|qd- CONFIG|"
             "wo- CONFIG RTCMB1CB2A ENABLE|wa- RTCM1005 COM1 1|wa- RTCM1033 COM1 1|wa- GPGSV COM1 10|was SAVECONFIG",
             mode);
    preview_text(s_prev, n, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING(want, text);
    TEST_ASSERT_NOT_NULL(strstr(mode, "MODE BASE 1234 "));

    /* The link already at the profile's baud: no baud step. */
    n = um98x_plan_preview(&p, 460800, s_prev, UM98X_PREVIEW_MAX);
    preview_text(s_prev, n, text, sizeof(text));
    TEST_ASSERT_NULL(strstr(text, "CONFIG COM1"));
}

TEST_CASE("plan preview: every write is what the plan sends when everything differs", "[um98x_plan]")
{
    um98x_base_profile_t p;
    fixed_profile(&p);
    p.com1_baud = 460800;
    p.sat_view_output = true;
    snprintf(p.antenna_model, sizeof(p.antenna_model), "TRM59800.00     SCIS");
    snprintf(p.antenna_serial, sizeof(p.antenna_serial), "5000123");

    /* A read-back that differs in everything: another signal group, no base mode, another antenna, B1C/B2a off. */
    um98x_readback_t rb;
    memset(&rb, 0, sizeof(rb));
    rb.config_ok = true;
    rb.config.signal_group_known = true;
    rb.config.signal_group_master = 4;
    rb.config.signal_group_slave = 5;
    rb.config.rx.antenna_known = true;
    snprintf(rb.config.rx.antenna_model, sizeof(rb.config.rx.antenna_model), "ADVNULLANTENNA");
    rb.config.rx.b1cb2a = UM98X_SETTING_OFF;
    rb.mode_ok = false;
    um98x_plan_t plan;
    TEST_ASSERT_TRUE(um98x_plan_build(&p, &rb, 115200, false, &plan));

    size_t n = um98x_plan_preview(&p, 115200, s_prev, UM98X_PREVIEW_MAX);
    size_t writes = 0;
    size_t k = 0;   /* the plan's step matched next */
    for (size_t i = 0; i < n; i++) {
        if (s_prev[i].kind != UM98X_PREVIEW_WRITE) {
            continue;
        }
        writes++;
        if (strcmp(s_prev[i].text, "UNLOG COM1") == 0) {
            continue;   /* the session's quiet port, before the plan */
        }
        TEST_ASSERT_LESS_THAN(plan.count, k);
        char cmd[UM98X_CMD_MAX];
        TEST_ASSERT_NOT_EQUAL(0, um98x_step_command(&p, &plan.step[k], cmd, sizeof(cmd)));
        TEST_ASSERT_EQUAL_STRING(cmd, s_prev[i].text);
        k++;
    }
    TEST_ASSERT_EQUAL_size_t(plan.count, k);   /* nothing the plan sends is missing from the preview */
    TEST_ASSERT_EQUAL_size_t(plan.count + 1, writes);
    TEST_ASSERT_TRUE(s_prev[n - 1].saved);
    TEST_ASSERT_EQUAL_STRING("SAVECONFIG", s_prev[n - 1].text);
}
