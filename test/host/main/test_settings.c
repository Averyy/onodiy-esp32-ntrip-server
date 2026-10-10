/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * settings.c on IDF's emulated flash (linux target). Every test starts on a
 * fresh flash image (all 0xFF, deleted again at the end).
 */
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_private/partition_linux.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "settings.h"
#include "unity.h"

static void nvs_up(void)
{
    esp_err_t err = esp_event_loop_create_default();   /* settings posts change events */
    TEST_ASSERT_TRUE(err == ESP_OK || err == ESP_ERR_INVALID_STATE);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_flash_init());
}

static void nvs_down(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, nvs_flash_deinit());
    esp_partition_get_file_mmap_ctrl_input()->remove_dump = true;
    TEST_ASSERT_EQUAL(ESP_OK, esp_partition_file_munmap());
}

static void raw_set_schema(uint16_t v)
{
    nvs_handle_t h;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open(SETTINGS_NS_SYS, NVS_READWRITE, &h));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u16(h, SETTINGS_KEY_SCHEMA, v));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(h));
    nvs_close(h);
}

/* The namespace holds exactly these keys (NULL-terminated list). */
static void expect_keys(const char *ns, const char *const *keys)
{
    size_t expected = 0;
    while (keys[expected] != NULL) {
        expected++;
    }
    size_t found = 0;
    nvs_iterator_t it = NULL;
    esp_err_t res = nvs_entry_find(NVS_DEFAULT_PART_NAME, ns, NVS_TYPE_ANY, &it);
    while (res == ESP_OK) {
        nvs_entry_info_t info;
        TEST_ASSERT_EQUAL(ESP_OK, nvs_entry_info(it, &info));
        bool listed = false;
        for (size_t i = 0; i < expected; i++) {
            listed = listed || strcmp(keys[i], info.key) == 0;
        }
        TEST_ASSERT_TRUE_MESSAGE(listed, info.key);
        found++;
        res = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_NVS_NOT_FOUND, res, ns);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(expected, found, ns);
}

TEST_CASE("settings without NVS run degraded", "[settings]")
{
    /* nvs_flash_init() not called: nothing is readable. */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, settings_init());
    TEST_ASSERT_FALSE(settings_is_available());
    uint8_t v;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, settings_get_u8(SETTINGS_NS_WEB, SETTINGS_KEY_WEB_PUBLIC, &v));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, settings_set_u8(SETTINGS_NS_WEB, SETTINGS_KEY_WEB_PUBLIC, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, settings_factory_reset());
}

TEST_CASE("settings: a fresh device reads as schema 0 and gets stamped", "[settings]")
{
    nvs_up();
    TEST_ASSERT_EQUAL(ESP_OK, settings_init());
    TEST_ASSERT_TRUE(settings_is_available());
    TEST_ASSERT_EQUAL_UINT16(0, settings_stored_schema());
    uint16_t v = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_get_u16(SETTINGS_NS_SYS, SETTINGS_KEY_SCHEMA, &v));
    TEST_ASSERT_EQUAL_UINT16(SETTINGS_SCHEMA_VERSION, v);

    TEST_ASSERT_EQUAL(ESP_OK, settings_init());     /* next boot */
    TEST_ASSERT_EQUAL_UINT16(SETTINGS_SCHEMA_VERSION, settings_stored_schema());
    nvs_down();
}

TEST_CASE("settings: a newer stored schema is left as-is", "[settings]")
{
    nvs_up();
    raw_set_schema(SETTINGS_SCHEMA_VERSION + 1);
    TEST_ASSERT_EQUAL(ESP_OK, settings_init());
    TEST_ASSERT_TRUE(settings_is_available());
    TEST_ASSERT_EQUAL_UINT16(SETTINGS_SCHEMA_VERSION + 1, settings_stored_schema());
    uint16_t v = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_get_u16(SETTINGS_NS_SYS, SETTINGS_KEY_SCHEMA, &v));
    TEST_ASSERT_EQUAL_UINT16(SETTINGS_SCHEMA_VERSION + 1, v);
    nvs_down();
}

TEST_CASE("settings stay readable when the version stamp cannot be written", "[settings]")
{
    nvs_up();
    nvs_handle_t h;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("fill", NVS_READWRITE, &h));
    esp_err_t err = ESP_OK;
    for (unsigned i = 0; i < 4096 && err == ESP_OK; i++) {
        char key[16];
        snprintf(key, sizeof(key), "k%u", i);
        err = nvs_set_u32(h, key, 42);
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_ENOUGH_SPACE, err);
    nvs_close(h);

    TEST_ASSERT_EQUAL(ESP_OK, settings_init());
    TEST_ASSERT_TRUE(settings_is_available());
    uint32_t v = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_get_u32("fill", "k0", &v));
    TEST_ASSERT_EQUAL_UINT32(42, v);
    uint16_t schema;
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, settings_get_u16(SETTINGS_NS_SYS, SETTINGS_KEY_SCHEMA, &schema));
    nvs_down();
}

