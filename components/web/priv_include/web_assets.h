/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Embedded browser UI: the asset table and the lookups the HTTP handlers
 * need (pure logic, no IDF includes, host-testable).
 *
 * The build generates the table (components/web/tools/ui_bundle.py, run by
 * this component's CMakeLists.txt): every asset gzip-compressed, in flash,
 * served with Content-Encoding: gzip. The names are fixed (API.md, request
 * checks):
 *
 *   /index.html   the page; also served for "/"
 *   /app.js       the whole UI script
 *   /app.css      styles
 *   /favicon.svg  icon
 *
 * The UI routes with the URL fragment (/#/casters), so no other path is a
 * UI asset. Conditional requests use the ETag the server chooses (API.md:
 * the app version and web_ui_digest, quoted); web_etag_match() evaluates
 * If-None-Match against it. web_ui_digest identifies the bundle itself (first
 * 16 hex digits of a SHA-256 over the compressed assets), so a development
 * build whose version string does not change still gets a fresh UI.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One embedded asset. */
typedef struct {
    const char *path;      /**< served path, e.g. "/app.js" */
    const char *type;      /**< Content-Type value */
    const uint8_t *gz;     /**< gzip-compressed body */
    size_t gz_len;         /**< its length in bytes */
} web_asset_t;

/** The generated table (web_ui_assets.c in the build directory). */
extern const web_asset_t web_ui_assets[];
extern const size_t web_ui_asset_count;
extern const char web_ui_digest[];
extern const size_t web_ui_gz_total;

/**
 * The asset a request URI names, or NULL. Only the path counts: a query
 * ("?v=2") or fragment is ignored, and "/" means "/index.html". Matching is
 * exact and case-sensitive.
 *
 * @param assets table to search (normally web_ui_assets)
 * @param count  its number of entries
 * @param uri    request URI as received (may be NULL)
 */
const web_asset_t *web_asset_find(const web_asset_t *assets, size_t count, const char *uri);

/**
 * True when an If-None-Match header value matches `etag` (RFC 9110 weak
 * comparison, as If-None-Match uses): "*", or any entry of the
 * comma-separated list whose opaque tag equals etag's, with or without the
 * W/ prefix on either side. etag is the full quoted value the server sends
 * (e.g. "\"v0.3.0\""). A malformed list stops matching at the first bad
 * entry; NULL or empty never matches.
 */
bool web_etag_match(const char *if_none_match, const char *etag);

#ifdef __cplusplus
}
#endif
