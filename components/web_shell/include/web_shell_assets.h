/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */
#pragma once
#include "web_assets.h"
/* Product-owned generated storage; valid for firmware lifetime. The legacy
 * web_ui_assets globals remain supported by passing them in this descriptor. */
typedef struct {
    const web_asset_t *table;
    size_t count;
    const char *digest;
    size_t total;
} web_shell_assets_t;

/* Pure descriptor validation: bounded table, exact total/digest, unique paths,
 * header-safe MIME values, and no shared API/WS namespace. An empty asset set
 * is valid for a REST/WS-only product. */
#ifdef __cplusplus
extern "C" {
#endif
bool web_shell_assets_valid(const web_shell_assets_t *assets);
#ifdef __cplusplus
}
#endif
