/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "lifecycle.h"

#include <string.h>

#include "boot_guard.h"
#include "boot_history.h"
#include "diag.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "reboot_gate.h"
#include "sdkconfig.h"
#include "settings.h"

static const char *TAG = "lifecycle";

ESP_EVENT_DEFINE_BASE(LIFECYCLE_EVENT);

_Static_assert(CONFIG_LIFECYCLE_MAX_INHIBITS <= REBOOT_GATE_MAX_INHIBITS, "too many reboot inhibits");
_Static_assert(CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES >= 2,
               "the lifecycle service wakes on task notification index 1");
_Static_assert(BOOT_GUARD_UNKNOWN == UINT32_MAX && BOOT_HISTORY_UPTIME_UNKNOWN == UINT32_MAX,
               "unknown session uptime is UINT32_MAX everywhere");

/* Survives software, panic and watchdog resets; validated by magic + checksum. */
static RTC_NOINIT_ATTR boot_guard_state_t s_rtc;

static lifecycle_boot_info_t s_info;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Inhibits and the queue/commit decision for reboots. Read and changed under
 * s_lock only, so checking for inhibits and committing a reboot is one step. */
static reboot_gate_t s_gate;

/* Flags carried by a reboot request. They change the RTC state only when that
 * reboot is committed, never for a refused or still-queued request. */
#define FLAG_SAFE_RETRY (1u << 0)   /* boot_guard_prepare_safe_retry */
#define FLAG_CLEAR_SAFE (1u << 1)   /* boot_guard_clear */
static bool s_safe_cleared;         /* under s_lock: a later retry must not undo the user's clear */

/* Service task: 1 Hz stable-boot marking and safe-mode retry, and the reboots
 * committed by lifecycle_request_reboot() / lifecycle_inhibit_release(), so
 * neither the requester nor the releaser runs the shutdown hooks on its stack. */
#define SERVICE_STACK      4096
#define SERVICE_PRIO       13
#define SERVICE_NOTIFY_IDX 1        /* index 0 belongs to IDF APIs */
#define HEARTBEAT_S        60       /* session-uptime heartbeat (RTC only, no flash) */
static TaskHandle_t s_service;      /* under s_lock */
static bool s_deferred;             /* under s_lock: a committed reboot waits for the service task */
static bool s_retry_requested;      /* service task only */
static uint32_t s_last_beat_s;      /* service task only */
static bool s_app_trial;            /* under s_lock */
static lifecycle_update_t s_update; /* under s_lock: holder of the update slot */

/* Shutdown hooks run once per boot. If they overrun the budget, a one-shot
 * timer restarts anyway, well before the task watchdog of a subscribed caller
 * would turn a planned reboot into a crash reset. */
#define MAX_HOOKS              4
#define HOOK_OVERRUN_MARGIN_MS 500
static struct {
    lifecycle_shutdown_hook_t fn;
    void *ctx;
} s_hooks[MAX_HOOKS];
static int s_hook_count;
typedef enum { HOOKS_IDLE = 0, HOOKS_RUNNING, HOOKS_DONE } hooks_state_t;
static hooks_state_t s_hooks_state;     /* under s_lock */
static int64_t s_hooks_deadline_us;     /* under s_lock */
static esp_timer_handle_t s_overrun_timer;

#if defined(CONFIG_ESP_TASK_WDT_EN) && defined(CONFIG_ESP_TASK_WDT_TIMEOUT_S)
_Static_assert(CONFIG_LIFECYCLE_SHUTDOWN_BUDGET_MS + HOOK_OVERRUN_MARGIN_MS < CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000,
               "a reboot from a watchdog-subscribed task must restart before the task watchdog fires");
#endif

static boot_hw_reset_t map_reset(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:
        return BOOT_HW_POWER_ON;
    case ESP_RST_SW:
        return BOOT_HW_SOFTWARE;
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_CPU_LOCKUP:
        return BOOT_HW_CRASH;
    case ESP_RST_BROWNOUT:
    case ESP_RST_PWR_GLITCH:
        return BOOT_HW_BROWNOUT;
    default:
        return BOOT_HW_OTHER;
    }
}

