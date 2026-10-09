/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <string.h>

#include "semver.h"
#include "unity.h"

static int cmp(const char *a, const char *b)
{
    semver_t va;
    semver_t vb;
    TEST_ASSERT_TRUE_MESSAGE(semver_parse(a, &va), a);
    TEST_ASSERT_TRUE_MESSAGE(semver_parse(b, &vb), b);
    return semver_compare(&va, &vb);
}

TEST_CASE("semver accepts release versions", "[semver]")
{
    semver_t v;
    TEST_ASSERT_TRUE(semver_parse("v1.2.3", &v));
    TEST_ASSERT_EQUAL_UINT32(1, v.major);
    TEST_ASSERT_EQUAL_UINT32(2, v.minor);
    TEST_ASSERT_EQUAL_UINT32(3, v.patch);
    TEST_ASSERT_EQUAL_STRING("", v.pre);
    TEST_ASSERT_TRUE(semver_parse("0.10.0", &v));
    TEST_ASSERT_TRUE(semver_parse("v2.0.0-rc.1", &v));
    TEST_ASSERT_EQUAL_STRING("rc.1", v.pre);
    TEST_ASSERT_TRUE(semver_parse("v1.0.0-x-y.0", &v));
}

TEST_CASE("semver rejects dev builds and malformed strings", "[semver]")
{
    static const char *const bad[] = {
        "47fc47b-dirty+dev", "v0.3.0-4-g47fc47b-dirty+dev", "v0.3.0+dev", "unknown+dev",
        /* long describe output as the build clamps it: 27 characters + "+dev" = 31 */
        "v0.10.0-rc.1-123-g47fc47b-d+dev", "v1.0.0-beta.1-12-g47fc47b-d+dev",
        "", "v", "v1", "v1.2", "1.2.3.4", "v01.2.3",
        "v1.02.3", "v1.2.3-", "v1.2.3-rc..1", "v1.2.3-rc.01", "v1.2.3+build", "V1.2.3", "v1.2.3 ",
        "v4294967296.0.0", "0.0.0-unknown_", NULL,
    };
    semver_t v;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(semver_parse(bad[i], &v), bad[i] ? bad[i] : "(null)");
    }
}

TEST_CASE("semver orders like Semantic Versioning 2.0.0", "[semver]")
{
    static const char *const ordered[] = {
        "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2",
        "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "1.0.1", "1.1.0", "1.10.0", "2.0.0",
    };
    const size_t n = sizeof(ordered) / sizeof(ordered[0]);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_INT(0, cmp(ordered[i], ordered[i]));
        for (size_t j = i + 1; j < n; j++) {
            TEST_ASSERT_TRUE_MESSAGE(cmp(ordered[i], ordered[j]) < 0, ordered[i]);
            TEST_ASSERT_TRUE_MESSAGE(cmp(ordered[j], ordered[i]) > 0, ordered[j]);
        }
    }
    TEST_ASSERT_EQUAL_INT(0, cmp("v1.2.3", "1.2.3"));
}

TEST_CASE("semver: why dev names must keep +dev within 31 characters", "[semver]")
{
    /* esp_app_desc_t holds 31 characters. Had the build appended "+dev" to a
     * long describe string, IDF would cut it to this, which parses as a
     * pre-release ranked above the real rc.1. The build therefore shortens
     * the describe part so "+dev" always survives. */
    const char *cut = "v0.10.0-rc.1-123-g47fc47b-dirty";
    semver_t v;
    TEST_ASSERT_EQUAL_size_t(31, strlen(cut));
    TEST_ASSERT_TRUE(semver_parse(cut, &v));
    TEST_ASSERT_TRUE(cmp(cut, "v0.10.0-rc.1") > 0);
    TEST_ASSERT_FALSE(semver_parse("v0.10.0-rc.1-123-g47fc47b-d+dev", &v));
}