TEST_CASE("factory reset keeps exactly the device state", "[settings]")
{
    nvs_up();
    TEST_ASSERT_EQUAL(ESP_OK, settings_init());

    const uint8_t job[24] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    const uint8_t trial[12] = { 0xA1, 0xA2, 0xA3 };
    const uint8_t bad[8] = { 0xB1 };
    const uint8_t boots[16] = { 1, 1 };
    const uint8_t blob[20] = { 7 };

    /* User configuration. */
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_str(SETTINGS_NS_SYS, SETTINGS_KEY_HOSTNAME, "station"));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_u8(SETTINGS_NS_SYS, SETTINGS_KEY_SETUP_DONE, 1));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_blob(SETTINGS_NS_SYS, SETTINGS_KEY_ADMIN_HASH, blob, sizeof(blob)));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_str(SETTINGS_NS_WIFI, SETTINGS_KEY_WIFI_SSID, "home"));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_str(SETTINGS_NS_WIFI, SETTINGS_KEY_WIFI_PASS, "secret"));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_str(SETTINGS_NS_WIFI, SETTINGS_KEY_WIFI_PEND_SSID, "next-site"));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_str(SETTINGS_NS_WIFI, SETTINGS_KEY_WIFI_PEND_PASS, "secret2"));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_RECOVERY, 2));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_u32(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_BAUD, 115200));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_blob(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_PROFILE, blob, sizeof(blob)));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_u32(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_APPLIED, 0x1234));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_str(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_APPLIED_FW, "R4.10Build18975"));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_RTCM1230_OK, 1));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_str(SETTINGS_NS_OTA, SETTINGS_KEY_OTA_REPO, "owner/name"));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_str(SETTINGS_NS_OTA, SETTINGS_KEY_OTA_ETAG, "\"etag\""));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_i64(SETTINGS_NS_OTA, SETTINGS_KEY_OTA_LAST_CHECK, 1791590400));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_u8(SETTINGS_NS_WEB, SETTINGS_KEY_WEB_PUBLIC, 1));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_blob("cst0", SETTINGS_KEY_CST_CFG, blob, sizeof(blob)));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_blob("cst0", SETTINGS_KEY_CST_BACKOFF, blob, sizeof(blob)));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_blob("cst0", SETTINGS_KEY_CST_CA_PEM, blob, sizeof(blob)));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_blob("cst7", SETTINGS_KEY_CST_CFG, blob, sizeof(blob)));
    /* More keys than one erase batch holds. */
    for (unsigned i = 0; i < 40; i++) {
        char key[16];
        snprintf(key, sizeof(key), "extra%u", i);
        TEST_ASSERT_EQUAL(ESP_OK, settings_set_u8(SETTINGS_NS_WIFI, key, (uint8_t)i));
    }

    /* Device state. */
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_str(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_MODEL, "UM982"));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_str(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_FW, "R4.10Build18975"));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_blob(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_FLASH_JOB, job, sizeof(job)));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_blob(SETTINGS_NS_OTA, SETTINGS_KEY_OTA_TRIAL, trial, sizeof(trial)));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_blob(SETTINGS_NS_OTA, SETTINGS_KEY_OTA_BAD_VER, bad, sizeof(bad)));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_blob(SETTINGS_NS_LIFE, SETTINGS_KEY_BOOT_HISTORY, boots, sizeof(boots)));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_i64(SETTINGS_NS_LEDGER, SETTINGS_KEY_LEDGER_ALIVE, 1791590400));

    TEST_ASSERT_EQUAL(ESP_OK, settings_factory_reset());

    static const char *const sys[] = { SETTINGS_KEY_SCHEMA, NULL };
    static const char *const none[] = { NULL };
    static const char *const gnss[] = { SETTINGS_KEY_GNSS_MODEL, SETTINGS_KEY_GNSS_FW, SETTINGS_KEY_GNSS_FLASH_JOB, NULL };
    static const char *const ota[] = { SETTINGS_KEY_OTA_TRIAL, SETTINGS_KEY_OTA_BAD_VER, NULL };
    static const char *const life[] = { SETTINGS_KEY_BOOT_HISTORY, NULL };
    static const char *const ledger[] = { SETTINGS_KEY_LEDGER_ALIVE, NULL };
    expect_keys(SETTINGS_NS_SYS, sys);
    expect_keys(SETTINGS_NS_WIFI, none);
    expect_keys(SETTINGS_NS_GNSS, gnss);
    expect_keys(SETTINGS_NS_OTA, ota);
    expect_keys(SETTINGS_NS_WEB, none);
    expect_keys("cst0", none);
    expect_keys("cst7", none);
    expect_keys(SETTINGS_NS_LIFE, life);
    expect_keys(SETTINGS_NS_LEDGER, ledger);

    /* Kept values are untouched. */
    uint8_t buf[32];
    size_t len = sizeof(buf);
    TEST_ASSERT_EQUAL(ESP_OK, settings_get_blob(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_FLASH_JOB, buf, &len));
    TEST_ASSERT_EQUAL_size_t(sizeof(job), len);
    TEST_ASSERT_EQUAL_MEMORY(job, buf, sizeof(job));
    len = sizeof(buf);
    TEST_ASSERT_EQUAL(ESP_OK, settings_get_blob(SETTINGS_NS_OTA, SETTINGS_KEY_OTA_TRIAL, buf, &len));
    TEST_ASSERT_EQUAL_MEMORY(trial, buf, sizeof(trial));
    char str[24];
    TEST_ASSERT_EQUAL(ESP_OK, settings_get_str(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_FW, str, sizeof(str)));
    TEST_ASSERT_EQUAL_STRING("R4.10Build18975", str);
    uint16_t schema = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_get_u16(SETTINGS_NS_SYS, SETTINGS_KEY_SCHEMA, &schema));
    TEST_ASSERT_EQUAL_UINT16(SETTINGS_SCHEMA_VERSION, schema);
    nvs_down();
}