static void refresh_info(void)
{
    s_info.boots_since_power_on = s_rtc.boots;
    s_info.crashes = s_rtc.crashes;
    s_info.brownouts_since_power_on = s_rtc.brownouts;
    s_info.safe_mode = s_rtc.safe_mode != 0;
    s_info.safe_mode_retry_s = s_info.safe_mode ? boot_guard_safe_retry_s(&s_rtc) : 0;
}

/* Call under s_lock with the flags a committed (or joined) reboot hands back. */
static void apply_flags_locked(uint32_t flags)
{
    if (flags & FLAG_CLEAR_SAFE) {
        boot_guard_clear(&s_rtc);
        s_safe_cleared = true;
    }
    if ((flags & FLAG_SAFE_RETRY) && !s_safe_cleared) {
        boot_guard_prepare_safe_retry(&s_rtc, CONFIG_LIFECYCLE_BOOTLOOP_LIMIT);
    }
}

/* ---- shutdown hooks ------------------------------------------------------------ */

static void on_hooks_overrun(void *arg)
{
    (void)arg;
    ESP_LOGE(TAG, "shutdown hooks overran the %d ms budget: restarting anyway", CONFIG_LIFECYCLE_SHUTDOWN_BUDGET_MS);
    esp_restart();
}

/* The first caller runs the hooks; a second one (an esp_restart() racing a
 * lifecycle reboot) waits until they finish or the budget is used up, so
 * sessions are not cut half-closed. */
