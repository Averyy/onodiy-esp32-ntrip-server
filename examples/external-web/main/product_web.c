/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */
#include <stdatomic.h>
#include <string.h>
#include "esp_log.h"
#include "network.h"
#include "settings.h"
#include "counter_ui.h"
#include "product_web.h"
#include "web_shell.h"
#include "web_shell_auth.h"
#include "web_shell_parse.h"

#define PREFS "counter_ui"
#define AUTH "counter_auth"
static const char *const s_steps[] = { "network", "visibility" };
static atomic_bool s_recover;
static uint32_t s_counter;
/* Only the HTTP task captures/serializes this cached snapshot. */
static struct {
    uint32_t counter;
    bool network_known;
    network_status_t network;
} s_snapshot;

static const char *step_name(void *ctx, uint8_t step)
{
    (void)ctx;
    return step < sizeof(s_steps) / sizeof(s_steps[0]) ? s_steps[step] : s_steps[0];
}
static uint8_t pref(const char *key)
{
    uint8_t v = 0;
    (void)settings_get_u8(PREFS, key, &v);
    return v;
}
static bool capture(void *ctx, const void **snapshot, uint32_t *uptime)
{
    (void)ctx;
    s_snapshot.counter = s_counter;
    s_snapshot.network_known = network_get_status(&s_snapshot.network) == ESP_OK;
    *snapshot = &s_snapshot;
    *uptime = web_now_s();
    return true;
}
static void release(void *ctx, const void *snapshot) { (void)ctx; (void)snapshot; }
static bool product(void *ctx, const void *snapshot, bool pub, size_t item, wj_t *w, char *key)
{
    (void)ctx; (void)snapshot; (void)pub; (void)item; (void)key;
    wj_obj_open(w, "product");
    wj_uint(w, "counter", s_snapshot.counter);
    wj_obj_close(w);
    return wj_ok(w);
}
static bool network_section(void *ctx, const void *snapshot, bool pub, size_t item, wj_t *w, char *key)
{
    (void)ctx; (void)snapshot; (void)item; (void)key;
    const network_status_t *n = &s_snapshot.network;
    wj_obj_open(w, "network");
    if (s_snapshot.network_known) wj_bool(w, "online", n->online);
    else wj_null(w, "online");
    if (!pub) {
        wj_str_or_null(w, "ssid", s_snapshot.network_known ? n->ssid : NULL);
        wj_str_or_null(w, "ip", s_snapshot.network_known && n->online ? n->ip : NULL);
        if (s_snapshot.network_known) wj_bool(w, "setup_ap", n->setup_ap_active);
        else wj_null(w, "setup_ap");
    }
    wj_obj_close(w);
    return wj_ok(w);
}
static bool preferences(void *ctx, const void *snapshot, bool pub, size_t item, wj_t *w, char *key)
{
    (void)ctx; (void)snapshot; (void)pub; (void)item; (void)key;
    wj_obj_open(w, "preferences");
    wj_bool(w, "settings_available", settings_is_available());
    wj_obj_close(w);
    return wj_ok(w);
}
static esp_err_t settings_get(web_req_t *r)
{
    char buf[384];
    wj_t w;
    wj_init(&w, buf, sizeof(buf), NULL, NULL);
    wj_obj_open(&w, NULL);
    const bool available = settings_is_available();
    wj_bool(&w, "settings_available", available);
    if (available) {
        wj_bool(&w, "public_status", pref("public"));
        wj_bool(&w, "setup_done", pref("done"));
        wj_str(&w, "setup_step", step_name(NULL, pref("step")));
    } else {
        wj_null(&w, "public_status");
        wj_null(&w, "setup_done");
        wj_null(&w, "setup_step");
    }
    wj_obj_close(&w);
    return web_send_json_literal(r->req, 200, wj_text(&w));
}
static esp_err_t invalid(web_req_t *r, const char *field)
{
    return web_send_error(r->req, WEB_ERR_INVALID_FIELD, NULL, field, UINT32_MAX, NULL, NULL);
}
static esp_err_t settings_put(web_req_t *r)
{
    bool public_status = false, done = false;
    const char *step = NULL;
    web_member_t p = web_member_bool(r->body, "public_status", &public_status);
    web_member_t d = web_member_bool(r->body, "setup_done", &done);
    web_member_t s = web_member_str(r->body, "setup_step", 16, &step);
    if (p == WEB_MEMBER_BAD) return invalid(r, "public_status");
    if (d == WEB_MEMBER_BAD) return invalid(r, "setup_done");
    if (s == WEB_MEMBER_BAD || (s == WEB_MEMBER_OK && strcmp(step, s_steps[0]) && strcmp(step, s_steps[1])))
        return invalid(r, "setup_step");
    if (!settings_is_available()) return web_send_err(r->req, WEB_ERR_SETTINGS_UNAVAILABLE);
    settings_batch_t b;
    esp_err_t err = settings_batch_begin(PREFS, &b);
    if (err != ESP_OK) return web_send_internal(r->req, err);
    if (p == WEB_MEMBER_OK) err = settings_batch_set_u8(b, "public", public_status);
    if (err == ESP_OK && d == WEB_MEMBER_OK) err = settings_batch_set_u8(b, "done", done);
    if (err == ESP_OK && s == WEB_MEMBER_OK) err = settings_batch_set_u8(b, "step", !strcmp(step, s_steps[1]));
    if (err != ESP_OK) { settings_batch_abort(b); return web_send_internal(r->req, err); }
    err = settings_batch_commit(b);
    if (err != ESP_OK) return web_send_internal(r->req, err);
    if (p == WEB_MEMBER_OK) web_public_status_set_cached(public_status);
    return settings_get(r);
}
void product_web_recover(void) { atomic_store(&s_recover, true); web_kick(); }
static void tick(void *ctx, bool second, bool half)
{
    (void)ctx; (void)half;
    if (second) ++s_counter;
    if (atomic_exchange(&s_recover, false)) {
        esp_err_t err = settings_erase_key(AUTH, "password");
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND && settings_is_available())
            ESP_LOGW("product", "password reset: %s", esp_err_to_name(err));
        web_auth_admin_reset();
    }
}
esp_err_t product_web_init(void)
{
    const web_shell_config_t config = {
        .assets = counter_ui_assets,
        .title = COUNTER_UI_TITLE, .default_brand = COUNTER_UI_DEFAULT_BRAND,
        .product_noun = COUNTER_UI_PRODUCT_NOUN, .project_name = COUNTER_UI_PROJECT_NAME,
        .browser_storage_prefix = COUNTER_UI_BROWSER_STORAGE_PREFIX,
        .storage = { .password = {AUTH, "password"}, .sessions = {AUTH, "sessions"},
            .public_status = {PREFS, "public"}, .setup_done = {PREFS, "done"},
            .setup_step = {PREFS, "step"}, .hostname = {PREFS, "hostname"} },
        .route_capacity = 2, .section_capacity = 3, .status_bytes = 2048,
        .json_body_bytes = 256, .tick = tick, .setup_step_name = step_name,
    };
    esp_err_t err = web_shell_init(&config);
    if (err != ESP_OK) return err;
    const web_shell_status_provider_t provider = { .capture = capture, .release = release };
    err = web_shell_register_status(&provider);
    if (err != ESP_OK) return err;
    const web_shell_section_t sections[] = {
        { .key = "product", .visibility = WEB_SHELL_PUBLIC, .max_bytes = 80,
          .max_items = 1, .max_item_bytes = 80, .serialize = product },
        { .key = "network", .visibility = WEB_SHELL_PUBLIC, .max_bytes = 256,
          .max_items = 1, .max_item_bytes = 256, .serialize = network_section },
        { .key = "preferences", .max_bytes = 80, .max_items = 1,
          .max_item_bytes = 80, .serialize = preferences },
    };
    err = web_shell_register_sections(sections, sizeof(sections) / sizeof(sections[0]));
    if (err != ESP_OK) return err;
    const web_route_t routes[] = {
        { .uri = "/api/product/settings", .method = HTTP_GET, .access = WEB_ACC_ADMIN,
          .fn = settings_get },
        { .uri = "/api/product/settings", .method = HTTP_PUT, .access = WEB_ACC_ADMIN,
          .body = WEB_BODY_JSON, .max_body = 256, .fn = settings_put },
    };
    return web_shell_register_routes(routes, sizeof(routes) / sizeof(routes[0]));
}
