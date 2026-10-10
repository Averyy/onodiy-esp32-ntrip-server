/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "release_url.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "manifest.h"

static const char *const s_hosts[FW_RELEASE_HOST_COUNT] = {
    "github.com",
    "release-assets.githubusercontent.com",
    "objects.githubusercontent.com",
};

const char *fw_release_host(size_t i)
{
    return i < FW_RELEASE_HOST_COUNT ? s_hosts[i] : NULL;
}

bool fw_release_host_ok(const char *host, size_t len)
{
    if (host == NULL) {
        return false;
    }
    for (size_t i = 0; i < FW_RELEASE_HOST_COUNT; i++) {
        if (strlen(s_hosts[i]) == len && strncasecmp(host, s_hosts[i], len) == 0) {
            return true;
        }
    }
    return false;
}

bool fw_release_url_ok(const char *url)
{
    static const char scheme[] = "https://";
    if (url == NULL || strncasecmp(url, scheme, sizeof(scheme) - 1) != 0) {
        return false;
    }
    const char *host = url + sizeof(scheme) - 1;
    /* The authority ends at the path, the query or the fragment. */
    size_t auth_len = strcspn(host, "/?#");
    const char *colon = memchr(host, ':', auth_len);
    if (memchr(host, '@', auth_len) != NULL) {
        return false;   /* user info */
    }
    size_t host_len = colon != NULL ? (size_t)(colon - host) : auth_len;
    if (colon != NULL) {
        size_t port_len = auth_len - host_len - 1;
        if (port_len != 3 || strncmp(colon + 1, "443", 3) != 0) {
            return false;
        }
    }
    return fw_release_host_ok(host, host_len);
}

bool fw_release_hop_ok(const char *url, const char *repo)
{
    if (!fw_release_url_ok(url) || !fw_release_repo_ok(repo)) {
        return false;
    }
    const char *host = url + strlen("https://");
    size_t auth_len = strcspn(host, "/?#");
    size_t host_len = strcspn(host, ":/?#");
    if (!(host_len == strlen(s_hosts[0]) && strncasecmp(host, s_hosts[0], host_len) == 0)) {
        return true;   /* the asset CDN: the host is the rule */
    }
    /* github.com: "/<repo>/releases/" (the name is checked as configured,
     * which may start with a dot), then no dot segment and no escapes. */
    const char *path = host + auth_len;
    size_t path_len = strcspn(path, "?#");
    size_t repo_len = strlen(repo);
    static const char releases[] = "/releases/";
    if (path_len < 1 + repo_len + sizeof(releases) - 1 || path[0] != '/' ||
        strncasecmp(path + 1, repo, repo_len) != 0 ||
        strncasecmp(path + 1 + repo_len, releases, sizeof(releases) - 1) != 0) {
        return false;
    }
    for (size_t i = 1 + repo_len; i < path_len; i++) {
        if (path[i] == '%' || path[i] == '\\' || (path[i] == '/' && i + 1 < path_len && path[i + 1] == '.')) {
            return false;
        }
    }
    return true;
}

static bool part_ok(const char *s, size_t len, size_t max, bool owner)
{
    if (len == 0 || len > max) {
        return false;
    }
    if (!owner && ((len == 1 && s[0] == '.') || (len == 2 && s[0] == '.' && s[1] == '.'))) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                  (!owner && (c == '.' || c == '_'));
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool fw_release_repo_ok(const char *repo)
{
    if (repo == NULL) {
        return false;
    }
    const char *slash = strchr(repo, '/');
    if (slash == NULL) {
        return false;
    }
    const char *name = slash + 1;
    return part_ok(repo, (size_t)(slash - repo), 39, true) && part_ok(name, strlen(name), 100, false);
}

static bool fits(int n, size_t out_size)
{
    return n > 0 && (size_t)n < out_size;
}

bool fw_release_latest_url(char *out, size_t out_size, const char *repo, const char *asset)
{
    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (!fw_release_repo_ok(repo) || !fw_manifest_asset_name_ok(asset)) {
        return false;
    }
    return fits(snprintf(out, out_size, "https://github.com/%s/releases/latest/download/%s", repo, asset), out_size);
}

bool fw_release_asset_url(char *out, size_t out_size, const char *repo, const char *tag, const char *asset)
{
    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    /* A release tag is a version (semver.h), so the asset-name rule covers it as a path segment. */
    if (!fw_release_repo_ok(repo) || !fw_manifest_asset_name_ok(tag) || !fw_manifest_asset_name_ok(asset)) {
        return false;
    }
    return fits(snprintf(out, out_size, "https://github.com/%s/releases/download/%s/%s", repo, tag, asset),
                out_size);
}
