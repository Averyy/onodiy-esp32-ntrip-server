/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Web bodies of the receiver's first-apply hold: the apply preview
 * (GET /api/gnss/apply/preview) and first_apply_held in GET /api/gnss and
 * GET /api/status.
 */
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "um98x_plan.h"
#include "um98x_profile.h"
#include "unity.h"
#include "web_build.h"
#include "web_enums.h"

static char s_out[16384];
static um98x_preview_step_t s_steps[UM98X_PREVIEW_MAX];

static const cJSON *at(const cJSON *o, const char *k)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    TEST_ASSERT_NOT_NULL_MESSAGE(it, k);
    return it;
}

static cJSON *parse_out(void)
{
    cJSON *root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    return root;
}

TEST_CASE("receiver web: preview enum strings", "[web_receiver]")
{
    TEST_ASSERT_EQUAL_STRING("query", web_preview_kind_name(UM98X_PREVIEW_QUERY));
    TEST_ASSERT_EQUAL_STRING("write", web_preview_kind_name(UM98X_PREVIEW_WRITE));
    TEST_ASSERT_EQUAL_STRING("unknown", web_preview_kind_name(9));
    TEST_ASSERT_EQUAL_STRING("always", web_preview_when_name(UM98X_PREVIEW_ALWAYS));
    TEST_ASSERT_EQUAL_STRING("if_differs", web_preview_when_name(UM98X_PREVIEW_IF_DIFFERS));
    TEST_ASSERT_EQUAL_STRING("if_off", web_preview_when_name(UM98X_PREVIEW_IF_OFF));
    TEST_ASSERT_EQUAL_STRING("unknown", web_preview_when_name(9));
}

TEST_CASE("receiver web: GET /api/gnss/apply/preview lists the steps and ends with SAVECONFIG", "[web_receiver]")
{
    um98x_base_profile_t p;
    um98x_profile_default(&p, 1234);
    size_t n = um98x_plan_preview(&p, 115200, s_steps, UM98X_PREVIEW_MAX);
    TEST_ASSERT_GREATER_THAN(4, n);
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_apply_preview(&w, s_steps, n);
    TEST_ASSERT_TRUE(wj_ok(&w));
    cJSON *root = parse_out();
    const cJSON *steps = at(root, "steps");
    TEST_ASSERT_TRUE(cJSON_IsArray(steps));
    TEST_ASSERT_EQUAL(n, cJSON_GetArraySize(steps));
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(root, "saveconfig")));

    const cJSON *first = cJSON_GetArrayItem(steps, 0);
    TEST_ASSERT_EQUAL_STRING("UNLOG COM1", at(first, "text")->valuestring);
    TEST_ASSERT_EQUAL_STRING("write", at(first, "kind")->valuestring);
    TEST_ASSERT_EQUAL_STRING("always", at(first, "when")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(first, "saved")));
    const cJSON *q = cJSON_GetArrayItem(steps, 2);
    TEST_ASSERT_EQUAL_STRING("CONFIG", at(q, "text")->valuestring);
    TEST_ASSERT_EQUAL_STRING("query", at(q, "kind")->valuestring);
    const cJSON *sg = cJSON_GetArrayItem(steps, 4);
    TEST_ASSERT_EQUAL_STRING("CONFIG SIGNALGROUP 3 6", at(sg, "text")->valuestring);
    TEST_ASSERT_EQUAL_STRING("if_differs", at(sg, "when")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(sg, "saved")));
    bool if_off = false;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, steps)
    {
        TEST_ASSERT_EQUAL(4, cJSON_GetArraySize(it));   /* text, kind, when, saved: nothing else */
        if (strcmp(at(it, "when")->valuestring, "if_off") == 0) {
            if_off = true;
            TEST_ASSERT_EQUAL_STRING("CONFIG RTCMB1CB2A ENABLE", at(it, "text")->valuestring);
        }
    }
    TEST_ASSERT_TRUE(if_off);
    const cJSON *last = cJSON_GetArrayItem(steps, (int)n - 1);
    TEST_ASSERT_EQUAL_STRING("SAVECONFIG", at(last, "text")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(last, "saved")));
    cJSON_Delete(root);

    /* Text that needs JSON escaping stays one string. */
    snprintf(p.antenna_model, sizeof(p.antenna_model), "TRM59800.00     SCIS");
    n = um98x_plan_preview(&p, 115200, s_steps, UM98X_PREVIEW_MAX);
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_apply_preview(&w, s_steps, n);
    TEST_ASSERT_TRUE(wj_ok(&w));
    root = parse_out();
    bool found = false;
    cJSON_ArrayForEach(it, at(root, "steps"))
    {
        if (strstr(at(it, "text")->valuestring, "BASEANTENNAMODEL") != NULL) {
            found = true;
            TEST_ASSERT_EQUAL_STRING("CONFIG BASEANTENNAMODEL \"TRM59800.00     SCIS\" \"\" 0 USER",
                                     at(it, "text")->valuestring);
        }
    }
    TEST_ASSERT_TRUE(found);
    cJSON_Delete(root);

    /* An empty list says so. */
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_apply_preview(&w, s_steps, 0);
    root = parse_out();
    TEST_ASSERT_EQUAL(0, cJSON_GetArraySize(at(root, "steps")));
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(root, "saveconfig")));
    cJSON_Delete(root);
}

