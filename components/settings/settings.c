/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";

ESP_EVENT_DEFINE_BASE(SETTINGS_EVENT);

static uint16_t s_stored_schema;
static bool s_available;

#define CASTER_NAMESPACE_SLOTS 8 /* upper bound of CONFIG_NTRIP_MAX_CASTERS */

/* User-config namespaces a factory reset clears ("life" and "ledger" hold
 * device history and are left alone). */
static const char *const s_user_namespaces[] = {
    SETTINGS_NS_SYS, SETTINGS_NS_WIFI, SETTINGS_NS_GNSS, SETTINGS_NS_OTA, SETTINGS_NS_WEB,
};

/* Device state inside user namespaces that a factory reset must keep. The
 * schema version stays too: kept records are in that version's format, and a
 * reset must not make a newer app re-run migrations over them. */
static const struct {
    const char *ns;
    const char *key;
} s_keep_on_reset[] = {
    { SETTINGS_NS_SYS,  SETTINGS_KEY_SCHEMA },
    { SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_FLASH_JOB },
    { SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_MODEL },
    { SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_FW },
    { SETTINGS_NS_OTA,  SETTINGS_KEY_OTA_TRIAL },
    { SETTINGS_NS_OTA,  SETTINGS_KEY_OTA_BAD_VER },
};

static void notify_changed(const char *ns, const char *key)
{
    settings_changed_t ev = { 0 };
    strlcpy(ev.ns, ns, sizeof(ev.ns));
    strlcpy(ev.key, key ? key : "", sizeof(ev.key));
    /* Non-blocking: a full event queue must never stall a settings write.
     * Events are advisory, so a drop only delays a UI refresh. */
    if (esp_event_post(SETTINGS_EVENT, SETTINGS_EVENT_CHANGED, &ev, sizeof(ev), 0) != ESP_OK) {
        ESP_LOGW(TAG, "change event for %s/%s dropped", ns, ev.key);
    }
}

esp_err_t settings_init(void)
{
    s_available = false;
    uint16_t stored = 0;
    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NS_SYS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_u16(h, SETTINGS_KEY_SCHEMA, &stored);
        nvs_close(h);
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }
    s_stored_schema = stored;
    /* NVS is readable: settings are usable from here on, whatever happens to
     * the version stamp below. */
    s_available = true;

    if (stored > SETTINGS_SCHEMA_VERSION) {
        ESP_LOGW(TAG, "stored schema v%u is newer than v%u (rollback?): leaving data as-is",
                 (unsigned)stored, (unsigned)SETTINGS_SCHEMA_VERSION);
        return ESP_OK;
    }

    /* Migrations run here in order: v0->v1, v1->v2, ... each idempotent, each
     * copying forward without deleting retired keys. v0 means a fresh device:
     * nothing to migrate, defaults come from Kconfig. */

    if (stored != SETTINGS_SCHEMA_VERSION) {
        err = nvs_open(SETTINGS_NS_SYS, NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = nvs_set_u16(h, SETTINGS_KEY_SCHEMA, SETTINGS_SCHEMA_VERSION);
            if (err == ESP_OK) {
                err = nvs_commit(h);
            }
            nvs_close(h);
        }
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "schema v%u -> v%u", (unsigned)stored, (unsigned)SETTINGS_SCHEMA_VERSION);
        } else {
            /* Retried at the next boot (migrations are idempotent). Reads must
             * not depend on this write: without settings there are no Wi-Fi
             * credentials and, by design, no setup AP. */
            ESP_LOGW(TAG, "schema v%u -> v%u not recorded (%s): retried at the next boot",
                     (unsigned)stored, (unsigned)SETTINGS_SCHEMA_VERSION, esp_err_to_name(err));
        }
    }
    return ESP_OK;
}

bool settings_is_available(void)
{
    return s_available;
}

uint16_t settings_stored_schema(void)
{
    return s_stored_schema;
}

/* ---- generic open/read/write helpers -------------------------------------- */

static esp_err_t open_ns(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h)
{
    if (ns == NULL || h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_available) {
        return ESP_ERR_INVALID_STATE;
    }
    return nvs_open(ns, mode, h);
}

