/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Availability ledger around the pure core (ledger_core.c): a spinlock, the
 * RTC copy of the running counters (written every tick and right after the
 * restore at boot, read back after a software, panic or watchdog reset when
 * lifecycle accepts its stamp), and NVS (every
 * CONFIG_LEDGER_ALIVE_PERSIST_S and at the start of every reboot: today and
 * the last-alive mark it runs up to as one blob when today changed, the
 * mark alone in the alive key when it did not; the history when it changes;
 * read back after a power cut, or when the RTC copy is not valid or
 * predates another app image's run). The NVS writes of the tick and of a
 * reboot's shutdown hook are serialised by a mutex.
 */
#include "ledger.h"

#include <string.h>

#include "diag.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ledger_core.h"
#include "sdkconfig.h"
#include "settings.h"

static const char *TAG = "ledger";

_Static_assert(LEDGER_MAX_SLOTS == LC_SLOTS, "slot count differs between the API and the core");
_Static_assert((int)LEDGER_CLOCK_NONE == (int)LC_CLOCK_NONE && (int)LEDGER_CLOCK_SNTP == (int)LC_CLOCK_SNTP &&
                   (int)LEDGER_CLOCK_GNSS == (int)LC_CLOCK_GNSS,
               "clock sources differ between the API and the core");

#define DAY_S          86400
#define SHUTDOWN_WAIT_MS 300   /* of the shutdown budget, for a tick's writes to finish */
#define TODAY_BLOB_MAX 160u    /* today with its mark and the online times: 154 bytes */
#define HIST_BLOB_MAX  (3u + (size_t)CONFIG_LEDGER_HISTORY_DAYS * (4u + LC_SLOTS * 16u + 8u))

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static lc_t s_core;                                     /* under s_lock */
static EXT_RAM_BSS_ATTR lc_day_t s_hist[CONFIG_LEDGER_HISTORY_DAYS];
static EXT_RAM_BSS_ATTR uint8_t s_hist_blob[HIST_BLOB_MAX];   /* ledger_init, and under s_store */
static SemaphoreHandle_t s_store;                       /* the settings writes and s_hist_blob */
static bool s_ready;
static bool s_store_warned;
static bool s_planned_paused;                           /* under s_lock: casters paused for receiver work */
static bool s_planned_maint;                            /* under s_lock: receiver maintenance state */

/* Survives software, panic and watchdog resets; validated by magic, size and
 * CRC, and used only when lifecycle accepts its stamp. */
static RTC_NOINIT_ATTR lc_rtc_t s_rtc;
static uint32_t s_stamp;   /* this boot's image generation, stamped into every RTC copy */
static int64_t s_alive_key_s;   /* the alive key as stored (under s_store after init) */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void to_api(const lc_day_t *d, ledger_day_t *out)
{
    memset(out, 0, sizeof(*out));
    out->utc_day = d->utc_day;
    for (size_t s = 0; s < LEDGER_MAX_SLOTS; s++) {
        out->down_s[s] = d->down_ms[s] / 1000u;
        out->planned_s[s] = d->planned_ms[s] / 1000u;
        out->reconnects[s] = d->reconnects[s];
        out->up_s[s] = d->up_ms[s] / 1000u;
    }
    out->power_off_s = d->power_off_ms / 1000u;
    out->rx_overflows = d->rx_overflows;
}

/* How the previous session ended, as the reset tells: every reboot through
 * lifecycle ends in esp_restart(); anything else but a power cut is a crash,
 * a watchdog or the reset pin. */
static lc_reset_t reset_kind(esp_reset_reason_t r)
{
    if (r == ESP_RST_POWERON || r == ESP_RST_BROWNOUT || r == ESP_RST_PWR_GLITCH || r == ESP_RST_UNKNOWN) {
        return LC_RESET_POWER_CUT;
    }
    return (r == ESP_RST_SW) ? LC_RESET_SOFTWARE : LC_RESET_OTHER;
}

/* The books carry the previous session's flags for this boot only: its own
 * RTC copy and the stored today stop carrying them right after the restore,
 * so a boot that ends before its first tick (a crash in an init or start
 * step, an app trial reverted at init) hands nothing on. The stored today is
 * rewritten as stored (the span of the stored mark), only without the flags. */
static void consume_flags(lc_snapshot_t *stored, bool stored_ok)
{
    lc_snapshot_t snap;
    lc_snapshot(&s_core, &snap);
    lc_rtc_t blk;
    lc_rtc_seal(&blk, &snap, s_stamp);
    memcpy(&s_rtc, &blk, sizeof(blk));
    if (!stored_ok || (!stored->reboot_planned && !stored->planned)) {
        return;
    }
    stored->reboot_planned = false;
    stored->planned = false;
    uint8_t buf[TODAY_BLOB_MAX];
    size_t len = lc_encode_today(stored, buf, sizeof(buf));   /* with the blob's own mark, as stored */
    if (len == 0 || settings_set_blob(SETTINGS_NS_LEDGER, SETTINGS_KEY_LEDGER_TODAY, buf, len) != ESP_OK) {
        ESP_LOGW(TAG, "stored planned-downtime flags not cleared (settings unavailable?)");
    }
}

esp_err_t ledger_init(bool (*rtc_stamp_ok)(uint32_t stamp), uint32_t rtc_stamp)
{
    if (s_ready) {
        return ESP_OK;
    }
    s_stamp = rtc_stamp;
    if (s_store == NULL && (s_store = xSemaphoreCreateMutex()) == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int64_t up_ms = esp_timer_get_time() / 1000;
    lc_init(&s_core, s_hist, CONFIG_LEDGER_HISTORY_DAYS, (uint32_t)up_ms, (uint64_t)up_ms);
    lc_set_boot_window(&s_core, (uint32_t)CONFIG_LEDGER_PLANNED_BOOT_S * 1000u);

    lc_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.today.utc_day = -1;
    snap.pending.utc_day = -1;
    /* The day stored last as today: after an RTC restore it can be a day
     * that finished just before the reset and is not in the stored history
     * yet (lc_merge_stored_day). */
    lc_snapshot_t stored;
    memset(&stored, 0, sizeof(stored));
    stored.today.utc_day = -1;
    bool stored_ok = false;
    if (settings_get_i64(SETTINGS_NS_LEDGER, SETTINGS_KEY_LEDGER_ALIVE, &s_alive_key_s) != ESP_OK) {
        s_alive_key_s = 0;
    }
    {
        uint8_t buf[TODAY_BLOB_MAX];
        size_t len = sizeof(buf);
        stored_ok = settings_get_blob(SETTINGS_NS_LEDGER, SETTINGS_KEY_LEDGER_TODAY, buf, &len) == ESP_OK &&
                    lc_decode_today(buf, len, &stored);
        if (!stored_ok) {
            stored.today.utc_day = -1;
        }
    }
    /* The stored today with the online time up to the later mark: while the
     * alive key alone moved on, every expected slot streamed. Only the copy
     * (the flags are cleared from the blob as stored, below). */
    lc_snapshot_t stored_now = stored;
    if (stored_ok) {
        lc_stored_online(&stored_now, stored.alive_ms, lc_stored_mark(stored.alive_ms, s_alive_key_s));
    }
    /* A power cut (or a brownout) is never planned, whichever copy is read:
     * RTC memory that happens to pass its checks after one is not trusted
     * for that either; nor is a planned reboot's flag after a crash. */
    lc_reset_t reset = reset_kind(esp_reset_reason());
    const char *from;
    bool rtc_valid = lc_rtc_valid(&s_rtc);
    if (rtc_valid && rtc_stamp_ok != NULL && rtc_stamp_ok(s_rtc.stamp)) {
        snap = s_rtc.snap;
        from = "RTC copy";
    } else {
        /* Today and the mark it runs up to are one blob; the alive key moves
         * the mark on while today did not change. An older layout (no mark
         * in the blob) gives the alive key alone. */
        if (stored_ok) {
            snap.today = stored_now.today;
            snap.expected_mask = stored.expected_mask;
            snap.protected_mask = stored.protected_mask;
            snap.planned = stored.planned;
            snap.reboot_planned = stored.reboot_planned;
        }
        snap.alive_ms = lc_stored_mark(stored_ok ? stored.alive_ms : 0, s_alive_key_s);
        from = rtc_valid ? "settings (RTC copy not from the run before this boot)" : "settings";
    }
    lc_restore(&s_core, &snap, reset);

    size_t hlen = sizeof(s_hist_blob);
    if (settings_get_blob(SETTINGS_NS_LEDGER, SETTINGS_KEY_LEDGER_HIST, s_hist_blob, &hlen) == ESP_OK) {
        /* Decoded in place; lc_set_history only ever moves entries forward. */
        size_t n = lc_decode_hist(s_hist_blob, hlen, s_hist, CONFIG_LEDGER_HISTORY_DAYS);
        lc_set_history(&s_core, s_hist, n);
    }
    lc_merge_stored_day(&s_core, &stored_now.today);
    consume_flags(&stored, stored_ok);
    s_ready = true;
    ESP_LOGI(TAG, "restored from %s: day %ld, %u days of history, target %d.%d %%, last alive %lld%s",
             from, (long)s_core.today.utc_day, (unsigned)s_core.hist_n, CONFIG_LEDGER_TARGET_PERMILLE / 10,
             CONFIG_LEDGER_TARGET_PERMILLE % 10, (long long)(snap.alive_ms / 1000),
             s_core.prev_reboot_planned ? ", after a planned reboot" : "");
    return ESP_OK;
}

void ledger_note_prev_reboot(bool planned)
{
    if (!s_ready) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    bool before = s_core.prev_reboot_planned;
    if (planned) {
        lc_planned_boot(&s_core);     /* ignored after a power cut or a crash; once per boot */
    } else {
        lc_unplanned_boot(&s_core);   /* whatever the books said */
    }
    bool after = s_core.prev_reboot_planned;
    portEXIT_CRITICAL(&s_lock);
    if (before != after) {
        ESP_LOGI(TAG, "%s", after ? "after a planned reboot (by lifecycle's record)"
                                  : "not after a planned reboot (by lifecycle's record)");
    }
}

void ledger_set_slot(size_t slot, bool enabled, bool protected_slot, bool streaming)
{
    if (!s_ready || slot >= LEDGER_MAX_SLOTS) {
        return;
    }
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    lc_set_slot(&s_core, now, slot, enabled, protected_slot, streaming);
    portEXIT_CRITICAL(&s_lock);
}

void ledger_set_planned(bool planned)
{
    if (!s_ready) {
        return;
    }
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    s_planned_paused = planned;
    lc_set_planned(&s_core, now, s_planned_paused || s_planned_maint);
    portEXIT_CRITICAL(&s_lock);
}

void ledger_set_maintenance(bool on)
{
    if (!s_ready) {
        return;
    }
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    s_planned_maint = on;
    lc_set_planned(&s_core, now, s_planned_paused || s_planned_maint);
    portEXIT_CRITICAL(&s_lock);
}

void ledger_set_rtcm_ok(bool ok)
{
    if (!s_ready) {
        return;
    }
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    lc_set_rtcm_ok(&s_core, now, ok);
    portEXIT_CRITICAL(&s_lock);
}

void ledger_set_utc(int64_t unix_s, ledger_clock_t source)
{
    if (!s_ready) {
        return;
    }
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    lc_clock_t before = s_core.clock;
    bool taken = lc_set_utc(&s_core, now, unix_s * 1000, (lc_clock_t)source);
    portEXIT_CRITICAL(&s_lock);
    if (!taken) {
        return;
    }
    diag_set_utc(unix_s);
    if ((lc_clock_t)source != before) {
        ESP_LOGI(TAG, "clock: %s", source == LEDGER_CLOCK_GNSS ? "GNSS" : "SNTP");
    }
}

void ledger_set_gps_time(uint32_t tow_ms, uint32_t age_ms)
{
    if (!s_ready) {
        return;
    }
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    lc_clock_t before = s_core.clock;
    bool taken = lc_set_gps_tow(&s_core, now, tow_ms, age_ms);
    int64_t unix_ms = taken ? lc_unix_now(&s_core, now) : -1;
    portEXIT_CRITICAL(&s_lock);
    if (!taken || unix_ms < 0) {
        return;
    }
    diag_set_utc(unix_ms / 1000);
    if (before != LC_CLOCK_GNSS) {
        ESP_LOGI(TAG, "clock: GNSS");
    }
}

void ledger_count_overflow(void)
{
    if (!s_ready) {
        return;
    }
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    lc_count_overflow(&s_core, now);
    portEXIT_CRITICAL(&s_lock);
}

/*
 * Accrue, refresh the RTC copy and store what is due (caller holds s_store).
 * `interval_ms` 0 stores now (a reboot; without a clock, today with the mark
 * it was restored with, or nothing without one: the RTC copy carries the
 * flags then). Today is only ever stored together with its mark, in one
 * blob that NVS replaces as a unit; while today is unchanged since it was
 * stored, the mark alone goes to the alive key. Each store is one write
 * (plus lowering the alive key after the clock went back), so neither a
 * failed write nor a power cut can leave today ahead of its mark.
 */
static void step(uint32_t interval_ms)
{
    uint32_t now = now_ms();
    lc_snapshot_t snap;
    int64_t alive_ms = 0;
    bool store_today = false;
    bool store_hist = false;
    size_t hist_len = 0;
    portENTER_CRITICAL(&s_lock);
    lc_tick(&s_core, now);
    lc_snapshot(&s_core, &snap);
    bool due = lc_persist_due(&s_core, now, interval_ms, &alive_ms, &store_today, &store_hist);
    if (due && store_hist) {
        hist_len = lc_encode_hist(s_core.hist, s_core.hist_n, s_hist_blob, sizeof(s_hist_blob));
    }
    portEXIT_CRITICAL(&s_lock);

    /* RTC copy every tick: a reset other than a power cut loses at most a second. */
    lc_rtc_t blk;
    lc_rtc_seal(&blk, &snap, s_stamp);
    memcpy(&s_rtc, &blk, sizeof(blk));

    if (!due) {
        return;
    }
    /* History before today: a day rolled into the history must not vanish
     * from both if only one write goes through. */
    bool hist_ok = !store_hist || (hist_len > 0 && settings_set_blob(SETTINGS_NS_LEDGER, SETTINGS_KEY_LEDGER_HIST,
                                                                     s_hist_blob, hist_len) == ESP_OK);
    bool today_ok = !store_today;
    bool alive_ok = true;
    if (store_today) {
        /* Today with its mark, one write. After a failed history write
         * neither moves: the stored day stays the old one, and so does the
         * mark it runs up to (the time since counts as gap, never twice). */
        if (hist_ok) {
            snap.alive_ms = alive_ms;
            uint8_t buf[TODAY_BLOB_MAX];
            size_t len = lc_encode_today(&snap, buf, sizeof(buf));
            today_ok = len > 0 &&
                       settings_set_blob(SETTINGS_NS_LEDGER, SETTINGS_KEY_LEDGER_TODAY, buf, len) == ESP_OK;
        }
        /* Only after the clock went back (a correction, or a previous
         * session's clock that ran ahead): a restore takes the later mark,
         * so the alive key must not stay ahead of this one. */
        if (today_ok && s_alive_key_s > alive_ms / 1000 &&
            settings_set_i64(SETTINGS_NS_LEDGER, SETTINGS_KEY_LEDGER_ALIVE, alive_ms / 1000) == ESP_OK) {
            s_alive_key_s = alive_ms / 1000;
        }
    } else {
        /* Today unchanged since it was stored: it is exact up to here. */
        alive_ok = settings_set_i64(SETTINGS_NS_LEDGER, SETTINGS_KEY_LEDGER_ALIVE, alive_ms / 1000) == ESP_OK;
        if (alive_ok) {
            s_alive_key_s = alive_ms / 1000;
        }
    }
    if (!hist_ok || !today_ok) {
        portENTER_CRITICAL(&s_lock);
        lc_mark_dirty(&s_core, !today_ok, !hist_ok);
        portEXIT_CRITICAL(&s_lock);
    }
    if ((!hist_ok || !today_ok || !alive_ok) && !s_store_warned) {
        s_store_warned = true;
        ESP_LOGW(TAG, "counters not stored (settings unavailable?); retrying every %d s",
                 CONFIG_LEDGER_ALIVE_PERSIST_S);
    }
}

void ledger_tick(void)
{
    if (!s_ready) {
        return;
    }
    xSemaphoreTake(s_store, portMAX_DELAY);
    step((uint32_t)CONFIG_LEDGER_ALIVE_PERSIST_S * 1000u);
    xSemaphoreGive(s_store);
}

void ledger_shutdown(bool planned)
{
    if (!s_ready) {
        return;
    }
    /* First the flag: every RTC copy from here on carries it, stored or not. */
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    lc_shutdown(&s_core, now, planned);
    portEXIT_CRITICAL(&s_lock);
    /* Bounded: a tick holds the mutex only for its own writes. */
    if (xSemaphoreTake(s_store, pdMS_TO_TICKS(SHUTDOWN_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "counters not stored before the reboot (settings busy)");
        return;
    }
    step(0);
    xSemaphoreGive(s_store);
    ESP_LOGI(TAG, "counters stored before the reboot%s", planned ? " (planned downtime from here)" : "");
}

esp_err_t ledger_get_today(ledger_day_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        memset(out, 0, sizeof(*out));
        out->utc_day = -1;
        return ESP_ERR_INVALID_STATE;
    }
    lc_day_t d;
    portENTER_CRITICAL(&s_lock);
    lc_today(&s_core, &d);
    portEXIT_CRITICAL(&s_lock);
    to_api(&d, out);
    return ESP_OK;
}

esp_err_t ledger_get_history(ledger_day_t *days, size_t *count)
{
    if (count == NULL || (days == NULL && *count > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t cap = *count;
    *count = 0;
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t n = 0;
    while (n < cap) {
        lc_day_t d;
        bool have = false;
        portENTER_CRITICAL(&s_lock);
        if (n < s_core.hist_n) {
            d = s_core.hist[n];
            have = true;
        }
        portEXIT_CRITICAL(&s_lock);
        if (!have) {
            break;
        }
        to_api(&d, &days[n]);
        n++;
    }
    *count = n;
    return ESP_OK;
}

uint32_t ledger_protected_down_today_s(void)
{
    if (!s_ready) {
        return 0;
    }
    portENTER_CRITICAL(&s_lock);
    uint64_t ms = lc_protected_down_ms(&s_core);
    portEXIT_CRITICAL(&s_lock);
    uint64_t s = ms / 1000u;
    return (s > UINT32_MAX) ? UINT32_MAX : (uint32_t)s;
}

uint32_t ledger_protected_budget_left_s(void)
{
    /* Seconds online the target needs, before the daily grant. */
    const int64_t needed = (int64_t)DAY_S * CONFIG_LEDGER_TARGET_PERMILLE / 1000 - CONFIG_LEDGER_GRACE_S;
    int64_t left = (int64_t)DAY_S - needed - (int64_t)ledger_protected_down_today_s();
    if (left < 0) {
        return 0;
    }
    return (left > DAY_S) ? (uint32_t)DAY_S : (uint32_t)left;
}

int32_t ledger_utc_seconds_of_day(void)
{
    if (!s_ready) {
        return -1;
    }
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    int64_t u = lc_unix_now(&s_core, now);
    portEXIT_CRITICAL(&s_lock);
    return (u < 0) ? -1 : (int32_t)((u / 1000) % DAY_S);
}

ledger_clock_t ledger_clock_source(void)
{
    if (!s_ready) {
        return LEDGER_CLOCK_NONE;
    }
    portENTER_CRITICAL(&s_lock);
    lc_clock_t c = s_core.clock;
    portEXIT_CRITICAL(&s_lock);
    return (ledger_clock_t)c;
}
