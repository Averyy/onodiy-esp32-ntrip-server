/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Where releases come from (pure logic, host-testable): the repository name,
 * the URLs built from it, and the hosts a transfer may connect to.
 *
 * Every update transfer starts at github.com:
 *
 *   https://github.com/<owner>/<name>/releases/latest/download/<manifest>
 *   https://github.com/<owner>/<name>/releases/download/<tag>/<asset>
 *
 * For a public repository GitHub answers both with redirects: "latest" to
 * the tagged URL on github.com, the tagged URL to a short-lived signed URL on
 * its release asset CDN, release-assets.githubusercontent.com (where GitHub
 * moved release downloads in 2025; objects.githubusercontent.com, the host
 * before that, stays on the list). Before every connection (the first and
 * each redirect) fw_http.c checks the URL the HTTP client is about to use:
 * https, port 443, no user info, and one of exactly these hosts
 * (case-insensitive, no wildcards: raw.githubusercontent.com and other
 * GitHub-hosted content are not release hosts). The release workflow follows
 * the same redirects for every published release and fails when a hop
 * leaves this list (.github/workflows/build.yml reads it from release_url.c).
 * Should GitHub move release downloads to another host, devices report
 * untrusted_host until a firmware with the new host is installed by a manual
 * upload.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Number of hosts in the list (for tests and logs). */
#define FW_RELEASE_HOST_COUNT 3

/** The i-th release host (NULL past the end). */
const char *fw_release_host(size_t i);

/** `host` (exactly `len` characters, no port) is a release host. */
bool fw_release_host_ok(const char *host, size_t len);

/**
 * `url` is "https://<release host>[:443]" followed by nothing, a path ("/...")
 * or a query; no user info, no other port, no other scheme. Scheme and host
 * compare case-insensitively.
 */
bool fw_release_url_ok(const char *url);

/**
 * "owner/name" as GitHub allows it: one '/', owner 1-39 of [A-Za-z0-9-],
 * name 1-100 of [A-Za-z0-9._-] and not "." or "..".
 */
bool fw_release_repo_ok(const char *repo);

/** https://github.com/<repo>/releases/latest/download/<asset>; false if a part is invalid or it does not fit. */
bool fw_release_latest_url(char *out, size_t out_size, const char *repo, const char *asset);

/** https://github.com/<repo>/releases/download/<tag>/<asset>; false if a part is invalid or it does not fit. */
bool fw_release_asset_url(char *out, size_t out_size, const char *repo, const char *tag, const char *asset);

#ifdef __cplusplus
}
#endif