static void run_hooks(void)
{
    bool run = false;
    taskENTER_CRITICAL(&s_lock);
    if (s_hooks_state == HOOKS_IDLE) {
        s_hooks_state = HOOKS_RUNNING;
        s_hooks_deadline_us = esp_timer_get_time() + (int64_t)CONFIG_LIFECYCLE_SHUTDOWN_BUDGET_MS * 1000;
        run = true;
    }
    int64_t deadline = s_hooks_deadline_us;
    taskEXIT_CRITICAL(&s_lock);

    if (!run) {
        for (;;) {
            taskENTER_CRITICAL(&s_lock);
            bool done = (s_hooks_state == HOOKS_DONE);
            taskEXIT_CRITICAL(&s_lock);
            if (done || esp_timer_get_time() >= deadline) {
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (s_overrun_timer != NULL) {
        (void)esp_timer_start_once(s_overrun_timer,
                                   ((uint64_t)CONFIG_LIFECYCLE_SHUTDOWN_BUDGET_MS + HOOK_OVERRUN_MARGIN_MS) * 1000);
    }
    for (int i = 0; i < s_hook_count; i++) {
        int64_t left_us = deadline - esp_timer_get_time();
        if (left_us <= 0) {
            ESP_LOGW(TAG, "shutdown budget used up, %d hook(s) skipped", s_hook_count - i);
            break;
        }
        s_hooks[i].fn((uint32_t)(left_us / 1000), s_hooks[i].ctx);
    }
    if (s_overrun_timer != NULL) {
        (void)esp_timer_stop(s_overrun_timer);   /* the restart follows anyway; avoid a second one */
    }
    taskENTER_CRITICAL(&s_lock);
    s_hooks_state = HOOKS_DONE;
    taskEXIT_CRITICAL(&s_lock);
}

/* Best effort for an esp_restart() that bypasses lifecycle. IDF runs shutdown
 * handlers newest first and Wi-Fi registers esp_wifi_stop after this one, so
 * here the network is already down: nothing reaches a caster any more. Only
 * lifecycle_reboot() and lifecycle_request_reboot() run the hooks while the
 * network still works. */
static void on_esp_restart(void)
{
    run_hooks();
}

/* ---- reboot execution ------------------------------------------------------------ */

static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

/* Runs a reboot the gate committed. Only the committer (or the service task it
 * handed the reboot to) gets here, so this runs once. */
static void __attribute__((noreturn)) do_reboot(lifecycle_reboot_reason_t reason)
{
    uint32_t code = (uint32_t)reason;
    if (reason == LIFECYCLE_REBOOT_BOOT_FAILURE) {
        code |= BOOT_GUARD_REASON_CRASH;
    }
    uint32_t now = uptime_s();
    taskENTER_CRITICAL(&s_lock);
    boot_guard_set_reboot_reason(&s_rtc, code);
    /* Exact session length for the next boot; a GNSS-ladder reboot also
     * spends one from the reboot budget. */
    if (reason == LIFECYCLE_REBOOT_GNSS_LADDER) {
        boot_guard_record_ladder_reboot(&s_rtc, now);
    } else {
        boot_guard_heartbeat(&s_rtc, now);
    }
    taskEXIT_CRITICAL(&s_lock);

    ESP_LOGW(TAG, "rebooting (reason %d)", (int)reason);
    (void)esp_event_post(LIFECYCLE_EVENT, LIFECYCLE_EVENT_PRE_REBOOT, &reason, sizeof(reason), 0);
    run_hooks();
    esp_restart();
}

/* Another task runs the committed reboot; it restarts within the hook budget
 * plus the overrun margin. */
static void __attribute__((noreturn)) wait_for_reboot(void)
{
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

/* After a COMMIT from a queued request or a release: hand the reboot to the
 * service task. Without one (it could not be started) it runs right here. */
static void run_deferred(bool here, lifecycle_reboot_reason_t reason, TaskHandle_t service)
{
    if (here) {
        do_reboot(reason);
    }
    xTaskNotifyGiveIndexed(service, SERVICE_NOTIFY_IDX);
}

/* Queued request: commit now, queue behind an inhibit, or join a reboot in progress. */
static esp_err_t request_reboot(lifecycle_reboot_reason_t reason, uint32_t flags)
{
    uint32_t apply;
    bool here = false;
    TaskHandle_t service;
    taskENTER_CRITICAL(&s_lock);
    reboot_gate_result_t res = reboot_gate_request(&s_gate, (uint32_t)reason, flags, &apply);
    apply_flags_locked(apply);
    service = s_service;
    if (res == REBOOT_GATE_COMMIT) {
        here = (service == NULL);
        s_deferred = !here;
    }
    const char *holder = reboot_gate_holder(&s_gate);
    taskEXIT_CRITICAL(&s_lock);

    switch (res) {
    case REBOOT_GATE_COMMIT:
        run_deferred(here, reason, service);
        return ESP_OK;
    case REBOOT_GATE_QUEUED:
        ESP_LOGI(TAG, "reboot (reason %d) queued until %s finishes", (int)reason, holder ? holder : "an operation");
        return ESP_OK;
    case REBOOT_GATE_BUSY:
        return ESP_OK;   /* a reboot is already under way */
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

/* Immediate reboot in the caller: commit now, refuse while inhibited, or wait
 * for a reboot already in progress. */
static esp_err_t reboot_now(lifecycle_reboot_reason_t reason, bool force, uint32_t flags)
{
    uint32_t apply;
    taskENTER_CRITICAL(&s_lock);
    reboot_gate_result_t res = reboot_gate_now(&s_gate, (uint32_t)reason, flags, force, &apply);
    apply_flags_locked(apply);
    const char *holder = reboot_gate_holder(&s_gate);
    taskEXIT_CRITICAL(&s_lock);

    switch (res) {
    case REBOOT_GATE_COMMIT:
        do_reboot(reason);
    case REBOOT_GATE_BUSY:
        wait_for_reboot();
    default:
        if (reason == LIFECYCLE_REBOOT_NONE) {
            return ESP_ERR_INVALID_ARG;
        }
        ESP_LOGW(TAG, "reboot (reason %d) refused: %s in progress", (int)reason, holder ? holder : "an operation");
        return ESP_ERR_INVALID_STATE;
    }
}

/* ---- service task ------------------------------------------------------------------ */

static void service_poll(void)
{
    uint32_t now = uptime_s();

    if (now - s_last_beat_s >= HEARTBEAT_S) {
        s_last_beat_s = now;
        taskENTER_CRITICAL(&s_lock);
        boot_guard_heartbeat(&s_rtc, now);
        taskEXIT_CRITICAL(&s_lock);
    }

    if (!s_info.stable && now >= CONFIG_LIFECYCLE_STABLE_UPTIME_S) {
        taskENTER_CRITICAL(&s_lock);
        boot_guard_on_stable(&s_rtc);
        s_info.stable = true;
        refresh_info();
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "boot stable after %u s", (unsigned)now);
    }

    if (s_info.safe_mode && !s_retry_requested && now >= s_info.safe_mode_retry_s) {
        ESP_LOGW(TAG, "safe mode episode over: retrying a normal boot");
        /* The crash counter is set up for the retry when the reboot is
         * committed, not now: a queued retry must not change anything yet. */
        s_retry_requested = (request_reboot(LIFECYCLE_REBOOT_SAFE_MODE_RETRY, FLAG_SAFE_RETRY) == ESP_OK);
    }
}

static void service_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTakeIndexed(SERVICE_NOTIFY_IDX, pdTRUE, pdMS_TO_TICKS(1000));
        lifecycle_reboot_reason_t reason = LIFECYCLE_REBOOT_NONE;
        taskENTER_CRITICAL(&s_lock);
        if (s_deferred) {
            s_deferred = false;
            reason = (lifecycle_reboot_reason_t)s_gate.reason;
        }
        taskEXIT_CRITICAL(&s_lock);
        if (reason != LIFECYCLE_REBOOT_NONE) {
            do_reboot(reason);
        }
        service_poll();
    }
}

static esp_err_t start_service(void)
{
    esp_err_t err = ESP_OK;
    if (s_overrun_timer == NULL) {
        /* Created now, not at reboot time: a low-heap reboot must not depend
         * on an allocation. */
        const esp_timer_create_args_t args = {
            .callback = on_hooks_overrun,
            .name = "lc_overrun",
        };
        err = esp_timer_create(&args, &s_overrun_timer);
        if (err != ESP_OK) {
            s_overrun_timer = NULL;
            ESP_LOGE(TAG, "hook overrun timer not created (%s): shutdown hooks are unbounded", esp_err_to_name(err));
        }
    }
    taskENTER_CRITICAL(&s_lock);
    bool running = (s_service != NULL);
    taskEXIT_CRITICAL(&s_lock);
    if (running) {
        return err;
    }
    TaskHandle_t task = NULL;
    if (xTaskCreate(service_task, "lifecycle", SERVICE_STACK, NULL, SERVICE_PRIO, &task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    taskENTER_CRITICAL(&s_lock);
    s_service = task;
    taskEXIT_CRITICAL(&s_lock);
    return err;
}

/* ---- boot accounting ------------------------------------------------------------- */

void lifecycle_early_boot(void)
{
    esp_reset_reason_t raw = esp_reset_reason();
    boot_guard_result_t res;
    boot_guard_on_boot(&s_rtc, map_reset(raw), CONFIG_LIFECYCLE_BOOTLOOP_LIMIT, &res);
    reboot_gate_init(&s_gate, CONFIG_LIFECYCLE_MAX_INHIBITS);

    memset(&s_info, 0, sizeof(s_info));
    s_info.reset_reason = raw;
    s_info.reset_class = (lifecycle_reset_class_t)res.cls;
    s_info.prev_reboot_reason = (lifecycle_reboot_reason_t)(res.prev_reboot_reason & ~BOOT_GUARD_REASON_CRASH);
    s_info.prev_session_uptime_s = res.prev_session_s;   /* BOOT_GUARD_UNKNOWN == UINT32_MAX */
    refresh_info();

    (void)esp_register_shutdown_handler(on_esp_restart);

    if (res.cls == BOOT_RESET_BROWNOUT) {
        ESP_LOGW(TAG, "brownout reset (%u since power-on): check the power supply",
                 (unsigned)s_info.brownouts_since_power_on);
    }
    if (res.cls == BOOT_RESET_CRASH) {
        ESP_LOGW(TAG, "crash reset: %u since the last stable boot (safe mode at %d)",
                 (unsigned)s_info.crashes, CONFIG_LIFECYCLE_BOOTLOOP_LIMIT);
    }
    if (s_info.safe_mode) {
        ESP_LOGW(TAG, "SAFE MODE (episode %u): normal boot retried after %u s of uptime",
                 (unsigned)s_rtc.safe_episodes, (unsigned)s_info.safe_mode_retry_s);
    }
}

/* Prepend this boot to life/boots. A history this app cannot read (a newer
 * layout, after a rollback) is left untouched: "life" survives factory reset
 * and a rolled-back app deletes nothing. */
static esp_err_t record_boot(void)
{
    if (!settings_is_available()) {
        return ESP_ERR_INVALID_STATE;
    }

    boot_history_entry_t entries[BOOT_HISTORY_LEN];
    size_t count = 0;
    uint8_t buf[BOOT_HISTORY_BLOB_MAX];
    size_t len = sizeof(buf);
    esp_err_t err = settings_get_blob(SETTINGS_NS_LIFE, SETTINGS_KEY_BOOT_HISTORY, buf, &len);
    if (err == ESP_OK && !boot_history_decode(buf, len, &entries[1], BOOT_HISTORY_LEN - 1, &count)) {
        err = ESP_ERR_INVALID_VERSION;
    }
    if (err == ESP_ERR_INVALID_VERSION || err == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGW(TAG, "boot history has a newer layout: left as-is, this boot is not recorded");
        return ESP_OK;
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }

    boot_history_entry_t *now = &entries[0];
    memset(now, 0, sizeof(*now));
    now->cls = (uint8_t)s_info.reset_class;
    now->raw = (uint8_t)s_info.reset_reason;
    now->reboot_reason = (uint16_t)s_info.prev_reboot_reason;
    now->boot_no = s_info.boots_since_power_on;
    now->prev_uptime_s = s_info.prev_session_uptime_s;
    strlcpy(now->version, esp_app_get_description()->version, sizeof(now->version));

    len = boot_history_encode(entries, count + 1, buf, sizeof(buf));
    if (len == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    return settings_set_blob(SETTINGS_NS_LIFE, SETTINGS_KEY_BOOT_HISTORY, buf, len);
}

/* The event journal's timeline of boots (the persisted list is life/boots). */
static void journal_boot(void)
{
    uint32_t prev = s_info.prev_session_uptime_s;
    int32_t prev_s = (prev == BOOT_HISTORY_UPTIME_UNKNOWN) ? -1 : (int32_t)(prev > INT32_MAX ? INT32_MAX : prev);
    (void)diag_journal_append(DIAG_EV_BOOT, (int32_t)s_info.reset_class, (int32_t)s_info.prev_reboot_reason);
    (void)diag_journal_append(DIAG_EV_SESSION, prev_s, (int32_t)s_info.reset_reason);
}

esp_err_t lifecycle_init(void)
{
    esp_err_t err = start_service();
    if (err == ESP_ERR_NO_MEM) {
        ESP_LOGE(TAG, "service task not started: no stable-boot marking, heartbeat or safe-mode retry this boot");
    }
    esp_err_t hist = record_boot();
    journal_boot();
    return (err != ESP_OK) ? err : hist;
}

bool lifecycle_in_safe_mode(void)
{
    return s_info.safe_mode;
}

void lifecycle_get_boot_info(lifecycle_boot_info_t *out)
{
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    *out = s_info;
    taskEXIT_CRITICAL(&s_lock);
}

esp_err_t lifecycle_clear_safe_mode(void)
{
    /* The counters are cleared together with the commit of this reboot: a
     * refused request leaves the safe-mode state exactly as it was. */
    return reboot_now(LIFECYCLE_REBOOT_SAFE_MODE_CLEARED, false, FLAG_CLEAR_SAFE);
}

/* ---- inhibits and reboots ---------------------------------------------------------- */

esp_err_t lifecycle_inhibit_acquire(const char *what, lifecycle_inhibit_t *out)
{
    if (what == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_lock);
    int token = reboot_gate_acquire(&s_gate, what);
    taskEXIT_CRITICAL(&s_lock);
    if (token == REBOOT_GATE_TOKEN_REBOOTING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (token == REBOOT_GATE_TOKEN_FULL) {
        return ESP_ERR_NO_MEM;
    }
    *out = token;
    return ESP_OK;
}

void lifecycle_inhibit_release(lifecycle_inhibit_t token)
{
    uint32_t apply;
    bool here = false;
    lifecycle_reboot_reason_t reason = LIFECYCLE_REBOOT_NONE;
    TaskHandle_t service;
    taskENTER_CRITICAL(&s_lock);
    reboot_gate_result_t res = reboot_gate_release(&s_gate, token, &apply);
    service = s_service;
    if (res == REBOOT_GATE_COMMIT) {
        apply_flags_locked(apply);
        reason = (lifecycle_reboot_reason_t)s_gate.reason;
        here = (service == NULL);
        s_deferred = !here;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (res == REBOOT_GATE_COMMIT) {
        run_deferred(here, reason, service);
    }
}

bool lifecycle_reboot_inhibited(const char **what)
{
    taskENTER_CRITICAL(&s_lock);
    const char *holder = reboot_gate_holder(&s_gate);
    taskEXIT_CRITICAL(&s_lock);
    if (what != NULL) {
        *what = holder;
    }
    return holder != NULL;
}

esp_err_t lifecycle_reboot(lifecycle_reboot_reason_t reason, bool force)
{
    return reboot_now(reason, force, 0);
}

esp_err_t lifecycle_request_reboot(lifecycle_reboot_reason_t reason)
{
    return request_reboot(reason, 0);
}

void lifecycle_fatal_boot_error(const char *what, esp_err_t err)
{
    ESP_LOGE(TAG, "%s failed (%s): rebooting in %d s", what ? what : "boot step", esp_err_to_name(err),
             CONFIG_LIFECYCLE_FATAL_RETRY_S);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_LIFECYCLE_FATAL_RETRY_S * 1000));
    /* Direct restart: used only before the network and the casters exist, so
     * there is no session for the shutdown hooks to close. */
    boot_guard_set_reboot_reason(&s_rtc, (uint32_t)LIFECYCLE_REBOOT_BOOT_FAILURE | BOOT_GUARD_REASON_CRASH);
    esp_restart();
}

esp_err_t lifecycle_register_shutdown_hook(lifecycle_shutdown_hook_t hook, void *ctx)
{
    if (hook == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_ERR_NO_MEM;
    taskENTER_CRITICAL(&s_lock);
    if (s_hook_count < MAX_HOOKS) {
        s_hooks[s_hook_count].fn = hook;
        s_hooks[s_hook_count].ctx = ctx;
        s_hook_count++;
        err = ESP_OK;
    }
    taskEXIT_CRITICAL(&s_lock);
    return err;
}

bool lifecycle_claim_once(uint32_t flag)
{
    taskENTER_CRITICAL(&s_lock);
    bool first = boot_guard_claim_once(&s_rtc, flag);
    taskEXIT_CRITICAL(&s_lock);
    return first;
}

bool lifecycle_ladder_reboot_allowed(uint32_t *retry_in_s)
{
    uint32_t now = uptime_s();
    taskENTER_CRITICAL(&s_lock);
    bool allowed = boot_guard_ladder_reboot_allowed(&s_rtc, now, retry_in_s);
    taskEXIT_CRITICAL(&s_lock);
    return allowed;
}

void lifecycle_set_app_trial(bool active)
{
    taskENTER_CRITICAL(&s_lock);
    s_app_trial = active;
    taskEXIT_CRITICAL(&s_lock);
}

bool lifecycle_app_trial_active(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool active = s_app_trial;
    taskEXIT_CRITICAL(&s_lock);
    return active;
}

esp_err_t lifecycle_update_claim(lifecycle_update_t what)
{
    if (what != LIFECYCLE_UPDATE_APP && what != LIFECYCLE_UPDATE_RECEIVER) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_lock);
    bool ok = (s_update == LIFECYCLE_UPDATE_NONE || s_update == what);
    if (ok) {
        s_update = what;
    }
    taskEXIT_CRITICAL(&s_lock);
    return ok ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void lifecycle_update_release(lifecycle_update_t what)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_update == what) {
        s_update = LIFECYCLE_UPDATE_NONE;
    }
    taskEXIT_CRITICAL(&s_lock);
}

lifecycle_update_t lifecycle_update_active(void)
{
    taskENTER_CRITICAL(&s_lock);
    lifecycle_update_t what = s_update;
    taskEXIT_CRITICAL(&s_lock);
    return what;
}