static esp_err_t finish_write(nvs_handle_t h, esp_err_t err, const char *ns, const char *key)
{
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        notify_changed(ns, key);
    }
    return err;
}

#define SETTINGS_SCALAR_ACCESSORS(suffix, type)                                        \
    esp_err_t settings_get_##suffix(const char *ns, const char *key, type *out)        \
    {                                                                                  \
        if (key == NULL || out == NULL) {                                              \
            return ESP_ERR_INVALID_ARG;                                                \
        }                                                                              \
        nvs_handle_t h;                                                                \
        esp_err_t err = open_ns(ns, NVS_READONLY, &h);                                 \
        if (err != ESP_OK) {                                                           \
            return err;                                                                \
        }                                                                              \
        err = nvs_get_##suffix(h, key, out);                                           \
        nvs_close(h);                                                                  \
        return err;                                                                    \
    }                                                                                  \
    esp_err_t settings_set_##suffix(const char *ns, const char *key, type value)       \
    {                                                                                  \
        if (key == NULL) {                                                             \
            return ESP_ERR_INVALID_ARG;                                                \
        }                                                                              \
        nvs_handle_t h;                                                                \
        esp_err_t err = open_ns(ns, NVS_READWRITE, &h);                                \
        if (err != ESP_OK) {                                                           \
            return err;                                                                \
        }                                                                              \
        return finish_write(h, nvs_set_##suffix(h, key, value), ns, key);              \
    }

SETTINGS_SCALAR_ACCESSORS(u8, uint8_t)
SETTINGS_SCALAR_ACCESSORS(u16, uint16_t)
SETTINGS_SCALAR_ACCESSORS(u32, uint32_t)
SETTINGS_SCALAR_ACCESSORS(i64, int64_t)

esp_err_t settings_get_str(const char *ns, const char *key, char *out, size_t out_size)
{
    if (key == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = out_size;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        out[0] = '\0';
    }
    return err;
}

esp_err_t settings_set_str(const char *ns, const char *key, const char *value)
{
    if (key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    return finish_write(h, nvs_set_str(h, key, value), ns, key);
}

esp_err_t settings_get_blob(const char *ns, const char *key, void *out, size_t *len)
{
    if (key == NULL || out == NULL || len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_blob(h, key, out, len);
    nvs_close(h);
    return err;
}

esp_err_t settings_set_blob(const char *ns, const char *key, const void *data, size_t len)
{
    if (key == NULL || (data == NULL && len != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    return finish_write(h, nvs_set_blob(h, key, data, len), ns, key);
}

esp_err_t settings_erase_key(const char *ns, const char *key)
{
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    return finish_write(h, err, ns, key);
}

esp_err_t settings_erase_namespace(const char *ns)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    return finish_write(h, nvs_erase_all(h), ns, NULL);
}

/* ---- batch ------------------------------------------------------------------ */

struct settings_batch {
    nvs_handle_t h;
    esp_err_t first_err;
    char ns[16];
};

esp_err_t settings_batch_begin(const char *ns, settings_batch_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    struct settings_batch *b = calloc(1, sizeof(*b));
    if (b == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = open_ns(ns, NVS_READWRITE, &b->h);
    if (err != ESP_OK) {
        free(b);
        return err;
    }
    strlcpy(b->ns, ns, sizeof(b->ns));
    *out = b;
    return ESP_OK;
}

static esp_err_t batch_result(settings_batch_t b, esp_err_t err)
{
    if (err != ESP_OK && b->first_err == ESP_OK) {
        b->first_err = err;
    }
    return err;
}

esp_err_t settings_batch_set_str(settings_batch_t b, const char *key, const char *value)
{
    if (b == NULL || key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return batch_result(b, nvs_set_str(b->h, key, value));
}

esp_err_t settings_batch_set_u8(settings_batch_t b, const char *key, uint8_t value)
{
    if (b == NULL || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return batch_result(b, nvs_set_u8(b->h, key, value));
}

esp_err_t settings_batch_set_u16(settings_batch_t b, const char *key, uint16_t value)
{
    if (b == NULL || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return batch_result(b, nvs_set_u16(b->h, key, value));
}

esp_err_t settings_batch_set_u32(settings_batch_t b, const char *key, uint32_t value)
{
    if (b == NULL || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return batch_result(b, nvs_set_u32(b->h, key, value));
}

esp_err_t settings_batch_set_blob(settings_batch_t b, const char *key, const void *data, size_t len)
{
    if (b == NULL || key == NULL || (data == NULL && len != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    return batch_result(b, nvs_set_blob(b->h, key, data, len));
}

esp_err_t settings_batch_commit(settings_batch_t b)
{
    if (b == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = finish_write(b->h, b->first_err, b->ns, NULL);
    free(b);
    return err;
}

void settings_batch_abort(settings_batch_t b)
{
    if (b == NULL) {
        return;
    }
    nvs_close(b->h);
    free(b);
}

/* ---- factory reset ------------------------------------------------------------ */

static bool keep_on_reset(const char *ns, const char *key)
{
    for (size_t i = 0; i < sizeof(s_keep_on_reset) / sizeof(s_keep_on_reset[0]); i++) {
        if (strcmp(s_keep_on_reset[i].ns, ns) == 0 && strcmp(s_keep_on_reset[i].key, key) == 0) {
            return true;
        }
    }
    return false;
}

/* Erase every key of one namespace except the listed device state. Kept keys
 * are never read, erased or rewritten, so neither a power cut nor a
 * concurrent write (e.g. the receiver-flash journal) can lose or roll them
 * back. Each key is erased on its own; the first error is returned. */
static esp_err_t erase_keeping(const char *ns)
{
    enum { BATCH = 16 };
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    esp_err_t first_err = ESP_OK;
    bool more = true;
    while (more && first_err == ESP_OK) {
        /* Collect names first and erase afterwards: an iterator is not
         * guaranteed to survive changes to the namespace it walks. */
        char keys[BATCH][NVS_KEY_NAME_MAX_SIZE];
        size_t n = 0;
        more = false;
        nvs_iterator_t it = NULL;
        esp_err_t res = nvs_entry_find_in_handle(h, NVS_TYPE_ANY, &it);
        while (res == ESP_OK) {
            nvs_entry_info_t info;
            if (nvs_entry_info(it, &info) == ESP_OK && !keep_on_reset(ns, info.key)) {
                if (n == BATCH) {
                    more = true;
                    break;
                }
                strlcpy(keys[n++], info.key, sizeof(keys[0]));
            }
            res = nvs_entry_next(&it);
        }
        nvs_release_iterator(it);
        if (res != ESP_OK && res != ESP_ERR_NVS_NOT_FOUND) {
            first_err = res;
        }

        for (size_t i = 0; i < n; i++) {
            esp_err_t e = nvs_erase_key(h, keys[i]);
            if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGE(TAG, "factory reset: cannot erase %s/%s: %s", ns, keys[i], esp_err_to_name(e));
                if (first_err == ESP_OK) {
                    first_err = e;
                }
            }
        }
    }

    err = nvs_commit(h);
    nvs_close(h);
    return (first_err != ESP_OK) ? first_err : err;
}

esp_err_t settings_factory_reset(void)
{
    if (!s_available) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t first_err = ESP_OK;
    const size_t n_fixed = sizeof(s_user_namespaces) / sizeof(s_user_namespaces[0]);

    for (size_t i = 0; i < n_fixed + CASTER_NAMESPACE_SLOTS; i++) {
        char ns[16];
        if (i < n_fixed) {
            strlcpy(ns, s_user_namespaces[i], sizeof(ns));
        } else {
            snprintf(ns, sizeof(ns), SETTINGS_NS_CASTER_FMT, (unsigned)(i - n_fixed));
        }
        esp_err_t err = erase_keeping(ns);
        if (err != ESP_OK && first_err == ESP_OK) {
            first_err = err;
        }
    }

    ESP_LOGW(TAG, "factory reset: user settings erased");
    (void)esp_event_post(SETTINGS_EVENT, SETTINGS_EVENT_FACTORY_RESET, NULL, 0, 0);
    return first_err;
}
