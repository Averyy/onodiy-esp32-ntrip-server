/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * PROVISIONAL: slot configuration persistence is implemented; the slot tasks
 * (connect, request, stream, backoff) are not yet.
 */
#include "ntrip_server.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "lifecycle.h"
#include "sdkconfig.h"
#include "settings.h"
#include "settings_codec.h"

static const char *TAG = "ntrip";

ESP_EVENT_DEFINE_BASE(NTRIP_EVENT);

#define CFG_BLOB_VERSION 1
#define CFG_BLOB_MAX     320

static ntrip_caster_cfg_t s_cfg[CONFIG_NTRIP_MAX_CASTERS];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_loaded;   /* slot configs read from settings (not in safe mode) */

static bool fits(const char *s, size_t size)
{
    return strnlen(s, size) < size;
}

static size_t encode_cfg(const ntrip_caster_cfg_t *c, uint8_t *buf, size_t cap)
{
    blob_writer_t w;
    blob_writer_init(&w, buf, cap);
    blob_put_u8(&w, CFG_BLOB_VERSION);
    blob_put_bool(&w, c->enabled);
    blob_put_bool(&w, c->protected_slot);
    blob_put_u8(&w, (uint8_t)c->proto);
    blob_put_bool(&w, c->tls);
    blob_put_u16(&w, c->port);
    blob_put_str(&w, c->name);
    blob_put_str(&w, c->host);
    blob_put_str(&w, c->mountpoint);
    blob_put_str(&w, c->username);
    blob_put_str(&w, c->password);
    return blob_writer_ok(&w) ? w.len : 0;
}

static bool decode_cfg(const uint8_t *buf, size_t len, ntrip_caster_cfg_t *out)
{
    blob_reader_t r;
    blob_reader_init(&r, buf, len);
    if (blob_get_u8(&r) != CFG_BLOB_VERSION) {
        return false;
    }
    ntrip_caster_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.enabled = blob_get_bool(&r);
    c.protected_slot = blob_get_bool(&r);
    c.proto = (ntrip_proto_t)blob_get_u8(&r);
    c.tls = blob_get_bool(&r);
    c.port = blob_get_u16(&r);
    blob_get_str(&r, c.name, sizeof(c.name));
    blob_get_str(&r, c.host, sizeof(c.host));
    blob_get_str(&r, c.mountpoint, sizeof(c.mountpoint));
    blob_get_str(&r, c.username, sizeof(c.username));
    blob_get_str(&r, c.password, sizeof(c.password));
    if (!blob_reader_ok(&r) || (c.proto != NTRIP_PROTO_V1 && c.proto != NTRIP_PROTO_V2)) {
        return false;
    }
    c.password_set = c.password[0] != '\0';
    *out = c;
    return true;
}

static void slot_defaults(size_t slot, ntrip_caster_cfg_t *out)
{
    if (slot == 0) {
        ntrip_server_onocoy_defaults(out);
    } else {
        memset(out, 0, sizeof(*out));
        out->port = 2101;
        out->proto = NTRIP_PROTO_V2;
    }
}

static esp_err_t load_slot(size_t slot, ntrip_caster_cfg_t *out)
{
    char ns[16];
    snprintf(ns, sizeof(ns), SETTINGS_NS_CASTER_FMT, (unsigned)slot);
    uint8_t buf[CFG_BLOB_MAX];
    size_t len = sizeof(buf);
    esp_err_t err = settings_get_blob(ns, SETTINGS_KEY_CST_CFG, buf, &len);
    if (err == ESP_OK && !decode_cfg(buf, len, out)) {
        ESP_LOGW(TAG, "slot %u: stored config unreadable, using defaults", (unsigned)slot);
        err = ESP_ERR_INVALID_VERSION;
    }
    if (err != ESP_OK) {
        slot_defaults(slot, out);
    }
    return err;
}

/* Planned reboot: close every session so the caster frees the mountpoint at
 * once. Runs inside the lifecycle shutdown budget. */
static void on_shutdown(uint32_t deadline_ms, void *ctx)
{
    (void)ctx;
    (void)deadline_ms;
    /* TODO: signal every slot to close (TLS close_notify / shutdown(SHUT_WR))
     * and wait for them, bounded by deadline_ms. */
}

