/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */
#include <string.h>
#include "report_core.h"
#include "unity.h"

static onocoy_report_day_t days[ONOCOY_REPORT_DAYS];

TEST_CASE("Onocoy report: explicit opt-in and real enabled station credentials only", "[onocoy_report]")
{
    ntrip_caster_cfg_t c;
    ntrip_server_onocoy_defaults(&c);
    c.enabled = true;
    strcpy(c.mountpoint, "station");
    strcpy(c.username, "station-user");
    strcpy(c.password, "station-password");
    TEST_ASSERT_FALSE(c.onocoy_stats);
    TEST_ASSERT_FALSE(report_eligible(&c));
    c.onocoy_stats = true;
    TEST_ASSERT_TRUE(report_eligible(&c));
    c.enabled = false;
    TEST_ASSERT_FALSE(report_eligible(&c));
    c.enabled = true;
    c.password[0] = 0;
    TEST_ASSERT_FALSE(report_eligible(&c));
    strcpy(c.password, "station-password");
    c.test_onocoy = true;
    TEST_ASSERT_FALSE(report_eligible(&c));
    c.test_onocoy = false;
    strcpy(c.username, "user&another=value");
    TEST_ASSERT_FALSE(report_eligible(&c));
    strcpy(c.username, "station-user");
    strcpy(c.host, "another.example");
    TEST_ASSERT_FALSE(report_eligible(&c));
}

TEST_CASE("Onocoy report: completed dates sorted, zero different from unknown", "[onocoy_report]")
{
    const char *j = "[{\"date\":\"2026-10-24\",\"up_time_seconds\":0,\"rewards\":0},"
        "{\"date\":\"2026-10-26\",\"up_time_seconds\":12},"
        "{\"date\":\"2026-10-25T00:00:00Z\",\"up_time_seconds\":86390,\"latency_avg\":0.12,\"latency_max\":0.4,\"rewards\":1.25}]";
    size_t n = 0;
    TEST_ASSERT_TRUE(report_parse(j, strlen(j), "2026-10-01", "2026-10-26", days, &n));
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_STRING("2026-10-25", days[0].date);
    TEST_ASSERT_EQUAL_UINT32(86390, days[0].uptime_s);
    TEST_ASSERT_EQUAL(15, days[0].known);
    TEST_ASSERT_DOUBLE_WITHIN(0.000001, 0.12, days[0].latency_avg);
    TEST_ASSERT_EQUAL(ONOCOY_REPORT_UPTIME | ONOCOY_REPORT_REWARDS, days[1].known);
    TEST_ASSERT_EQUAL_UINT32(0, days[1].uptime_s);
    j = "[{\"date\":\"2026-10-25\",\"up_time_seconds\":null,\"rewards\":null}]";
    TEST_ASSERT_TRUE(report_parse(j, strlen(j), "2026-10-01", "2026-10-26", days, &n));
    TEST_ASSERT_EQUAL(0, days[0].known);
}

TEST_CASE("Onocoy report: malformed, impossible, duplicate and trailing data refused", "[onocoy_report]")
{
    const char *bad[] = {"{}", "[] junk", "[{\"date\":\"2026-02-30\"}]",
        "[{\"date\":\"2026-10-25\",\"up_time_seconds\":86401}]",
        "[{\"date\":\"2026-10-25\",\"up_time_seconds\":1.5}]",
        "[{\"date\":\"2026-10-25\",\"latency_avg\":-1}]",
        "[{\"date\":\"2026-10-25\",\"rewards\":\"1\"}]",
        "[{\"date\":\"2026-10-25\"},{\"date\":\"2026-10-25\"}]"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        size_t n = 99;
        TEST_ASSERT_FALSE_MESSAGE(report_parse(bad[i], strlen(bad[i]), "2026-01-01", "2026-10-26", days, &n), bad[i]);
        TEST_ASSERT_EQUAL_size_t(0, n);
    }
}

TEST_CASE("Onocoy report: successful checks daily, auth refusals never loop", "[onocoy_report]")
{
    TEST_ASSERT_EQUAL_UINT32(86400, report_retry_s(true, 200, 0));
    const int rejected[] = {400, 401, 403, 404, 408, 409, 422, 429};
    for (size_t i = 0; i < sizeof(rejected)/sizeof(rejected[0]); i++) {
        TEST_ASSERT_EQUAL_UINT32(86400, report_retry_s(false, rejected[i], 0));
        TEST_ASSERT_EQUAL_UINT32(86400, report_retry_s(false, rejected[i], 100));
    }
    TEST_ASSERT_EQUAL_UINT32(900, report_retry_s(false, 500, 0));
    TEST_ASSERT_EQUAL_UINT32(3600, report_retry_s(false, 503, 1));
    TEST_ASSERT_EQUAL_UINT32(86400, report_retry_s(false, 0, 100));
}

TEST_CASE("Onocoy report: manual refresh only after a failure", "[onocoy_report]")
{
    TEST_ASSERT_TRUE(report_manual_allowed(ONOCOY_REPORT_AUTH));
    TEST_ASSERT_TRUE(report_manual_allowed(ONOCOY_REPORT_ERROR));
    TEST_ASSERT_FALSE(report_manual_allowed(ONOCOY_REPORT_OK));
    TEST_ASSERT_FALSE(report_manual_allowed(ONOCOY_REPORT_EMPTY));
    TEST_ASSERT_FALSE(report_manual_allowed(ONOCOY_REPORT_PENDING));
    TEST_ASSERT_FALSE(report_manual_allowed(ONOCOY_REPORT_WAIT_NETWORK));
    TEST_ASSERT_FALSE(report_manual_allowed(ONOCOY_REPORT_WAIT_TIME));
    TEST_ASSERT_FALSE(report_manual_allowed(ONOCOY_REPORT_OFF));
}
