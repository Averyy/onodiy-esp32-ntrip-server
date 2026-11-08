/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "network.h"
#include "settings.h"
#include "web_shell_internal.h"
#include "web_shell_build.h"
static const char *TAG="web";
static EXT_RAM_BSS_ATTR network_status_t s_net;
static esp_err_t ok(httpd_req_t *req) { return web_send_json_literal(req,200,"{\"ok\":true}"); }
static esp_err_t bad(httpd_req_t *req,const web_bad_t *b) { return web_send_error(req,b->err,b->rule,b->field,UINT32_MAX,NULL,NULL); }
static void resp_begin(web_resp_t *r,httpd_req_t *req,int status) { size_t cap; char *buf=web_http_buf(&cap); web_resp_begin(r,req,status,buf,cap); }
static bool settings_ok(void) { return settings_is_available(); }
static uint8_t read_u8(const char *ns,const char *key,uint8_t def) { uint8_t v; return settings_get_u8(ns,key,&v)==ESP_OK ? v : def; }
static esp_err_t h_session_get(web_req_t *r)
{
    web_admin_state_t admin = web_auth_admin_state();
    if (network_get_status(&s_net) != ESP_OK) {
        memset(&s_net, 0, sizeof(s_net));
    }
    bool authed = r->sess >= 0;
    char name[33];
    web_shell_name(name, sizeof(name));
    const web_shell_config_t *cfg=web_shell_config();
    uint8_t step=authed ? read_u8(cfg->storage.setup_step.ns,cfg->storage.setup_step.key,0) : 0;
    bool done=read_u8(cfg->storage.setup_done.ns,cfg->storage.setup_done.key,0) != 0;
    web_shell_session_snap_t snap={
        .authenticated=authed,.admin_set=admin != WEB_ADMIN_NONE,.setup_done=done,
        .claim_allowed=admin == WEB_ADMIN_NONE && r->on_ap,.on_setup_ap=r->on_ap,
        .public_status=web_public_status(),.settings_available=settings_ok(),
        .idle_left_s=authed ? web_auth_session_left_s(r->sess) : 0,
        .hostname=name,.mdns_host=s_net.mdns_host,.ap_ssid=s_net.ap_ssid,
        .setup_step=authed && !done && cfg->setup_step_name ? cfg->setup_step_name(cfg->ctx,step) : NULL,
    };
    web_resp_t resp;
    resp_begin(&resp,r->req,200);
    web_shell_build_session(&resp.w,&snap);
    return web_resp_end(&resp);
}

static esp_err_t h_session_post(web_req_t *r)
{
    web_admin_state_t admin = web_auth_admin_state();
    if (admin == WEB_ADMIN_NONE) {
        return web_send_err(r->req, WEB_ERR_ADMIN_NOT_SET);
    }
    if (admin == WEB_ADMIN_UNREADABLE) {
        return web_send_error(r->req, WEB_ERR_SETTINGS_UNAVAILABLE,
                              "The stored password cannot be read: hold the BOOT button to reset it", NULL, UINT32_MAX,
                              NULL, NULL);
    }
    uint32_t wait = web_auth_throttle_wait_s(r->req, r->on_ap);
    if (wait > 0) {
        return web_send_error(r->req, WEB_ERR_RATE_LIMITED, NULL, NULL, wait, NULL, NULL);
    }
    web_bad_t b = { 0 };
    const char *pw = NULL;
    bool remember = false;
    if (!web_parse_login(r->body, &pw, &remember, &b)) {
        return bad(r->req, &b);
    }
    if (!web_auth_check(r->req, r->on_ap, pw)) {
        return web_send_err(r->req, WEB_ERR_WRONG_PASSWORD);
    }
    /* A fresh login replaces the one this browser had. */
    char cookie[96];
    if (web_auth_start_session(r->req, remember, r->sess, cookie) < 0) {
        return web_send_internal(r->req, ESP_FAIL);
    }
    if (remember) {
        return web_send_json_literal(r->req, 200, "{\"authenticated\":true,\"idle_timeout_s\":null,\"remembered\":true}");
    }
    char body[80];
    snprintf(body, sizeof(body), "{\"authenticated\":true,\"idle_timeout_s\":%u,\"remembered\":false}",
             (unsigned)WEB_SESSION_IDLE_S);
    return web_send_json_literal(r->req, 200, body);
}

