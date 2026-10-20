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
 *  - Keys marked "device state" are what the station stores on its own
 *    (counters, history, detected receiver facts, timers, journals): written
 *    with the settings_set_state_* writers, which post no
 *    SETTINGS_EVENT_CHANGED (every open page reloads its data on that event).
 *    Every other key is a setting: written with settings_set_* and announced.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Current schema version, stored as SETTINGS_KEY_SCHEMA in SETTINGS_NS_SYS. */
#define SETTINGS_SCHEMA_VERSION 1

/* ---- "sys": schema and device identity ---------------------------------- */
#define SETTINGS_NS_SYS             "sys"
#define SETTINGS_KEY_SCHEMA         "schema"      /**< u16, device state: schema version; survives factory reset */
#define SETTINGS_KEY_HOSTNAME       "hostname"    /**< str: DHCP hostname */
#define SETTINGS_KEY_ADMIN_HASH     "admin_hash"  /**< blob: v1 {iterations u32, salt[16], PBKDF2-HMAC-SHA256[32]} */
#define SETTINGS_KEY_SETUP_DONE     "setup_done"  /**< u8: first-run wizard completed */
#define SETTINGS_KEY_SETUP_STEP     "setup_step"  /**< u8: the setup wizard's next step (1 wifi, 2 reception, 3 onocoy, 4 casters; 0 or missing = none); erased by POST /api/setup/done */

/* ---- "wifi": station credentials ----------------------------------------- */
#define SETTINGS_NS_WIFI            "wifi"
#define SETTINGS_KEY_WIFI_SSID      "ssid"        /**< str (<= 32): the station's credentials (confirmed by an IP, or entered at first setup) */
#define SETTINGS_KEY_WIFI_PASS      "pass"        /**< str (<= 64), secret */
#define SETTINGS_KEY_WIFI_PEND_SSID "pend_ssid"   /**< str (<= 32): changed credentials not confirmed yet (tried first at boot; replace ssid/pass once they get an IP) */
#define SETTINGS_KEY_WIFI_PEND_PASS "pend_pass"   /**< str (<= 64), secret: written with pend_ssid in one batch */
#define SETTINGS_KEY_WIFI_AP_PASS   "ap_pass"     /**< str: setup AP password, empty = open */

/* ---- "gnss": receiver configuration and state ----------------------------- */
#define SETTINGS_NS_GNSS            "gnss"
#define SETTINGS_KEY_GNSS_BAUD      "baud"        /**< u32, device state: link baud rate last detected/set */
#define SETTINGS_KEY_GNSS_PROFILE   "profile"     /**< blob: um98x_profile_encode() */
#define SETTINGS_KEY_GNSS_APPLIED   "applied_hash" /**< u32, device state: um98x_ctl_applied_hash() of the profile last applied: its um98x_profile_hash() on COM1, mixed with the port on COM2/COM3 */
#define SETTINGS_KEY_GNSS_APPLIED_FW "applied_fw" /**< str (<= 47 chars, like every receiver firmware string), device state: receiver firmware at the last successful apply (written with applied_hash in one batch) */
#define SETTINGS_KEY_GNSS_RTCM1230_OK "rtcm1230_ok" /**< u8, device state: the receiver accepted (1) or rejected (0) the RTCM1230 output at the last successful apply (same batch as applied_hash; missing = not applied) */
#define SETTINGS_KEY_GNSS_AUTOCFG   "autocfg"     /**< u8: apply the profile when the receiver drifts from it; 0 = alert only and the health ladder's reconfigure step is refused (a receiver update always re-applies) */
#define SETTINGS_KEY_GNSS_RECOVERY  "recovery"    /**< u8: automatic receiver recovery (the health monitor's GNSS ladder): 0 = full (default), 1 = no ESP32 reboot, 2 = alerts only; any other value reads as 0 */
#define SETTINGS_KEY_GNSS_MODEL     "model"       /**< str, device state: last detected model; survives factory reset */
#define SETTINGS_KEY_GNSS_FW        "fw"          /**< str, device state: last detected firmware; survives factory reset */
#define SETTINGS_KEY_GNSS_BASE_POS  "base_pos"    /**< blob: locked antenna position (um98x_base_position_t, encoded) */
#define SETTINGS_KEY_GNSS_FLASH_JOB "flash_job"   /**< blob, device state: open receiver-flash journal (versioned record, see the flasher); survives factory reset */
#define SETTINGS_KEY_GNSS_BENCH_RACE "bench_race" /**< u8, device state: bench builds only: 1 = the next boot streams the receiver's stop key from its earliest point (one-shot, erased before it runs) */
#define SETTINGS_KEY_GNSS_SG_OK     "sg_ok"       /**< u16, device state: (master << 8) | slave of the signal group the receiver accepted at the last successful apply (same batch as applied_hash): where a refused group goes back to when the session's own read-back is unknown */
#define SETTINGS_KEY_GNSS_SG_TEST   "sg_test"     /**< blob, device state: the signal-group comparison's record (sg_test_core.h, versioned), written at each transition, every 5 min while measuring, every 10 min while deciding and before a planned restart; the last test's record stays (its results are shown after a restart); erased by factory reset */
#define SETTINGS_KEY_GNSS_MAINT     "maint"       /**< blob, device state: receiver maintenance's stored copy while it is on (station_core.h stn_maint_store_*, versioned: the image stamp, the time left at most, the chosen duration, the end by the clock), so that a reset by the ESP32's EN pin, which loses the RTC memory, resumes it; written at entry, at each extension and every minute, erased when it ends; erased by factory reset */
#define SETTINGS_KEY_GNSS_HOLD      "hold"      /**< u8, device state: 1 = this device's first receiver configuration is held (only queries reach the receiver until the user applies); written once by a build with CONFIG_STATION_HOLD_FIRST_APPLY on a device whose profile was never applied (with gnss/autocfg = 0, and gnss/recovery = 2 if unset, in one batch); erased by the first successful apply, by gnss/autocfg turned on, by factory reset */
#define SETTINGS_KEY_GNSS_WIRING    "wiring"      /**< blob (a setting): how the receiver is wired, board_wiring_encode() (v1 {u8 version = 1, u8 rx GPIO, u8 tx GPIO, u8 port 1-3}); missing = the build's pins and port; read once at start (board_init), written only by a user save (PUT /api/system), erased when the build's own is saved; survives factory reset (it describes the hardware) */

