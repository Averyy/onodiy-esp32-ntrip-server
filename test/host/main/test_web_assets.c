/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>

#include "unity.h"
#include "web_assets.h"

static const uint8_t s_body[] = { 0x1f, 0x8b };

static const web_asset_t s_assets[] = {
    { "/index.html", "text/html; charset=utf-8", s_body, sizeof(s_body) },
    { "/app.js", "text/javascript; charset=utf-8", s_body, sizeof(s_body) },
    { "/app.css", "text/css; charset=utf-8", s_body, sizeof(s_body) },
    { "/favicon.svg", "image/svg+xml", s_body, sizeof(s_body) },
};
#define N_ASSETS (sizeof(s_assets) / sizeof(s_assets[0]))

TEST_CASE("web assets: the root and /index.html serve the page", "[web_assets]")
{
    TEST_ASSERT_EQUAL_PTR(&s_assets[0], web_asset_find(s_assets, N_ASSETS, "/"));
    TEST_ASSERT_EQUAL_PTR(&s_assets[0], web_asset_find(s_assets, N_ASSETS, "/index.html"));
    TEST_ASSERT_EQUAL_PTR(&s_assets[0], web_asset_find(s_assets, N_ASSETS, "/?from=portal"));
    TEST_ASSERT_EQUAL_PTR(&s_assets[0], web_asset_find(s_assets, N_ASSETS, "/#/casters"));
}

TEST_CASE("web assets: fixed names match exactly, query ignored", "[web_assets]")
{
    TEST_ASSERT_EQUAL_PTR(&s_assets[1], web_asset_find(s_assets, N_ASSETS, "/app.js"));
    TEST_ASSERT_EQUAL_PTR(&s_assets[1], web_asset_find(s_assets, N_ASSETS, "/app.js?v=2"));
    TEST_ASSERT_EQUAL_PTR(&s_assets[2], web_asset_find(s_assets, N_ASSETS, "/app.css"));
    TEST_ASSERT_EQUAL_PTR(&s_assets[3], web_asset_find(s_assets, N_ASSETS, "/favicon.svg"));

    TEST_ASSERT_NULL(web_asset_find(s_assets, N_ASSETS, "/APP.JS"));
    TEST_ASSERT_NULL(web_asset_find(s_assets, N_ASSETS, "/app.jsx"));
    TEST_ASSERT_NULL(web_asset_find(s_assets, N_ASSETS, "/app.j"));
    TEST_ASSERT_NULL(web_asset_find(s_assets, N_ASSETS, "/ui/app.js"));
    TEST_ASSERT_NULL(web_asset_find(s_assets, N_ASSETS, "//app.js"));
    TEST_ASSERT_NULL(web_asset_find(s_assets, N_ASSETS, "/api/status"));
    TEST_ASSERT_NULL(web_asset_find(s_assets, N_ASSETS, "/generate_204"));
    TEST_ASSERT_NULL(web_asset_find(s_assets, N_ASSETS, "/favicon.ico"));
}

TEST_CASE("web assets: bad input finds nothing", "[web_assets]")
{
    TEST_ASSERT_NULL(web_asset_find(s_assets, N_ASSETS, NULL));
    TEST_ASSERT_NULL(web_asset_find(s_assets, N_ASSETS, ""));
    TEST_ASSERT_NULL(web_asset_find(s_assets, N_ASSETS, "app.js"));
    TEST_ASSERT_NULL(web_asset_find(s_assets, N_ASSETS, "?x"));
    TEST_ASSERT_NULL(web_asset_find(NULL, N_ASSETS, "/app.js"));
    TEST_ASSERT_NULL(web_asset_find(s_assets, 0, "/"));
}

TEST_CASE("web etag: exact, weak and listed tags match", "[web_assets]")
{
    const char *etag = "\"v0.3.0\"";
    TEST_ASSERT_TRUE(web_etag_match("\"v0.3.0\"", etag));
    TEST_ASSERT_TRUE(web_etag_match("W/\"v0.3.0\"", etag));
    TEST_ASSERT_TRUE(web_etag_match("\"v0.3.0\"", "W/\"v0.3.0\""));
    TEST_ASSERT_TRUE(web_etag_match("  \"v0.3.0\"  ", etag));
    TEST_ASSERT_TRUE(web_etag_match("\"v0.2.0\", \"v0.3.0\"", etag));
    TEST_ASSERT_TRUE(web_etag_match("\"v0.2.0\",W/\"v0.3.0\"", etag));
    TEST_ASSERT_TRUE(web_etag_match("\"a\",,  \"v0.3.0\",", etag));
    TEST_ASSERT_TRUE(web_etag_match("*", etag));
    TEST_ASSERT_TRUE(web_etag_match(" * ", etag));
}

TEST_CASE("web etag: other, malformed and empty values do not match", "[web_assets]")
{
    const char *etag = "\"v0.3.0\"";
    TEST_ASSERT_FALSE(web_etag_match("\"v0.3.1\"", etag));
    TEST_ASSERT_FALSE(web_etag_match("\"v0.3.0+dev\"", etag));
    TEST_ASSERT_FALSE(web_etag_match("\"v0.3.\"", etag));
    TEST_ASSERT_FALSE(web_etag_match("v0.3.0", etag));               /* unquoted */
    TEST_ASSERT_FALSE(web_etag_match("\"v0.3.0", etag));             /* unterminated */
    TEST_ASSERT_FALSE(web_etag_match("w/\"v0.3.0\"", etag));         /* the prefix is case-sensitive */
    TEST_ASSERT_FALSE(web_etag_match("\"a\" x, \"v0.3.0\"", etag));  /* garbage stops the list */
    TEST_ASSERT_FALSE(web_etag_match("*, \"v0.3.0\"", etag));        /* "*" only alone */
    TEST_ASSERT_FALSE(web_etag_match("", etag));
    TEST_ASSERT_FALSE(web_etag_match(" , ", etag));
    TEST_ASSERT_FALSE(web_etag_match(NULL, etag));
    TEST_ASSERT_FALSE(web_etag_match("\"v0.3.0\"", NULL));
    TEST_ASSERT_FALSE(web_etag_match("\"v0.3.0\"", "v0.3.0"));       /* the server's tag must be quoted */
}