static esp_err_t h_session_delete(web_req_t *r)
{
    web_auth_logout(r->req, r->sess);
    return ok(r->req);
}

static esp_err_t h_setup_admin(web_req_t *r)
{
    if (web_auth_admin_state() != WEB_ADMIN_NONE) {
        return web_send_err(r->req, WEB_ERR_ADMIN_ALREADY_SET);
    }
    if (!r->on_ap) {
        return web_send_err(r->req, WEB_ERR_NOT_ON_SETUP_AP);
    }
    web_bad_t b = { 0 };
    const char *pw = NULL;
    if (!web_parse_password(r->body, "password", &pw, &b)) {
        return bad(r->req, &b);
    }
    if (!web_password_valid(pw)) {
        return web_send_error(r->req, WEB_ERR_INVALID_FIELD, "8-64 characters, no control characters", "password",
                              UINT32_MAX, NULL, NULL);
    }
    bool persisted = false;
    esp_err_t err = web_auth_set_password(pw, &persisted);
    if (err != ESP_OK) {
        return web_send_internal(r->req, err);
    }
    ESP_LOGW(TAG, "admin password set over the setup Wi-Fi%s", persisted ? "" : " (this boot only)");
    /* Never remembered: its cookie belongs to the setup network's address, which closes after the setup. */
    char cookie[96];
    if (web_auth_start_session(r->req, false, -1, cookie) < 0) {
        return web_send_internal(r->req, ESP_FAIL);
    }
    return web_send_json_literal(r->req, 200, persisted ? "{\"authenticated\":true,\"persisted\":true}"
                                                        : "{\"authenticated\":true,\"persisted\":false}");
}

static esp_err_t h_admin_password(web_req_t *r)
{
    uint32_t wait = web_auth_throttle_wait_s(r->req, r->on_ap);
    if (wait > 0) {
        return web_send_error(r->req, WEB_ERR_RATE_LIMITED, NULL, NULL, wait, NULL, NULL);
    }
    web_bad_t b = { 0 };
    const char *cur = NULL;
    const char *next = NULL;
    if (!(web_parse_password(r->body, "current", &cur, &b) && web_parse_password(r->body, "new", &next, &b))) {
        return bad(r->req, &b);
    }
    if (!settings_ok()) {
        return web_send_err(r->req, WEB_ERR_SETTINGS_UNAVAILABLE);
    }
    if (!web_auth_check(r->req, r->on_ap, cur)) {
        return web_send_err(r->req, WEB_ERR_WRONG_PASSWORD);
    }
    if (!web_password_valid(next)) {
        return web_send_error(r->req, WEB_ERR_INVALID_FIELD, "8-64 characters, no control characters", "new",
                              UINT32_MAX, NULL, NULL);
    }
    bool persisted = false;
    esp_err_t err = web_auth_set_password(next, &persisted);
    if (err != ESP_OK) {
        return web_send_internal(r->req, err);
    }
    web_auth_end_sessions(r->sess);
    ESP_LOGW(TAG, "admin password changed: other sessions ended");
    return ok(r->req);
}


const web_route_t web_shell_builtin_routes[]={
 {"/api/session",HTTP_GET,WEB_ACC_PUB,WEB_BODY_NONE,0,h_session_get,NULL,NULL},
 {"/api/session",HTTP_POST,WEB_ACC_PUB,WEB_BODY_JSON,0,h_session_post,NULL,NULL},
 {"/api/session",HTTP_DELETE,WEB_ACC_ADMIN,WEB_BODY_NONE,0,h_session_delete,NULL,NULL},
 {"/api/setup/admin",HTTP_POST,WEB_ACC_CLAIM,WEB_BODY_JSON,0,h_setup_admin,NULL,NULL},
 {"/api/admin-password",HTTP_PUT,WEB_ACC_ADMIN,WEB_BODY_JSON,0,h_admin_password,NULL,NULL},
 {"/api/status",HTTP_GET,WEB_ACC_PUBSTAR,WEB_BODY_NONE,0,web_shell_status_rest,NULL,NULL},
};
const size_t web_shell_builtin_route_count=sizeof(web_shell_builtin_routes)/sizeof(web_shell_builtin_routes[0]);