TEST_CASE("factory reset keeps a newer schema version", "[settings]")
{
    /* A rolled-back app resets: the kept records are still in the newer
     * format, so the stamp must stay and the newer app must not migrate again. */
    nvs_up();
    raw_set_schema(SETTINGS_SCHEMA_VERSION + 1);
    TEST_ASSERT_EQUAL(ESP_OK, settings_init());
    TEST_ASSERT_EQUAL(ESP_OK, settings_factory_reset());
    TEST_ASSERT_EQUAL(ESP_OK, settings_init());     /* next boot */
    TEST_ASSERT_EQUAL_UINT16(SETTINGS_SCHEMA_VERSION + 1, settings_stored_schema());
    nvs_down();
}

TEST_CASE("settings batch reports the first error and writes the rest", "[settings]")
{
    nvs_up();
    TEST_ASSERT_EQUAL(ESP_OK, settings_init());
    settings_batch_t b = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, settings_batch_begin(SETTINGS_NS_WIFI, &b));
    TEST_ASSERT_EQUAL(ESP_OK, settings_batch_set_str(b, SETTINGS_KEY_WIFI_SSID, "home"));
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_KEY_TOO_LONG, settings_batch_set_u8(b, "a_key_name_too_long", 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_batch_set_str(b, SETTINGS_KEY_WIFI_PASS, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, settings_batch_set_u16(b, "port", 2101));
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_KEY_TOO_LONG, settings_batch_commit(b));

    /* Not atomic by design: the good keys are written. */
    char ssid[16];
    TEST_ASSERT_EQUAL(ESP_OK, settings_get_str(SETTINGS_NS_WIFI, SETTINGS_KEY_WIFI_SSID, ssid, sizeof(ssid)));
    TEST_ASSERT_EQUAL_STRING("home", ssid);
    uint16_t port = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_get_u16(SETTINGS_NS_WIFI, "port", &port));
    TEST_ASSERT_EQUAL_UINT16(2101, port);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_batch_begin(SETTINGS_NS_WIFI, NULL));
    settings_batch_abort(NULL);
    nvs_down();
}

TEST_CASE("settings: the recovery mode is a user setting a factory reset takes back to full", "[settings]")
{
    nvs_up();
    TEST_ASSERT_EQUAL(ESP_OK, settings_init());
    uint8_t v = 0xFF;
    /* not stored: the reader's default (full) applies */
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, settings_get_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_RECOVERY, &v));
    TEST_ASSERT_EQUAL(ESP_OK, settings_set_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_RECOVERY, 1));
    TEST_ASSERT_EQUAL(ESP_OK, settings_get_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_RECOVERY, &v));
    TEST_ASSERT_EQUAL_UINT8(1, v);
    TEST_ASSERT_TRUE(strlen(SETTINGS_KEY_GNSS_RECOVERY) <= 15);
    TEST_ASSERT_TRUE(strlen(SETTINGS_KEY_WIFI_PEND_SSID) <= 15 && strlen(SETTINGS_KEY_WIFI_PEND_PASS) <= 15);
    TEST_ASSERT_EQUAL(ESP_OK, settings_factory_reset());
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, settings_get_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_RECOVERY, &v));
    nvs_down();
}
