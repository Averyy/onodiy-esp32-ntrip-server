/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 * Pure registration, frame/delta and asynchronous request ownership decisions.
 * No RTOS, sockets or product headers. Runtime adapters provide the effects.
 */
#pragma once
#include "web_shell_provider.h"
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif
#define WEB_SHELL_ROUTES_MAX 84u

typedef struct {
    const char *uri;
    int method;                /* HTTP_ANY = INT_MAX */
    bool (*path_known)(void *ctx, const char *path);
    void *ctx;
} web_shell_route_rule_t;

typedef struct {
    size_t route_cap, section_cap, status_cap;
    size_t route_count, section_count;
    bool sealed, failed;
    web_shell_route_rule_t routes[WEB_SHELL_ROUTES_MAX];
    web_shell_section_t sections[WEB_SHELL_SECTIONS_MAX];
    web_shell_status_provider_t status;
} web_shell_registry_t;

bool web_shell_registry_init(web_shell_registry_t *r, size_t routes, size_t sections, size_t status_bytes);
bool web_shell_registry_routes(web_shell_registry_t *r, const web_shell_route_rule_t *routes, size_t count);
bool web_shell_registry_sections(web_shell_registry_t *r, const web_shell_section_t *sections, size_t count);
bool web_shell_registry_status(web_shell_registry_t *r, const web_shell_status_provider_t *provider);
bool web_shell_registry_seal(web_shell_registry_t *r);
bool web_shell_registry_path(const web_shell_registry_t *r, const char *path);

/* A frame owns one heap block of status_cap bytes, kept across builds so a
 * periodic build allocates nothing; zero initialize before use, dispose after
 * use, one registry per frame. Build failure leaves a frame that renders
 * nothing. Baselines are explicit commits, so failed serialization or
 * composition cannot accidentally acknowledge changes. */
typedef struct {
    unsigned section;
    char key[WEB_SHELL_ITEM_KEY_MAX + 1];
    uint32_t crc;
    size_t offset, len;
} web_shell_cell_t;
typedef struct {
    char *text;
    size_t cap, len, count;
    uint32_t uptime_s;
    bool public_form;
    bool built;
    web_shell_cell_t cells[WEB_SHELL_ITEMS_MAX];
} web_shell_frame_t;
typedef struct {
    bool valid;
    size_t count;
    web_shell_cell_t cells[WEB_SHELL_ITEMS_MAX];
} web_shell_baseline_t;

/* Takes the frame's block now (a start-up call: later builds then cannot fail for memory). */
bool web_shell_frame_reserve(const web_shell_registry_t *r, web_shell_frame_t *frame);
bool web_shell_frame_build(const web_shell_registry_t *r, const void *snapshot,
                           uint32_t uptime_s, bool pub, web_shell_frame_t *frame);
void web_shell_frame_dispose(web_shell_frame_t *frame);
bool web_shell_frame_removed(const web_shell_frame_t *f, const web_shell_baseline_t *baseline);
/* The REST form written straight to `w`, a streaming writer: no frame and no
 * buffer beyond the writer's own, the same bytes as a full ws=false render,
 * under the same byte limits. A section's key prefix is checked by frame
 * builds only: a fragment already flushed cannot be read back. */
bool web_shell_status_stream(const web_shell_registry_t *r, const void *snapshot,
                             uint32_t uptime_s, bool pub, wj_t *w);
/* ws=false: REST envelope. ws=true: status/data envelope and full/delta. */
size_t web_shell_frame_render(const web_shell_registry_t *r, const web_shell_frame_t *f,
                             const web_shell_baseline_t *baseline, bool ws, bool full,
                             uint32_t seq, char *out, size_t cap);
void web_shell_frame_commit(const web_shell_frame_t *f, web_shell_baseline_t *baseline);

/* Pure exactly-once ownership protocol used by the real shared worker.
 * Acquire failure owns nothing; enqueue failure restores deadlines and
 * completes the copy; successful enqueue transfers ownership to the worker.
 * Finish shuts/drains incomplete/close responses before completion. */
typedef struct {
    void *ctx;
    void *(*acquire)(void *ctx, void *request);
    void (*deadline)(void *ctx, void *request, bool owned);
    bool (*enqueue)(void *ctx, void *copy);
    void (*close)(void *ctx, void *copy);
    void (*complete)(void *ctx, void *copy);
} web_shell_owner_ops_t;
bool web_shell_owner_submit(const web_shell_owner_ops_t *ops, void *request);
void web_shell_owner_finish(const web_shell_owner_ops_t *ops, void **owned, bool close);

/* Validated declarative CSP: HTTPS DNS origins only, no paths/query/ports,
 * wildcard, whitespace, credentials, or policy fragments. Host is validated
 * by the request precheck; blanks around it are left out of the policy.
 * Fixed directives cannot be overridden. */
bool web_shell_origin_valid(const char *origin);
bool web_shell_csp(char *out, size_t cap, const char *host,
                   const char *const *img, size_t img_count,
                   const char *const *connect, size_t connect_count);

#ifdef __cplusplus
}
#endif
