/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Config/descriptor values are copied. Their strings and asset tables are
 * immutable; strings, tables, callbacks and contexts live for firmware lifetime.
 *
 * Singleton HTTP shell. Init/register on the app task before start. Register
 * once; start seals the registry, including across stop/start.
 * Stop rejects new HTTP work and interrupts/drains owned responses before stopping
 * HTTP; queued accepted jobs still run. Browser disconnect is not cancellation
 * of an accepted firmware operation. Product contexts survive stop: the worker
 * is not destroyed. Product cancellation belongs to its service owner, which
 * must preserve refusal/not_cancellable semantics and release its reservations.
 * Event/progress APIs copy input; they may be called from any ordinary task
 * (not ISR). HTTP/tick/provider callbacks must not block or call stop/init.
 */
#pragma once
#include "web_shell_assets.h"
#include "web_shell_http.h"
#include "web_shell_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/* All backend keys are explicit: no namespace defaults or migration. Values
 * keep the existing password/session codec and u8 public/setup representations. */
typedef struct { const char *ns; const char *key; } web_shell_storage_key_t;
typedef struct {
    web_shell_storage_key_t password, sessions, public_status, setup_done, setup_step, hostname;
} web_shell_storage_t;

typedef struct {
    web_shell_assets_t assets;
    const char *title;
    const char *default_brand;
    const char *product_noun;
    const char *project_name;
    const char *browser_storage_prefix;
    web_shell_storage_t storage;
    size_t route_capacity;          /* product routes; builtins use eight additional handlers */
    size_t section_capacity;        /* 1..16 */
    size_t status_bytes;            /* 256..16384, full frame/message limit */
    size_t json_body_bytes;         /* default 6144, 256..6144 */
    const char *const *img_origins;  /* up to four validated HTTPS origins */
    size_t img_origin_count;
    const char *const *connect_origins;
    size_t connect_origin_count;
    void *ctx;
    /* Called by the publishing task before queueing; writes an optional
     * coalescing key. Must be thread-safe/nonblocking, with no HTTP access. */
    bool (*event_key)(void *ctx, const char *json, char *out, size_t cap);
    void (*admin_activity)(void *ctx);              /* HTTP task */
    void (*tick)(void *ctx, bool second, bool half); /* HTTP task, before WS */
    void (*progress)(void *ctx);                    /* HTTP task, after events at 2 Hz */
    void (*scan_done)(void *ctx);                   /* HTTP task */
    const char *(*setup_step_name)(void *ctx, uint8_t step);
} web_shell_config_t;

esp_err_t web_shell_init(const web_shell_config_t *config);
esp_err_t web_shell_register_routes(const web_route_t *routes, size_t count);
esp_err_t web_shell_register_status(const web_shell_status_provider_t *provider);
esp_err_t web_shell_register_sections(const web_shell_section_t *sections, size_t count);
esp_err_t web_shell_start(void);
esp_err_t web_shell_stop(void);
/* Event JSON goes to authenticated clients. Overlong/malformed input fails
 * rather than truncating JSON. Progress/log are restricted to admin clients. */
esp_err_t web_shell_event(const char *json);
esp_err_t web_shell_publish(const char *topic, const char *json);
esp_err_t web_shell_progress(const char *json);

#ifdef __cplusplus
}
#endif
