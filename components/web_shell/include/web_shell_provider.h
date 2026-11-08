/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Register before start, once per firmware lifetime. Descriptors are copied;
 * their strings are immutable; strings, callback code and contexts remain valid for firmware
 * lifetime (stop keeps the worker). Registration batches are atomic. A failed
 * registration prevents start until init is retried before any successful
 * start. No registration is allowed after the first start attempt.
 *
 * Capture/release and serializers run on the HTTP task, never concurrently.
 * Capture once per REST request or WS cycle; return a consistent cached
 * snapshot and its captured monotonic uptime. Release is called exactly once
 * after every successful capture, including serialization/allocation failures.
 * A failing capture owns its cleanup. Callbacks must be bounded/nonblocking;
 * never retain request, snapshot or writer pointers beyond release/return.
 * Represent unknown/unavailable values explicitly rather than inventing data.
 *
 * Serializers write bounded JSON fragments through the supplied writer:
 * REPLACE writes exactly one member named key; KEYED_ARRAY writes one array
 * element with a stable, nonempty item key, or nothing for an absent item.
 * Public serializers must redact private fields. Public visibility is opt-in.
 * A serializer failure/overflow invalidates the entire frame: no partial REST
 * JSON, no WS message, no delta baseline advance. WS retries the next cycle;
 * removal of a keyed item forces a full snapshot. Client queue refusal also
 * forces resync. Failed snapshots never refresh the client's freshness.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "web_json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_SHELL_SECTIONS_MAX 16u
#define WEB_SHELL_ITEMS_MAX 64u
#define WEB_SHELL_ITEM_KEY_MAX 32u
#define WEB_SHELL_STATUS_MAX 16384u
#define WEB_SHELL_API_VERSION 1

typedef struct {
    void *ctx;
    bool (*capture)(void *ctx, const void **snapshot, uint32_t *uptime_s);
    void (*release)(void *ctx, const void *snapshot);
} web_shell_status_provider_t;

typedef enum {
    WEB_SHELL_ADMIN_ONLY = 0,
    WEB_SHELL_PUBLIC = 1,
} web_shell_visibility_t;

typedef enum {
    WEB_SHELL_REPLACE = 0,
    WEB_SHELL_KEYED_ARRAY = 1,
} web_shell_merge_t;

typedef struct {
    const char *key;
    web_shell_visibility_t visibility;
    web_shell_merge_t merge;
    size_t max_bytes;          /* complete section, including key and punctuation */
    size_t max_items;          /* KEYED_ARRAY slots; 1 for REPLACE */
    size_t max_item_bytes;     /* one fragment, excluding its NUL */
    void *ctx;
    bool (*serialize)(void *ctx, const void *snapshot, bool public_form,
                      size_t item, wj_t *writer, char item_key[WEB_SHELL_ITEM_KEY_MAX + 1]);
} web_shell_section_t;

#ifdef __cplusplus
}
#endif
