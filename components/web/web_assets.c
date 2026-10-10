/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Asset lookup and If-None-Match evaluation for the embedded UI (pure logic).
 */
#include "web_assets.h"

#include <string.h>

#define INDEX_PATH "/index.html"

const web_asset_t *web_asset_find(const web_asset_t *assets, size_t count, const char *uri)
{
    if (assets == NULL || uri == NULL || uri[0] != '/') {
        return NULL;
    }
    size_t len = strcspn(uri, "?#");
    const char *path = uri;
    if (len == 1) {
        path = INDEX_PATH;
        len = sizeof(INDEX_PATH) - 1;
    }
    for (size_t i = 0; i < count; i++) {
        const char *p = assets[i].path;
        if (p != NULL && strlen(p) == len && memcmp(p, path, len) == 0) {
            return &assets[i];
        }
    }
    return NULL;
}

/* Opaque tag of an entity tag: the quoted part, quotes included, without W/. */
static bool opaque_tag(const char *tag, const char **start, size_t *len)
{
    if (tag[0] == 'W' && tag[1] == '/') {
        tag += 2;
    }
    if (tag[0] != '"') {
        return false;
    }
    const char *end = strchr(tag + 1, '"');
    if (end == NULL) {
        return false;
    }
    *start = tag;
    *len = (size_t)(end - tag) + 1;
    return true;
}

static bool is_ows(char c)
{
    return c == ' ' || c == '\t';
}

bool web_etag_match(const char *if_none_match, const char *etag)
{
    const char *want;
    size_t want_len;
    if (if_none_match == NULL || etag == NULL || !opaque_tag(etag, &want, &want_len)) {
        return false;
    }
    const char *p = if_none_match;
    while (is_ows(*p)) {
        p++;
    }
    if (p[0] == '*') {
        const char *q = p + 1;
        while (is_ows(*q)) {
            q++;
        }
        return *q == '\0';
    }
    while (*p != '\0') {
        while (is_ows(*p) || *p == ',') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *tag;
        size_t tag_len;
        if (!opaque_tag(p, &tag, &tag_len)) {
            return false;
        }
        if (tag_len == want_len && memcmp(tag, want, want_len) == 0) {
            return true;
        }
        p = tag + tag_len;
        while (is_ows(*p)) {
            p++;
        }
        if (*p != ',' && *p != '\0') {
            return false;
        }
    }
    return false;
}
