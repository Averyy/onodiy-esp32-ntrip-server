/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Where releases come from: the repository name, the URLs built from it and
 * the hosts a transfer may connect to (fw_update release_url.c).
 */
#include <stdio.h>
#include <string.h>

#include "release_url.h"
#include "unity.h"

TEST_CASE("release hosts: GitHub and its release asset CDN only", "[release_url]")
{
    static const char *const good[] = {
        "https://github.com/Averyy/onocoydiy-esp-um982/releases/latest/download/manifest.json",
        /* as the HTTP client prints it */
        "https://github.com:443/Averyy/onocoydiy-esp-um982/releases/download/v1.0.0/fw.bin",
        "https://GitHub.COM/a/b",
        "HTTPS://github.com/a/b",
        "https://release-assets.githubusercontent.com/github-production-release-asset/1/2?sp=r&sig=x",
        "https://release-assets.githubusercontent.com:443/github-production-release-asset/1/2",
        "https://objects.githubusercontent.com/github-production-release-asset-2e65be/1/2?X-Amz-Signature=x",
        "https://github.com",
        "https://github.com?x=1",
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        TEST_ASSERT_TRUE_MESSAGE(fw_release_url_ok(good[i]), good[i]);
    }
    static const char *const bad[] = {
        NULL,
        "",
        "http://github.com/a/b",                              /* not https */
        "ftp://github.com/a/b",
        "github.com/a/b",
        "https://evil.example/github.com/a/b",
        "https://github.com.evil.example/a/b",                 /* not a suffix match */
        "https://evilgithub.com/a/b",
        "https://www.github.com/a/b",
        "https://api.github.com/repos/a/b/releases",
        "https://raw.githubusercontent.com/a/b/main/fw.bin",  /* repository content, not releases */
        "https://githubusercontent.com/a",
        "https://user@github.com/a/b",                        /* user info */
        "https://github.com@evil.example/a/b",
        "https://github.com:8443/a/b",                        /* another port */
        "https://github.com:443x/a/b",
        "https://github.com:/a/b",
        "https://github.com.:443/a/b",
        "https://[::1]/a/b",
        "https://github.com\\@evil.example/",
        "https:///github.com/a",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(fw_release_url_ok(bad[i]), bad[i] != NULL ? bad[i] : "(null)");
    }
}

TEST_CASE("release hosts: exact names, case-insensitive", "[release_url]")
{
    TEST_ASSERT_EQUAL_STRING("github.com", fw_release_host(0));
    TEST_ASSERT_NULL(fw_release_host(FW_RELEASE_HOST_COUNT));
    for (size_t i = 0; i < FW_RELEASE_HOST_COUNT; i++) {
        const char *h = fw_release_host(i);
        TEST_ASSERT_NOT_NULL(h);
        TEST_ASSERT_TRUE(fw_release_host_ok(h, strlen(h)));
        TEST_ASSERT_FALSE(fw_release_host_ok(h, strlen(h) - 1));   /* a prefix is not the host */
    }
    TEST_ASSERT_TRUE(fw_release_host_ok("GITHUB.com", 10));
    TEST_ASSERT_TRUE(fw_release_host_ok("github.com:443", 10));   /* the length bounds it */
    TEST_ASSERT_FALSE(fw_release_host_ok("github.co", 9));
    TEST_ASSERT_FALSE(fw_release_host_ok(NULL, 0));
    TEST_ASSERT_FALSE(fw_release_host_ok("", 0));

    /* A host too long for the client's URL buffer is cut: still refused. */
    char url[160];
    memset(url, 'a', sizeof(url));
    memcpy(url, "https://github.com", 18);
    url[sizeof(url) - 1] = '\0';
    TEST_ASSERT_FALSE(fw_release_url_ok(url));
}

TEST_CASE("release repository names", "[release_url]")
{
    static const char *const good[] = {
        "Averyy/onocoydiy-esp-um982", "a/b", "a-b/c.d_e", "x/.github", "x/a..b",
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        TEST_ASSERT_TRUE_MESSAGE(fw_release_repo_ok(good[i]), good[i]);
    }
    static const char *const bad[] = {
        NULL, "", "a", "/b", "a/", "a/b/c", "a b/c", "a/b c", "a_b/c", "a.b/c", "a/.", "a/..", "../b",
        "a/b?x", "a/b#x", "a/b%2F",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(fw_release_repo_ok(bad[i]), bad[i] != NULL ? bad[i] : "(null)");
    }
    char owner[48];
    memset(owner, 'o', sizeof(owner));
    memcpy(&owner[39], "/r", 3);   /* an owner of 39 */
    TEST_ASSERT_TRUE(fw_release_repo_ok(owner));
    memset(owner, 'o', sizeof(owner));
    memcpy(&owner[40], "/r", 3);   /* 40 */
    TEST_ASSERT_FALSE(fw_release_repo_ok(owner));

    char name[120];
    memset(name, 'r', sizeof(name));
    memcpy(name, "o/", 2);
    name[102] = '\0';   /* a name of 100 */
    TEST_ASSERT_TRUE(fw_release_repo_ok(name));
    name[102] = 'r';
    name[103] = '\0';   /* 101 */
    TEST_ASSERT_FALSE(fw_release_repo_ok(name));
}

TEST_CASE("release URLs are built from valid parts only, on github.com", "[release_url]")
{
    char url[256];
    TEST_ASSERT_TRUE(fw_release_latest_url(url, sizeof(url), "Averyy/onocoydiy-esp-um982", "manifest.json"));
    TEST_ASSERT_EQUAL_STRING("https://github.com/Averyy/onocoydiy-esp-um982/releases/latest/download/manifest.json",
                             url);
    TEST_ASSERT_TRUE(fw_release_url_ok(url));
    TEST_ASSERT_TRUE(fw_release_asset_url(url, sizeof(url), "a/b", "v1.2.3-rc.1", "onocoydiy-esp-um982.bin"));
    TEST_ASSERT_EQUAL_STRING("https://github.com/a/b/releases/download/v1.2.3-rc.1/onocoydiy-esp-um982.bin", url);
    TEST_ASSERT_TRUE(fw_release_url_ok(url));

    TEST_ASSERT_FALSE(fw_release_latest_url(url, sizeof(url), "a/b/c", "manifest.json"));
    TEST_ASSERT_EQUAL_STRING("", url);
    TEST_ASSERT_FALSE(fw_release_latest_url(url, sizeof(url), "a/b", "../manifest.json"));
    TEST_ASSERT_FALSE(fw_release_asset_url(url, sizeof(url), "a/b", "../v1", "fw.bin"));
    TEST_ASSERT_FALSE(fw_release_asset_url(url, sizeof(url), "a/b", "v1.0.0", "x/fw.bin"));
    TEST_ASSERT_FALSE(fw_release_asset_url(url, sizeof(url), "a/b", "", "fw.bin"));
    TEST_ASSERT_FALSE(fw_release_asset_url(url, sizeof(url), NULL, "v1.0.0", "fw.bin"));
    /* Does not fit. */
    char small[40];
    TEST_ASSERT_FALSE(fw_release_asset_url(small, sizeof(small), "a/b", "v1.0.0", "fw.bin"));
    TEST_ASSERT_FALSE(fw_release_latest_url(url, 0, "a/b", "m.json"));
}