/* ---- "cst0".."cst7": one namespace per caster slot ------------------------ */
#define SETTINGS_NS_CASTER_FMT      "cst%u"
#define SETTINGS_KEY_CST_CFG        "cfg"         /**< blob: whole slot config, encoded (password inside, secret) */
#define SETTINGS_KEY_CST_BACKOFF    "backoff"     /**< blob, device state: long backoff state {class, attempt, remaining s, hourly mode} and last-good credential hash */
#define SETTINGS_KEY_CST_CA_PEM     "ca_pem"      /**< blob: custom CA certificate (PEM) for a TLS slot */

/* ---- "ota": firmware update state (every install is user-initiated; the
 * daily check interval is a build-time constant) ----------------------------- */
#define SETTINGS_NS_OTA             "ota"
#define SETTINGS_KEY_OTA_REPO       "repo"        /**< str: GitHub repository override "owner/name"; used only by builds that configure a manifest key (CONFIG_FW_UPDATE_MANIFEST_KEY_1/_2), every other build fetches from its Kconfig repository only */
#define SETTINGS_KEY_OTA_TRIAL      "trial"       /**< blob, device state: app trial {version, baseline, stage, started, boots, crashes}; survives factory reset */
#define SETTINGS_KEY_OTA_BAD_VER    "bad_ver"     /**< blob, device state: versions rolled back (never offered by the update banner); survives factory reset */
#define SETTINGS_KEY_OTA_ETAG       "etag"        /**< str, device state: ETag of the last manifest fetched (written only when it changes) */
#define SETTINGS_KEY_OTA_MANIFEST   "manifest"    /**< blob, device state: the last manifest accepted, as fetched, after its repository and its ETag, each ended by a NUL (written only when it changes), so a restart keeps what the last check found */
#define SETTINGS_KEY_OTA_LAST_CHECK "last_check"  /**< i64, device state: unix time of the last successful update check (written once per check), so reboots add no checks */

/* ---- "life": boot history and image generation (crash counter and
 * safe-mode state live in RTC memory, so a power cycle always retries a
 * normal boot) ---------------------------------------------------------------- */
#define SETTINGS_NS_LIFE            "life"        /**< whole namespace survives factory reset */
#define SETTINGS_KEY_BOOT_HISTORY   "boots"       /**< blob, device state: last 16 boots (reset class, raw reason, planned-reboot reason, boot no., previous session uptime, app version) */
#define SETTINGS_KEY_BOOT_GEN       "gen"         /**< blob, device state: image generation {u8 version, u32 generation, u32 image id} stamped into the RTC blocks; written only when another app image boots */
#define SETTINGS_KEY_POWER_ACK      "pwr_ack"     /**< u32, device state: the newest journal record's seq when the user dismissed the "power was lost" alert; power losses recorded up to it no longer count (health_dismiss_power_lost); written only at a dismissal */

/* ---- "ledger": availability accounting ------------------------------------- */
#define SETTINGS_NS_LEDGER          "ledger"      /**< whole namespace survives factory reset */
#define SETTINGS_KEY_LEDGER_TODAY   "today"       /**< blob, device state: running counters of the current UTC day with the last-alive mark (UTC ms) they run up to, one unit (an older blob without the mark: the alive key alone) */
#define SETTINGS_KEY_LEDGER_HIST    "hist"        /**< blob, device state: last 30 UTC days (availability per slot, reconnects, overflows) */
#define SETTINGS_KEY_LEDGER_ALIVE   "alive"       /**< i64, device state: last "still running" unix time, written only while the stored today is unchanged; a restore takes the later of it and the today blob's mark (power-off gaps count as downtime) */

/* ---- "web" ------------------------------------------------------------------ */
#define SETTINGS_NS_WEB             "web"
#define SETTINGS_KEY_WEB_PUBLIC     "public_status" /**< u8: minimal status readable without login */
#define SETTINGS_KEY_WEB_SESSIONS   "sessions"    /**< blob, device state: remembered logins (web_core.h web_sess_store_*, versioned: the admin password's salt they belong to, the latest UTC they may assume, then per session the SHA-256 of its token and its end in UTC; never a token); written at a login, a logout, a purge of ended ones, a password change and when the clock sets or cuts back an end, plus at most once a day for the mark, never per request; erased by factory reset */

/* ---- "lcast": local NTRIP for a rover (local_caster.h) ------------------- */
#define SETTINGS_NS_LCAST           "lcast"
#define SETTINGS_KEY_LCAST_CFG      "cfg"         /**< blob (a setting: settings_set_blob, posts SETTINGS_EVENT_CHANGED): v1 {u8 version = 1, bool enabled, str mountpoint (1-47), str username (1-31), str password (0 or 8-63, secret)}; later versions only append fields, and a reader ignores what it does not know (blob_reader_remaining); at most 146 bytes; written only on a user save, never periodically */

#ifdef __cplusplus
}
#endif
