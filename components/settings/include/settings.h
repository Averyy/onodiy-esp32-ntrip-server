/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Persistent settings on NVS.
 *
 * Thin, typed access to the namespaces and keys declared in settings_schema.h,
 * plus schema versioning/migration and change notification. Components own the
 * meaning of their settings; this component owns storage and versioning.
 * Multi-field records are one blob each (see settings_codec.h).
 *
 * Change events are advisory: SETTINGS_EVENT_CHANGED tells the UI that
 * something changed, but owners apply new values through their own setters
 * (e.g. ntrip_server_set_config), never by reacting to the event. Events can
 * be dropped when the event queue is full.
 *
 * Only settings post it: what a person set (from any page, the API or the
 * BOOT button) and what the station changes in them itself (the receiver
 * profile at a position lock, for one). Every open page reloads its data on it.
 * Device state the station stores on its own (counters, history, detected
 * receiver facts, timers, journals: the keys settings_schema.h marks
 * "device state") goes through the settings_set_state_* writers, which post
 * nothing; pages read it from the status stream or their own polls.
 *
 * Thread safety: all functions may be called from any task (NVS is internally
 * locked). Do not call from a task whose stack is in PSRAM: NVS writes can
 * disable the flash cache.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "nvs.h"               /* the getters return NVS error codes (ESP_ERR_NVS_*) */
#include "settings_schema.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Posted on the default event loop after a successful write. */
ESP_EVENT_DECLARE_BASE(SETTINGS_EVENT);

/** SETTINGS_EVENT ids. */
typedef enum {
    SETTINGS_EVENT_CHANGED,      /**< data: settings_changed_t */
    SETTINGS_EVENT_FACTORY_RESET,/**< user settings erased; reboot follows */
} settings_event_t;

/** Payload of SETTINGS_EVENT_CHANGED. */
typedef struct {
    char ns[16];                 /**< namespace that changed */
    char key[16];                /**< key that changed ("" = several keys) */
} settings_changed_t;

/**
 * Check the stored schema version and migrate older data in place.
 * Requires nvs_flash_init() and the default event loop.
 *
 * A stored version newer than SETTINGS_SCHEMA_VERSION (after an app rollback)
 * is left untouched: keys are read as-is and nothing is deleted.
 *
 * Settings become available as soon as the stored version could be read.
 * Writing the new version stamp is not a condition: if it fails (e.g. NVS
 * full) it is logged and retried at the next boot, and reads keep working.
 *
 * If NVS could not be initialised or read (degraded boot), this fails and
 * settings_is_available() stays false: every getter then returns an error and
 * components run on their Kconfig defaults without persisting anything.
 */
esp_err_t settings_init(void);

/** True once settings_init() could read NVS (settings usable this boot). */
bool settings_is_available(void);

/** Schema version found in NVS at boot (0 = fresh device). */
uint16_t settings_stored_schema(void);

/** Read a string. ESP_ERR_NVS_NOT_FOUND if absent; out is always terminated. */
esp_err_t settings_get_str(const char *ns, const char *key, char *out, size_t out_size);
/** Write a string and post SETTINGS_EVENT_CHANGED. */
esp_err_t settings_set_str(const char *ns, const char *key, const char *value);

esp_err_t settings_get_u8(const char *ns, const char *key, uint8_t *out);
esp_err_t settings_set_u8(const char *ns, const char *key, uint8_t value);
esp_err_t settings_get_u16(const char *ns, const char *key, uint16_t *out);
esp_err_t settings_set_u16(const char *ns, const char *key, uint16_t value);
esp_err_t settings_get_u32(const char *ns, const char *key, uint32_t *out);
esp_err_t settings_set_u32(const char *ns, const char *key, uint32_t value);
esp_err_t settings_get_i64(const char *ns, const char *key, int64_t *out);
esp_err_t settings_set_i64(const char *ns, const char *key, int64_t value);

/**
 * Read a blob. On entry *len is the buffer size; on success it is the stored
 * size. ESP_ERR_NVS_INVALID_LENGTH if the buffer is too small.
 */
esp_err_t settings_get_blob(const char *ns, const char *key, void *out, size_t *len);
/** Write a blob (replaced as a unit) and post SETTINGS_EVENT_CHANGED. */
esp_err_t settings_set_blob(const char *ns, const char *key, const void *data, size_t len);

/** Remove one key (no error if absent). */
esp_err_t settings_erase_key(const char *ns, const char *key);

/**
 * Device state, not a setting anyone changes (the keys settings_schema.h
 * marks "device state": the ledger every minute, boot history, the detected
 * receiver, the receiver maintenance copy, ...): as settings_set_* and
 * settings_erase_key, but without SETTINGS_EVENT_CHANGED. Pages reload their
 * data on that event (and warn about changes made elsewhere while a form is
 * being edited), so a write the station makes on its own must not post it.
 */
esp_err_t settings_set_state_str(const char *ns, const char *key, const char *value);
esp_err_t settings_set_state_u8(const char *ns, const char *key, uint8_t value);
esp_err_t settings_set_state_u16(const char *ns, const char *key, uint16_t value);
esp_err_t settings_set_state_u32(const char *ns, const char *key, uint32_t value);
esp_err_t settings_set_state_i64(const char *ns, const char *key, int64_t value);
esp_err_t settings_set_state_blob(const char *ns, const char *key, const void *data, size_t len);
esp_err_t settings_erase_state_key(const char *ns, const char *key);

/** Remove every key in one namespace. */
esp_err_t settings_erase_namespace(const char *ns);

/**
 * Batch write: several keys of one namespace, one commit, one
 * SETTINGS_EVENT_CHANGED (key ""). Not atomic across keys (NVS writes each key
 * as it goes): anything that must change together belongs in one blob.
 */
typedef struct settings_batch *settings_batch_t;

esp_err_t settings_batch_begin(const char *ns, settings_batch_t *out);
esp_err_t settings_batch_set_str(settings_batch_t b, const char *key, const char *value);
esp_err_t settings_batch_set_u8(settings_batch_t b, const char *key, uint8_t value);
esp_err_t settings_batch_set_u16(settings_batch_t b, const char *key, uint16_t value);
esp_err_t settings_batch_set_u32(settings_batch_t b, const char *key, uint32_t value);
esp_err_t settings_batch_set_blob(settings_batch_t b, const char *key, const void *data, size_t len);
/** Commit, close and post one event. Returns the first error of the batch. */
esp_err_t settings_batch_commit(settings_batch_t b);
/** As settings_batch_commit, without the event: a batch of device state only. */
esp_err_t settings_batch_commit_state(settings_batch_t b);
/** Close without posting an event (writes already made stay written). NULL is ignored. */
void settings_batch_abort(settings_batch_t b);

/**
 * Erase all user settings (Wi-Fi, admin password, casters, receiver profile,
 * update policy, web) and post SETTINGS_EVENT_FACTORY_RESET. Device state
 * marked "survives factory reset" in settings_schema.h (schema version,
 * receiver flash journal, last receiver identity, app trial/rollback records,
 * boot history, availability ledger) is kept: the reset erases the other keys
 * one by one and never touches the kept ones, so a power cut during the reset
 * cannot lose them. Caller reboots afterwards.
 */
esp_err_t settings_factory_reset(void);

#ifdef __cplusplus
}
#endif
