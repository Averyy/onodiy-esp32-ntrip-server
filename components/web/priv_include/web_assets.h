/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Embedded UI assets. The table is generated at build time by ui_pack.py
 * from every file directly in components/web/ui/ (see "UI assets" in
 * API.md). Every entry is gzip-compressed and served with
 * Content-Encoding: gzip.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *path;        /**< URL path, "/app.js" ("/index.html" is also served at "/") */
    const char *mime;        /**< Content-Type */
    const uint8_t *data;     /**< gzip-compressed content */
    size_t len;
} web_asset_t;

extern const web_asset_t web_ui_assets[];
extern const size_t web_ui_asset_count;

#ifdef __cplusplus
}
#endif
