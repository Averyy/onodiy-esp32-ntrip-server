/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Local NTRIP caster, step 0: a RAM-only stand-in for every function of
 * local_caster.h, so the web API, the UI and the tests can be built against
 * the interface before the caster itself lands. It creates no task, opens no
 * socket, registers no shutdown hook and stores nothing: init loads nothing,
 * a saved config lives in RAM until the next restart, and the status says
 * LOCAL_CASTER_LISTENING whenever that config is on (no rover ever connects).
 * local_caster_check_config() is the real rule set.
 */
#include "local_caster.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "settings.h"
#include "settings_schema.h"

_Static_assert(sizeof(CONFIG_LWIP_LOCAL_HOSTNAME) <= LOCAL_CASTER_MOUNT_LEN,
               "the build's hostname must fit the default mountpoint");

/* Everything below is guarded by s_lock. The config never holds a password:
 * the stub serves nobody, so it keeps only whether one was given. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_ready;               /* local_caster_init() ran (never in safe mode) */
static bool s_saved;               /* a config was set since boot; else the defaults apply */
static local_caster_cfg_t s_cfg;

/* 1 to size - 1 of [A-Za-z0-9._-], terminated within the buffer. */
static bool name_ok(const char *s, size_t size)
{
    size_t n = strnlen(s, size);
    if (n == 0 || n == size) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                  c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* LOCAL_CASTER_PASS_MIN to LOCAL_CASTER_PASS_LEN - 1 bytes of 0x21-0x7E (printable, no space). */
static bool password_ok(const char *s)
{
    size_t n = strnlen(s, LOCAL_CASTER_PASS_LEN);
    if (n < LOCAL_CASTER_PASS_MIN || n == LOCAL_CASTER_PASS_LEN) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x21u || c > 0x7Eu) {
            return false;
        }
    }
    return true;
}

static esp_err_t refuse(const char *f, const char *r, const char **field, const char **rule)
{
    if (field != NULL) {
        *field = f;
    }
    if (rule != NULL) {
        *rule = r;
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t local_caster_check_config(const local_caster_cfg_t *cfg, bool password_stored, const char **field,
                                    const char **rule)
{
    if (cfg == NULL) {
        return refuse(NULL, NULL, field, rule);
    }
    if (!name_ok(cfg->mountpoint, sizeof(cfg->mountpoint))) {
        return refuse("mountpoint", "1 to 47 letters, digits, dots, hyphens or underscores", field, rule);
    }
    if (!name_ok(cfg->username, sizeof(cfg->username))) {
        return refuse("username", "1 to 31 letters, digits, dots, hyphens or underscores", field, rule);
    }
    /* An empty password keeps the stored one. */
    bool given = cfg->password[0] != '\0';
    if (given && !password_ok(cfg->password)) {
        return refuse("password", "8 to 63 characters, letters, digits and symbols, no spaces", field, rule);
    }
    if (cfg->enabled && !given && !password_stored) {
        return refuse("password", "needed to turn this on", field, rule);
    }
    return ESP_OK;
}

/* The default mountpoint: the station's hostname, read now (settings
 * sys/hostname, else the build's). A stored name the mountpoint rule refuses
 * is passed over, as the network component passes over an invalid one. */
static void default_mountpoint(char *out, size_t size)
{
    char name[LOCAL_CASTER_MOUNT_LEN] = "";
    if (settings_get_str(SETTINGS_NS_SYS, SETTINGS_KEY_HOSTNAME, name, sizeof(name)) == ESP_OK &&
        name_ok(name, sizeof(name))) {
        strlcpy(out, name, size);
    } else {
        strlcpy(out, CONFIG_LWIP_LOCAL_HOSTNAME, size);
    }
}

esp_err_t local_caster_init(void)
{
    portENTER_CRITICAL(&s_lock);
    s_ready = true;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t local_caster_start(void)
{
    /* Nothing to create: the stub has no task. */
    portENTER_CRITICAL(&s_lock);
    bool ready = s_ready;
    portEXIT_CRITICAL(&s_lock);
    return ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t local_caster_get_config(local_caster_cfg_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    bool ready = s_ready;
    bool saved = s_saved;
    local_caster_cfg_t cfg = s_cfg;
    portEXIT_CRITICAL(&s_lock);
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!saved) {
        memset(&cfg, 0, sizeof(cfg));
        default_mountpoint(cfg.mountpoint, sizeof(cfg.mountpoint));
        strlcpy(cfg.username, LOCAL_CASTER_DEFAULT_USER, sizeof(cfg.username));
    }
    cfg.password[0] = '\0';
    *out = cfg;
    return ESP_OK;
}

esp_err_t local_caster_set_config(const local_caster_cfg_t *cfg, const char **field, const char **rule)
{
    portENTER_CRITICAL(&s_lock);
    bool ready = s_ready;
    bool password_stored = s_cfg.password_set;
    portEXIT_CRITICAL(&s_lock);
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }
    /* A stored password is never cleared, so a save racing this one can only
     * make password_stored stale towards the stricter rule. */
    esp_err_t err = local_caster_check_config(cfg, password_stored, field, rule);
    if (err != ESP_OK) {
        return err;
    }
    local_caster_cfg_t next = { 0 };
    next.enabled = cfg->enabled;
    strlcpy(next.mountpoint, cfg->mountpoint, sizeof(next.mountpoint));
    strlcpy(next.username, cfg->username, sizeof(next.username));
    bool given = cfg->password[0] != '\0';

    portENTER_CRITICAL(&s_lock);
    next.password_set = s_cfg.password_set || given;
    s_cfg = next;
    s_saved = true;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

void local_caster_get_status(local_caster_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->port = CONFIG_LOCAL_CASTER_PORT;
    out->max_clients = LOCAL_CASTER_MAX_CLIENTS;
    portENTER_CRITICAL(&s_lock);
    bool on = s_ready && s_cfg.enabled;
    portEXIT_CRITICAL(&s_lock);
    out->state = on ? LOCAL_CASTER_LISTENING : LOCAL_CASTER_OFF;
}