static station_status_t s_st;

TEST_CASE("receiver web: first_apply_held in GET /api/gnss", "[web_receiver]")
{
    memset(&s_st, 0, sizeof(s_st));
    s_st.state = STATION_STATE_STREAMING;
    web_gnss_snap_t g;
    memset(&g, 0, sizeof(g));
    g.st = &s_st;
    g.link.mode = GNSS_LINK_MODE_STREAM;
    g.link.last_rx_age_ms = UINT32_MAX;
    g.link.last_rtcm_age_ms = UINT32_MAX;
    um98x_profile_default(&g.profile, 1234);
    g.autocfg = false;
    g.recovery = HEALTH_RECOVERY_ALERTS;
    for (int held = 0; held < 2; held++) {
        s_st.first_apply_held = held != 0;
        wj_t w;
        wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
        web_build_gnss(&w, &g);
        TEST_ASSERT_TRUE(wj_ok(&w));
        cJSON *root = parse_out();
        TEST_ASSERT_EQUAL(held != 0, cJSON_IsTrue(at(root, "first_apply_held")));
        TEST_ASSERT_TRUE(cJSON_IsBool(at(root, "first_apply_held")));
        TEST_ASSERT_TRUE(cJSON_IsFalse(at(root, "autocfg")));
        TEST_ASSERT_EQUAL_STRING("alerts_only", at(root, "recovery")->valuestring);
        cJSON_Delete(root);
    }
}

static web_status_snap_t s_snap;

TEST_CASE("receiver web: first_apply_held in the status receiver section, admin form only", "[web_receiver]")
{
    memset(&s_snap, 0, sizeof(s_snap));
    strcpy(s_snap.hostname, "onocoydiy");
    strcpy(s_snap.version, "v0.3.0");
    s_snap.settings_available = true;
    s_snap.st.state = STATION_STATE_STREAMING;
    s_snap.st.first_apply_held = true;
    s_snap.st.next_detect_in_s = UINT32_MAX;
    s_snap.rtcm_age_ms = UINT32_MAX;
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_status(&w, &s_snap, false);
    TEST_ASSERT_TRUE(wj_ok(&w));
    cJSON *root = parse_out();
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(at(root, "receiver"), "first_apply_held")));
    cJSON_Delete(root);

    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_status(&w, &s_snap, true);
    TEST_ASSERT_TRUE(wj_ok(&w));
    root = parse_out();
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(at(root, "receiver"), "first_apply_held"));
    cJSON_Delete(root);
}
