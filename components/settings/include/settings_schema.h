/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Persistent settings schema: every NVS namespace and key the firmware uses.
 *
 * Rules:
 *  - NVS limits namespace and key names to 15 characters.
 *  - Keys are never reused with a different type. Retire a key and add a new one.
 *  - Records with more than one field (caster slot, profile, position, journal)
 *    are ONE blob written as a unit, serialised with settings_codec.h
 *    (explicit little-endian, leading version byte). Never store raw C structs:
 *    their layout depends on the compiler and padding.
 *  - Migrations copy data forward to new keys and never delete retired keys
 *    for at least two releases, so an app rollback still finds its data.
 *  - Bump SETTINGS_SCHEMA_VERSION only together with a migration step in
 *    settings.c. Until the first public release the layout may still change
 *    without a migration (no device stores v1 data yet).
 *  - Secrets (passwords) are write-only through the web API: readable on the
 *    device, never echoed back to a browser.
 *  - Keys marked "survives factory reset" are device state, not user config.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Current schema version, stored as SETTINGS_KEY_SCHEMA in SETTINGS_NS_SYS. */
#define SETTINGS_SCHEMA_VERSION 1

/* ---- "sys": schema and device identity ---------------------------------- */
#define SETTINGS_NS_SYS             "sys"
#define SETTINGS_KEY_SCHEMA         "schema"      /**< u16: schema version; survives factory reset */
#define SETTINGS_KEY_HOSTNAME       "hostname"    /**< str: DHCP hostname */
#define SETTINGS_KEY_ADMIN_HASH     "admin_hash"  /**< blob: v1 {iterations u32, salt[16], PBKDF2-HMAC-SHA256[32]} */
#define SETTINGS_KEY_SETUP_DONE     "setup_done"  /**< u8: first-run wizard completed */

/* ---- "wifi": station credentials ----------------------------------------- */
#define SETTINGS_NS_WIFI            "wifi"
#define SETTINGS_KEY_WIFI_SSID      "ssid"        /**< str (<= 32) */
#define SETTINGS_KEY_WIFI_PASS      "pass"        /**< str (<= 64), secret */
#define SETTINGS_KEY_WIFI_AP_PASS   "ap_pass"     /**< str: setup AP password, empty = open */

/* ---- "gnss": receiver configuration and state ----------------------------- */
#define SETTINGS_NS_GNSS            "gnss"
#define SETTINGS_KEY_GNSS_BAUD      "baud"        /**< u32: link baud rate last detected/set */
#define SETTINGS_KEY_GNSS_PROFILE   "profile"     /**< blob: um98x_profile_encode() */
#define SETTINGS_KEY_GNSS_APPLIED   "applied_hash" /**< u32: um98x_profile_hash() of the profile last applied */
#define SETTINGS_KEY_GNSS_AUTOCFG   "autocfg"     /**< u8: apply profile automatically on boot */
#define SETTINGS_KEY_GNSS_MODEL     "model"       /**< str: last detected model; survives factory reset */
#define SETTINGS_KEY_GNSS_FW        "fw"          /**< str: last detected firmware; survives factory reset */
#define SETTINGS_KEY_GNSS_BASE_POS  "base_pos"    /**< blob: locked antenna position (um98x_base_position_t, encoded) */
#define SETTINGS_KEY_GNSS_FLASH_JOB "flash_job"   /**< blob: open receiver-flash journal; survives factory reset */

/* ---- "cst0".."cst7": one namespace per caster slot ------------------------ */
#define SETTINGS_NS_CASTER_FMT      "cst%u"
#define SETTINGS_KEY_CST_CFG        "cfg"         /**< blob: whole slot config, encoded (password inside, secret) */

/* ---- "ota": firmware update policy and state ------------------------------- */
#define SETTINGS_NS_OTA             "ota"
#define SETTINGS_KEY_OTA_REPO       "repo"        /**< str: "owner/name" on GitHub */
#define SETTINGS_KEY_OTA_CHANNEL    "channel"     /**< u8: 0 = stable, 1 = pre-release */
#define SETTINGS_KEY_OTA_AUTO_APP   "auto_app"    /**< u8: install app updates automatically */
#define SETTINGS_KEY_OTA_AUTO_GNSS  "auto_gnss"   /**< u8: flash receiver updates automatically */
#define SETTINGS_KEY_OTA_INTERVAL   "interval_h"  /**< u16: hours between checks */
#define SETTINGS_KEY_OTA_TRIAL      "trial"       /**< blob: app trial {version, baseline, stage, started, boots, crashes}; survives factory reset */
#define SETTINGS_KEY_OTA_BAD_VER    "bad_ver"     /**< blob: versions rolled back (never auto-installed again); survives factory reset */
#define SETTINGS_KEY_OTA_ETAG       "etag"        /**< str: ETag of the last manifest fetched */
#define SETTINGS_KEY_OTA_GNSS_LAST  "gnss_last"   /**< i64: unix time of the last automatic receiver flash */

/* ---- "life": boot history (crash counter and safe-mode state live in RTC
 * memory, so a power cycle always retries a normal boot) ---------------------- */
#define SETTINGS_NS_LIFE            "life"        /**< whole namespace survives factory reset */
#define SETTINGS_KEY_BOOT_HISTORY   "boots"       /**< blob: last 16 boots (reset class, raw reason, planned-reboot reason, boot no., app version) */

/* ---- "ledger": availability accounting ------------------------------------- */
#define SETTINGS_NS_LEDGER          "ledger"      /**< whole namespace survives factory reset */
#define SETTINGS_KEY_LEDGER_TODAY   "today"       /**< blob: running counters of the current UTC day */
#define SETTINGS_KEY_LEDGER_HIST    "hist"        /**< blob: last 30 UTC days (availability per slot, reconnects, overflows) */
#define SETTINGS_KEY_LEDGER_ALIVE   "alive"       /**< i64: last "still running" unix time (power-off gaps count as downtime) */

/* ---- "web" ------------------------------------------------------------------ */
#define SETTINGS_NS_WEB             "web"
#define SETTINGS_KEY_WEB_PUBLIC     "public_status" /**< u8: minimal status readable without login */

#ifdef __cplusplus
}
#endif
