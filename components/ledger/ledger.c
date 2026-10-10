/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Availability ledger around the pure core (ledger_core.c): a spinlock, the
 * RTC copy of the running counters (written every tick, read back after a
 * software, panic or watchdog reset), and NVS (today and the last-alive mark
 * every CONFIG_LEDGER_ALIVE_PERSIST_S, the history when it changes; read
 * back after a power cut or when the RTC copy is not valid).
 */
#include "ledger.h"

#include <string.h>

#include "diag.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "ledger_core.h"
#include "sdkconfig.h"
#include "settings.h"

static const char *TAG = "ledger";

_Static_assert(LEDGER_MAX_SLOTS == LC_SLOTS, "slot count differs between the API and the core");
_Static_assert((int)LEDGER_CLOCK_NONE == (int)LC_CLOCK_NONE && (int)LEDGER_CLOCK_SNTP == (int)LC_CLOCK_SNTP &&
                   (int)LEDGER_CLOCK_GNSS == (int)LC_CLOCK_GNSS,
               "clock sources differ between the API and the core");

#define DAY_S          86400
#define TODAY_BLOB_MAX 160u
#define HIST_BLOB_MAX  (3u + (size_t)CONFIG_LEDGER_HISTORY_DAYS * (4u + LC_SLOTS * 12u + 8u))

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static lc_t s_core;                                     /* under s_lock */
static EXT_RAM_BSS_ATTR lc_day_t s_hist[CONFIG_LEDGER_HISTORY_DAYS];
static EXT_RAM_BSS_ATTR uint8_t s_hist_blob[HIST_BLOB_MAX];   /* ledger_init and ledger_tick only */
static bool s_ready;
static bool s_store_warned;

/* Survives software, panic and watchdog resets; validated by magic, size and CRC. */
static RTC_NOINIT_ATTR lc_rtc_t s_rtc;

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
    }
    out->power_off_s = d->power_off_ms / 1000u;
    out->rx_overflows = d->rx_overflows;
}

static bool power_cut(esp_reset_reason_t r)
{
    return r == ESP_RST_POWERON || r == ESP_RST_BROWNOUT || r == ESP_RST_PWR_GLITCH || r == ESP_RST_UNKNOWN;
}

esp_err_t ledger_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    int64_t up_ms = esp_timer_get_time() / 1000;
    lc_init(&s_core, s_hist, CONFIG_LEDGER_HISTORY_DAYS, (uint32_t)up_ms, (uint64_t)up_ms);

    lc_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.today.utc_day = -1;
    snap.pending.utc_day = -1;
    bool cut = false;
    const char *from;
    if (lc_rtc_valid(&s_rtc)) {
        snap = s_rtc.snap;
        from = "RTC copy";
    } else {
        uint8_t buf[TODAY_BLOB_MAX];
        size_t len = sizeof(buf);
        if (settings_get_blob(SETTINGS_NS_LEDGER, SETTINGS_KEY_LEDGER_TODAY, buf, &len) == ESP_OK) {
            (void)lc_decode_today(buf, len, &snap);
        }
        int64_t alive_s = 0;
        if (settings_get_i64(SETTINGS_NS_LEDGER, SETTINGS_KEY_LEDGER_ALIVE, &alive_s) == ESP_OK && alive_s > 0) {
            snap.alive_ms = alive_s * 1000;
        }
        cut = power_cut(esp_reset_reason());
        from = "settings";
    }
    lc_restore(&s_core, &snap, cut);

    size_t hlen = sizeof(s_hist_blob);
    if (settings_get_blob(SETTINGS_NS_LEDGER, SETTINGS_KEY_LEDGER_HIST, s_hist_blob, &hlen) == ESP_OK) {
        /* Decoded in place; lc_set_history only ever moves entries forward. */
        size_t n = lc_decode_hist(s_hist_blob, hlen, s_hist, CONFIG_LEDGER_HISTORY_DAYS);
        lc_set_history(&s_core, s_hist, n);
    }
    s_ready = true;
    ESP_LOGI(TAG, "restored from %s: day %ld, %u days of history, target %d.%d %%, last alive %lld",
             from, (long)s_core.today.utc_day, (unsigned)s_core.hist_n, CONFIG_LEDGER_TARGET_PERMILLE / 10,
             CONFIG_LEDGER_TARGET_PERMILLE % 10, (long long)(snap.alive_ms / 1000));
    return ESP_OK;
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
    lc_set_planned(&s_core, now, planned);
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

void ledger_tick(void)
{
    if (!s_ready) {
        return;
    }
    uint32_t now = now_ms();
    lc_snapshot_t snap;
    int64_t alive_ms = 0;
    bool store_today = false;
    bool store_hist = false;
    size_t hist_len = 0;
    portENTER_CRITICAL(&s_lock);
    lc_tick(&s_core, now);
    lc_snapshot(&s_core, &snap);
    bool due = lc_persist_due(&s_core, now, (uint32_t)CONFIG_LEDGER_ALIVE_PERSIST_S * 1000u, &alive_ms,
                              &store_today, &store_hist);
    if (due && store_hist) {
        hist_len = lc_encode_hist(s_core.hist, s_core.hist_n, s_hist_blob, sizeof(s_hist_blob));
    }
    portEXIT_CRITICAL(&s_lock);

    /* RTC copy every tick: a reset other than a power cut loses at most a second. */
    lc_rtc_t blk;
    lc_rtc_seal(&blk, &snap);
    memcpy(&s_rtc, &blk, sizeof(blk));

    if (!due) {
        return;
    }
    /* History before today: a day rolled into the history must not vanish
     * from both if only one write goes through. */
    bool hist_ok = !store_hist || (hist_len > 0 && settings_set_blob(SETTINGS_NS_LEDGER, SETTINGS_KEY_LEDGER_HIST,
                                                                     s_hist_blob, hist_len) == ESP_OK);
    bool today_ok = !store_today;
    if (store_today && hist_ok) {   /* after a failed history write, the stored day stays the old one */
        uint8_t buf[TODAY_BLOB_MAX];
        size_t len = lc_encode_today(&snap, buf, sizeof(buf));
        today_ok = len > 0 &&
                   settings_set_blob(SETTINGS_NS_LEDGER, SETTINGS_KEY_LEDGER_TODAY, buf, len) == ESP_OK;
    }
    bool alive_ok = settings_set_i64(SETTINGS_NS_LEDGER, SETTINGS_KEY_LEDGER_ALIVE, alive_ms / 1000) == ESP_OK;
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