esp_err_t ntrip_server_init(void)
{
    for (size_t i = 0; i < ntrip_server_slot_count(); i++) {
        (void)load_slot(i, &s_cfg[i]);
    }
    s_loaded = true;
    esp_err_t err = lifecycle_register_shutdown_hook(on_shutdown, NULL);
    if (err != ESP_OK) {
        return err;
    }
    /* TODO: create the slot tasks here (idle until start), one hub consumer
     * and fixed buffers each. */
    ESP_LOGI(TAG, "%d caster slots", CONFIG_NTRIP_MAX_CASTERS);
    return ESP_OK;
}

esp_err_t ntrip_server_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ntrip_server_stop(void)
{
    return ESP_OK;
}

size_t ntrip_server_slot_count(void)
{
    return CONFIG_NTRIP_MAX_CASTERS;
}

esp_err_t ntrip_server_get_config(size_t slot, ntrip_caster_cfg_t *out, bool with_secret)
{
    if (out == NULL || slot >= ntrip_server_slot_count()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_loaded) {
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&s_lock);
    *out = s_cfg[slot];
    taskEXIT_CRITICAL(&s_lock);
    out->password_set = out->password[0] != '\0';
    out->clear_password = false;
    if (!with_secret) {
        memset(out->password, 0, sizeof(out->password));
    }
    return ESP_OK;
}

esp_err_t ntrip_server_set_config(size_t slot, const ntrip_caster_cfg_t *cfg)
{
    if (cfg == NULL || slot >= ntrip_server_slot_count()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_loaded) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!fits(cfg->name, sizeof(cfg->name)) || !fits(cfg->host, sizeof(cfg->host)) ||
        !fits(cfg->mountpoint, sizeof(cfg->mountpoint)) || !fits(cfg->username, sizeof(cfg->username)) ||
        !fits(cfg->password, sizeof(cfg->password)) ||
        (cfg->proto != NTRIP_PROTO_V1 && cfg->proto != NTRIP_PROTO_V2)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->enabled && (cfg->host[0] == '\0' || cfg->port == 0 || cfg->mountpoint[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }

    ntrip_caster_cfg_t next = *cfg;
    next.clear_password = false;
    if (cfg->clear_password) {
        memset(next.password, 0, sizeof(next.password));
    } else if (next.password[0] == '\0') {
        /* Keep the stored password only for the same host: never hand one
         * caster's password to another. */
        taskENTER_CRITICAL(&s_lock);
        if (strcmp(next.host, s_cfg[slot].host) == 0) {
            memcpy(next.password, s_cfg[slot].password, sizeof(next.password));
        }
        taskEXIT_CRITICAL(&s_lock);
    }
    next.password_set = next.password[0] != '\0';

    uint8_t buf[CFG_BLOB_MAX];
    size_t len = encode_cfg(&next, buf, sizeof(buf));
    if (len == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    char ns[16];
    snprintf(ns, sizeof(ns), SETTINGS_NS_CASTER_FMT, (unsigned)slot);
    esp_err_t err = settings_set_blob(ns, SETTINGS_KEY_CST_CFG, buf, len);
    memset(buf, 0, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    taskENTER_CRITICAL(&s_lock);
    s_cfg[slot] = next;
    taskEXIT_CRITICAL(&s_lock);
    /* TODO: tell only this slot's task to reconnect with the new config. */
    return ESP_OK;
}

esp_err_t ntrip_server_get_status(size_t slot, ntrip_caster_status_t *out)
{
    if (out == NULL || slot >= ntrip_server_slot_count()) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->state = NTRIP_STATE_DISABLED;
    return ESP_OK;
}

esp_err_t ntrip_server_restart_slot(size_t slot)
{
    if (slot >= ntrip_server_slot_count()) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ntrip_server_set_paused(bool paused, bool disconnect_now)
{
    (void)paused;
    (void)disconnect_now;
    return ESP_ERR_NOT_SUPPORTED;
}

void ntrip_server_onocoy_defaults(ntrip_caster_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    out->enabled = false;  /* enabled once the user enters credentials */
    out->protected_slot = true;
    strlcpy(out->name, "Onocoy", sizeof(out->name));
    strlcpy(out->host, "servers.onocoy.com", sizeof(out->host));
    out->port = 2101;
    out->proto = NTRIP_PROTO_V1;
    out->tls = false;
}
