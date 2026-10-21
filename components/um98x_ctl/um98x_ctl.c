/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * RTOS side of the command layer: the GNSS link as the executor's port, the
 * lock shared with the RX task, the wake-up, the task watchdog, persistence
 * and the base-position source. Decisions live in the pure cores
 * (um98x_reply, um98x_txn, um98x_plan, um98x_exec, um98x_base).
 */
#include "um98x_ctl.h"

#include <string.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gnss_obs.h"
#include "sdkconfig.h"
#include "settings.h"
#include "settings_schema.h"
#include "um98x_exec.h"
#include "um98x_plan.h"
#include "um98x_reply.h"

static const char *TAG = "um98x_ctl";

#define WRITE_TIMEOUT_MS 1000u   /* a command line is < 128 B: well under this even at 9600 */
#define WAIT_SLICE_MS    1000    /* longest block between watchdog feeds */
#define CALL_WAIT_MS     1000u   /* a second caller waits this long, then gets ESP_ERR_TIMEOUT */

/* Shared with the RX task under s_lock: the channel (transaction in flight,
 * restart evidence, counters). Small, so it stays in internal RAM. */
static um98x_chan_t s_chan;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t s_wake;    /* a line for the waiting executor */
static SemaphoreHandle_t s_call;    /* one blocking call at a time */
static volatile bool s_inited;
static volatile bool s_prompt;      /* loader prompt seen (station's on_prompt sink) */

static um98x_ctl_restart_cb_t s_restart_cb;   /* under s_lock */
static void *s_restart_ctx;

static um98x_info_t s_info;         /* under s_lock; valid = identity known this boot */

/* ---- port --------------------------------------------------------------- */

typedef struct {
    gnss_link_lease_t lease;
} port_ctx_t;

static int64_t mono_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void p_lock(void *ctx)
{
    (void)ctx;
    portENTER_CRITICAL(&s_lock);
}

static void p_unlock(void *ctx)
{
    (void)ctx;
    portEXIT_CRITICAL(&s_lock);
}

static int64_t p_now(void *ctx)
{
    (void)ctx;
    return mono_ms();
}

static bool p_write(void *ctx, const char *data, size_t len)
{
    const port_ctx_t *pc = ctx;
    return gnss_link_write(pc->lease, data, len, WRITE_TIMEOUT_MS) == ESP_OK;
}

static bool p_set_baud(void *ctx, uint32_t baud)
{
    const port_ctx_t *pc = ctx;
    return gnss_link_set_baud(pc->lease, baud) == ESP_OK;
}

static uint32_t p_get_baud(void *ctx)
{
    (void)ctx;
    return gnss_link_get_baud();
}

static void feed_watchdog(void)
{
#if CONFIG_ESP_TASK_WDT_EN
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        (void)esp_task_wdt_reset();
    }
#endif
}

static void p_wait(void *ctx, int64_t until_ms)
{
    (void)ctx;
    int64_t ms = until_ms - mono_ms();
    if (ms > WAIT_SLICE_MS) {
        ms = WAIT_SLICE_MS;
    }
    if (ms > 0) {
        TickType_t ticks = pdMS_TO_TICKS((uint32_t)ms);
        (void)xSemaphoreTake(s_wake, ticks > 0 ? ticks : 1);
    }
    feed_watchdog();
}

static void p_counts(void *ctx, um98x_link_counts_t *out)
{
    (void)ctx;
    gnss_link_stats_t st;
    memset(out, 0, sizeof(*out));
    if (gnss_link_get_stats(&st) == ESP_OK) {
        out->rtcm_frames = st.framing.rtcm_frames;
        out->text_lines = st.framing.text_lines;
        out->rx_bytes = st.rx_bytes;
    }
}

static bool p_prompt(void *ctx)
{
    (void)ctx;
    return s_prompt;
}

/* One executor per call, bound to the caller's lease. */
typedef struct {
    port_ctx_t pc;
    um98x_port_t port;
    um98x_exec_t ex;
} call_t;

static void call_setup(call_t *c, gnss_link_lease_t lease)
{
    c->pc.lease = lease;
    c->port = (um98x_port_t){
        .ctx = &c->pc,
        .lock = p_lock,
        .unlock = p_unlock,
        .now_ms = p_now,
        .write = p_write,
        .set_baud = p_set_baud,
        .get_baud = p_get_baud,
        .wait = p_wait,
        .counts = p_counts,
        .prompt_seen = p_prompt,
    };
    c->ex = (um98x_exec_t){ .ch = &s_chan, .port = &c->port };
}

static um98x_link_mode_t link_mode(void)
{
    switch (gnss_link_get_mode()) {
    case GNSS_LINK_MODE_STREAM:
        return UM98X_LINK_STREAM;
    case GNSS_LINK_MODE_COMMAND:
        return UM98X_LINK_COMMAND;
    case GNSS_LINK_MODE_RAW:
        return UM98X_LINK_RAW;
    default:
        return UM98X_LINK_OFF;
    }
}

/* cmd: the single command or query of the call, NULL for a whole session. */
static esp_err_t call_begin(gnss_link_lease_t lease, const char *cmd)
{
    if (!s_inited || !um98x_cmd_allowed(link_mode(), lease != NULL, cmd)) {
        return ESP_ERR_INVALID_STATE;   /* the allow-list of the link's current mode */
    }
    if (xSemaphoreTake(s_call, pdMS_TO_TICKS(CALL_WAIT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;   /* another session runs */
    }
    (void)xSemaphoreTake(s_wake, 0);   /* drop a stale wake-up */
    s_prompt = false;                  /* only a hit during this call counts */
    return ESP_OK;
}

static void call_end(void)
{
    xSemaphoreGive(s_call);
}

/* ---- identity and persistence ------------------------------------------- */

static void info_from_versiona(const gnss_versiona_t *v, um98x_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->valid = true;
    _Static_assert(sizeof(out->model) == sizeof(v->model) && sizeof(out->firmware) == sizeof(v->firmware) &&
                       sizeof(out->serial) == sizeof(v->serial) && sizeof(out->pn) == sizeof(v->pn) &&
                       sizeof(out->efuse_id) == sizeof(v->efuse_id) &&
                       sizeof(out->compile_date) == sizeof(v->compile_date),
                   "version log fields and receiver info must match");
    memcpy(out->model, v->model, sizeof(out->model));
    memcpy(out->firmware, v->firmware, sizeof(out->firmware));
    out->build = v->build;
    memcpy(out->serial, v->serial, sizeof(out->serial));
    memcpy(out->pn, v->pn, sizeof(out->pn));
    memcpy(out->efuse_id, v->efuse_id, sizeof(out->efuse_id));
    memcpy(out->compile_date, v->compile_date, sizeof(out->compile_date));
}

static void versiona_from_info(const um98x_info_t *in, gnss_versiona_t *v)
{
    memset(v, 0, sizeof(*v));
    if (!in->valid) {
        return;
    }
    memcpy(v->model, in->model, sizeof(v->model));
    memcpy(v->firmware, in->firmware, sizeof(v->firmware));
    v->build = in->build;
    memcpy(v->serial, in->serial, sizeof(v->serial));
    memcpy(v->pn, in->pn, sizeof(v->pn));
    memcpy(v->efuse_id, in->efuse_id, sizeof(v->efuse_id));
    memcpy(v->compile_date, in->compile_date, sizeof(v->compile_date));
}

static void put_str_if_changed(const char *key, const char *value)
{
    char cur[48];
    if (settings_get_str(SETTINGS_NS_GNSS, key, cur, sizeof(cur)) == ESP_OK && strcmp(cur, value) == 0) {
        return;
    }
    esp_err_t err = settings_set_state_str(SETTINGS_NS_GNSS, key, value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gnss/%s not saved: %s", key, esp_err_to_name(err));
    }
}

static void remember_identity(const gnss_versiona_t *v)
{
    um98x_info_t info;
    info_from_versiona(v, &info);
    portENTER_CRITICAL(&s_lock);
    s_info = info;
    portEXIT_CRITICAL(&s_lock);
    um98x_link_model_set(um98x_model_of(info.model));
    put_str_if_changed(SETTINGS_KEY_GNSS_MODEL, info.model);
    put_str_if_changed(SETTINGS_KEY_GNSS_FW, info.firmware);
}

static uint32_t stored_baud(void)
{
    uint32_t baud = 0;
    if (settings_get_u32(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_BAUD, &baud) != ESP_OK) {
        baud = 0;   /* none: the candidate list starts at 115200 */
    }
    return baud;
}

static void persist_baud(uint32_t baud)
{
    if (baud == 0 || stored_baud() == baud) {
        return;
    }
    esp_err_t err = settings_set_state_u32(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_BAUD, baud);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gnss/baud not saved: %s", esp_err_to_name(err));
    }
}

/* The applied state, in one batch: the next boot's applied check compares
 * with it. */
static bool persist_applied(const um98x_base_profile_t *p, const um98x_apply_result_t *r)
{
    settings_batch_t b;
    esp_err_t err = settings_batch_begin(SETTINGS_NS_GNSS, &b);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "applied state not saved: %s", esp_err_to_name(err));
        return false;
    }
    err = settings_batch_set_u32(b, SETTINGS_KEY_GNSS_APPLIED, um98x_applied_hash(p));
    if (err == ESP_OK) {
        err = settings_batch_set_str(b, SETTINGS_KEY_GNSS_APPLIED_FW, r->firmware);
    }
    if (err == ESP_OK) {
        err = settings_batch_set_u8(b, SETTINGS_KEY_GNSS_RTCM1230_OK, r->rtcm1230_ok ? 1 : 0);
    }
    if (err == ESP_OK) {
        /* The group this receiver runs: a later refused group goes back to it. */
        uint8_t m;
        uint8_t sl;
        um98x_target_group(p, um98x_link_model(), &m, &sl);
        err = settings_batch_set_u16(b, SETTINGS_KEY_GNSS_SG_OK, (uint16_t)(((unsigned)m << 8) | sl));
    }
    if (err == ESP_OK && r->baud != 0 && r->baud != stored_baud()) {
        err = settings_batch_set_u32(b, SETTINGS_KEY_GNSS_BAUD, r->baud);
    }
    if (err != ESP_OK) {
        settings_batch_abort(b);
        ESP_LOGW(TAG, "applied state not saved: %s", esp_err_to_name(err));
        return false;
    }
    err = settings_batch_commit_state(b);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "applied state not saved: %s", esp_err_to_name(err));
        return false;
    }
    /* The first configuration the user held back has now been made. */
    uint8_t held = 0;
    if (settings_get_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_HOLD, &held) == ESP_OK) {
        err = settings_erase_state_key(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_HOLD);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "gnss/hold not erased: %s", esp_err_to_name(err));
        }
    }
    return true;
}

static esp_err_t to_esp_err(um98x_res_t res)
{
    switch (res) {
    case UM98X_OK:
        return ESP_OK;
    case UM98X_E_TIMEOUT:
        return ESP_ERR_TIMEOUT;
    case UM98X_E_REJECTED:
    case UM98X_E_FAILED:
        return ESP_FAIL;
    case UM98X_E_ARG:
        return ESP_ERR_INVALID_ARG;
    case UM98X_E_UNSUPPORTED:
        return ESP_ERR_NOT_SUPPORTED;
    case UM98X_E_LOST:
    case UM98X_E_NOT_FOUND:
        return ESP_ERR_NOT_FOUND;
    case UM98X_E_PROMPT:
        return ESP_ERR_INVALID_RESPONSE;
    case UM98X_E_LINK:
    default:
        return ESP_ERR_INVALID_STATE;
    }
}

/* ---- public API --------------------------------------------------------- */

esp_err_t um98x_ctl_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    if (s_wake == NULL) {
        s_wake = xSemaphoreCreateBinary();
    }
    if (s_call == NULL) {
        s_call = xSemaphoreCreateMutex();
    }
    if (s_wake == NULL || s_call == NULL) {
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_lock);
    um98x_chan_init(&s_chan);
    memset(&s_info, 0, sizeof(s_info));
    portEXIT_CRITICAL(&s_lock);
    s_inited = true;
    return ESP_OK;
}

void um98x_ctl_set_port(uint8_t port)
{
    um98x_link_port_set(port);
}

uint32_t um98x_ctl_applied_hash(const um98x_base_profile_t *p)
{
    return um98x_applied_hash(p);
}

bool um98x_ctl_on_line(const char *line, size_t len, void *ctx)
{
    (void)ctx;
    if (!s_inited || line == NULL) {
        return false;
    }
    /* GNSS RX task: classify outside the lock, never block, never log. */
    um98x_line_t l;
    um98x_line_classify(line, len, &l);
    int64_t now = mono_ms();
    portENTER_CRITICAL(&s_lock);
    unsigned ev = um98x_chan_dispatch(&s_chan, line, len, &l, now);
    um98x_ctl_restart_cb_t cb = s_restart_cb;
    void *cb_ctx = s_restart_ctx;
    portEXIT_CRITICAL(&s_lock);
    if (ev & UM98X_CHAN_WAKE) {
        xSemaphoreGive(s_wake);
    }
    if ((ev & UM98X_CHAN_RESTART) && cb != NULL) {
        cb(cb_ctx);
    }
    return (ev & UM98X_CHAN_REPLY) != 0;
}

void um98x_ctl_notify_prompt(void)
{
    s_prompt = true;
    if (s_wake != NULL) {
        xSemaphoreGive(s_wake);
    }
}

void um98x_ctl_set_restart_cb(um98x_ctl_restart_cb_t cb, void *ctx)
{
    portENTER_CRITICAL(&s_lock);
    s_restart_cb = cb;
    s_restart_ctx = ctx;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t um98x_ctl_session_begin(gnss_link_lease_t lease)
{
    if (lease == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = call_begin(lease, NULL);
    if (err != ESP_OK) {
        return err;
    }
    /* Terminates whatever partial line the receiver may hold. */
    err = gnss_link_write(lease, "\r\n", 2, WRITE_TIMEOUT_MS);
    call_end();
    return err == ESP_OK ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t um98x_ctl_detect(gnss_link_lease_t lease, bool full_list, um98x_detect_result_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (lease == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = call_begin(lease, NULL);
    if (err != ESP_OK) {
        return err;
    }
    call_t c;
    call_setup(&c, lease);
    uint32_t cands[UM98X_DETECT_MAX];
    size_t n = um98x_detect_candidates(stored_baud(), full_list, cands, UM98X_DETECT_MAX);
    um98x_detect_out_t d;
    um98x_res_t res = um98x_exec_detect(&c.ex, cands, n, &d);
    call_end();

    out->saw_bytes = d.saw_bytes;
    out->prompt = (res == UM98X_E_PROMPT);
    if (res == UM98X_OK) {
        out->found = true;
        out->baud = d.baud;
        out->by_listen = d.by_listen;
        persist_baud(d.baud);
        if (d.version_valid) {
            out->identity = true;
            remember_identity(&d.version);
            info_from_versiona(&d.version, &out->info);
        }
        ESP_LOGI(TAG, "receiver found at %u baud (%s)", (unsigned)d.baud, d.by_listen ? "listening" : "probe");
    } else if (res == UM98X_E_PROMPT) {
        ESP_LOGW(TAG, "receiver at its loader prompt: probing stopped");
    } else if (res == UM98X_E_LINK) {
        ESP_LOGW(TAG, "detection stopped: link write or baud switch failed");
    } else {
        ESP_LOGW(TAG, "receiver not found (%s)", d.saw_bytes ? "bytes but nothing valid" : "no bytes at all");
    }
    return to_esp_err(res);
}

esp_err_t um98x_ctl_leave_loader(gnss_link_lease_t lease)
{
    if (lease == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = call_begin(lease, NULL);
    if (err != ESP_OK) {
        return err;
    }
    call_t c;
    call_setup(&c, lease);
    um98x_res_t res = um98x_exec_leave_loader(&c.ex, UM98X_LOADER_WAIT_MS);
    call_end();
    return to_esp_err(res);
}

esp_err_t um98x_ctl_query_version(gnss_link_lease_t lease, bool after_restart, um98x_info_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    esp_err_t err = call_begin(lease, UM98X_CMD_VERSION);
    if (err != ESP_OK) {
        return err;
    }
    call_t c;
    call_setup(&c, lease);
    int64_t end = 0;
    if (after_restart) {
        /* 20 s after the latest restart evidence, if it is that recent. */
        int64_t now = mono_ms();
        portENTER_CRITICAL(&s_lock);
        int64_t evidence = s_chan.last_restart_ms > s_chan.last_banner_ms ? s_chan.last_restart_ms
                                                                          : s_chan.last_banner_ms;
        portEXIT_CRITICAL(&s_lock);
        if (evidence == UM98X_NONE_MS || now - evidence > UM98X_VERSION_RESTART_SPAN_MS) {
            evidence = now;
        }
        end = evidence + UM98X_VERSION_RESTART_SPAN_MS;
    }
    gnss_versiona_t v;
    bool ack_seen = false;
    um98x_res_t res = um98x_exec_version(&c.ex, after_restart, end, &v, &ack_seen);
    call_end();
    if (res == UM98X_OK) {
        remember_identity(&v);
        if (out != NULL) {
            info_from_versiona(&v, out);
        }
        return ESP_OK;
    }
    if (res == UM98X_E_TIMEOUT && ack_seen) {
        return ESP_ERR_INVALID_RESPONSE;   /* acknowledged, no version data yet */
    }
    return to_esp_err(res);
}

esp_err_t um98x_ctl_get_info(um98x_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_info;
    portEXIT_CRITICAL(&s_lock);
    return out->valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool um98x_ctl_model_supported(const um98x_info_t *info)
{
    return info != NULL && info->valid && um98x_model_supported(info->model);
}

bool um98x_ctl_is_um980(void)
{
    return um98x_link_model() == UM98X_MODEL_UM980;
}

void um98x_ctl_target_group(const um98x_base_profile_t *p, uint8_t *master, uint8_t *slave)
{
    um98x_target_group(p, um98x_link_model(), master, slave);
}

esp_err_t um98x_ctl_query_mode(gnss_link_lease_t lease, char *body, size_t len)
{
    if (body == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    body[0] = '\0';
    esp_err_t err = call_begin(lease, UM98X_CMD_MODE);
    if (err != ESP_OK) {
        return err;
    }
    call_t c;
    call_setup(&c, lease);
    um98x_res_t res = um98x_exec_query_mode(&c.ex, body, len);
    call_end();
    return to_esp_err(res);
}

esp_err_t um98x_ctl_query_own_fix(gnss_link_lease_t lease, bool sppnav, um98x_own_fix_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    const char *cmd = sppnav ? UM98X_CMD_SPPNAV : UM98X_CMD_BASEPOS;
    esp_err_t err = call_begin(lease, cmd);
    if (err != ESP_OK) {
        return err;
    }
    call_t c;
    call_setup(&c, lease);
    um98x_res_t res = um98x_exec_own_fix(&c.ex, cmd, out);
    call_end();
    switch (res) {
    case UM98X_OK:
        return ESP_OK;
    case UM98X_E_REJECTED:
        return ESP_ERR_NOT_SUPPORTED;
    case UM98X_E_FAILED:
        return ESP_ERR_INVALID_RESPONSE;
    default:
        return to_esp_err(res);
    }
}

esp_err_t um98x_ctl_command(gnss_link_lease_t lease, const char *cmd, char *reply, size_t reply_len,
                            uint32_t timeout_ms)
{
    if (reply != NULL && reply_len > 0) {
        reply[0] = '\0';
    }
    char norm[UM98X_CMD_MAX];
    if (!um98x_cmd_normalise(cmd, norm, sizeof(norm))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lease == NULL && !um98x_stream_query_allowed(norm)) {
        return ESP_ERR_INVALID_STATE;   /* configuring commands and CONFIG need a COMMAND lease */
    }
    esp_err_t err = call_begin(lease, norm);
    if (err != ESP_OK) {
        return err;
    }
    call_t c;
    call_setup(&c, lease);
    um98x_res_t res = um98x_exec_command(&c.ex, norm, timeout_ms, reply, reply != NULL ? reply_len : 0);
    call_end();
    return to_esp_err(res);
}

esp_err_t um98x_ctl_apply_profile(gnss_link_lease_t lease, const um98x_base_profile_t *profile,
                                  const um98x_apply_opts_t *opts, um98x_apply_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    if (!um98x_profile_valid(profile)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lease == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = call_begin(lease, NULL);
    if (err != ESP_OK) {
        return err;
    }
    um98x_info_t known;
    portENTER_CRITICAL(&s_lock);
    known = s_info;
    portEXIT_CRITICAL(&s_lock);
    gnss_versiona_t identity;
    versiona_from_info(&known, &identity);

    call_t c;
    call_setup(&c, lease);
    um98x_res_t res = um98x_exec_apply(&c.ex, profile, opts, &identity, stored_baud(), result);
    call_end();

    if (identity.model[0] != '\0') {
        remember_identity(&identity);   /* this session's (written only on change) */
    }
    if (result->answer_baud != 0 && res != UM98X_OK) {
        persist_baud(result->answer_baud);   /* where it last answered (a rollback's RESET may have moved it) */
    }
    if (res == UM98X_OK) {
        result->persisted = persist_applied(profile, result);
        ESP_LOGI(TAG, "profile applied (signal group %s, base mode %s, baud %u%s, 1230 %s)",
                 result->signal_group_sent ? "set" : "kept", result->mode_sent ? "sent" : "kept",
                 (unsigned)result->baud, result->baud_changed ? " changed" : "",
                 result->rtcm1230_ok ? "on" : "rejected");
        /* A success refused only optional messages (um98x_rtcm_optional): this
         * firmware lacks them. 1230 has its own word in the line above. */
        for (uint8_t i = 0; i < result->rtcm_refused_n && i < UM98X_MAX_RTCM_MSGS; i++) {
            if (result->rtcm_refused[i] != UM98X_RTCM_GLO_BIASES) {
                ESP_LOGW(TAG, "RTCM%u refused by this receiver firmware: applied without it",
                         (unsigned)result->rtcm_refused[i]);
            }
        }
    } else {
        ESP_LOGW(TAG, "profile apply failed at \"%s\": %s%s", result->failed_cmd, result->reason,
                 result->rolled_back ? (result->rollback_answered ? " (rolled back)" : " (rolled back, no answer)")
                                     : "");
    }
    return to_esp_err(res);
}

size_t um98x_ctl_apply_preview(const um98x_base_profile_t *profile, uint32_t link_baud, um98x_preview_step_t *out,
                               size_t cap)
{
    return um98x_plan_preview(profile, link_baud, out, cap);
}

esp_err_t um98x_ctl_restore_output(gnss_link_lease_t lease, const um98x_base_profile_t *profile,
                                   const um98x_apply_result_t *failed, um98x_apply_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Copied first: result may be the failed session's own result. */
    uint16_t skip[UM98X_MAX_RTCM_MSGS];
    size_t n_skip = 0;
    if (failed != NULL) {
        n_skip = failed->rtcm_refused_n < UM98X_MAX_RTCM_MSGS ? failed->rtcm_refused_n : UM98X_MAX_RTCM_MSGS;
        memcpy(skip, failed->rtcm_refused, n_skip * sizeof(skip[0]));
    }
    memset(result, 0, sizeof(*result));
    if (!um98x_profile_valid(profile)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lease == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = call_begin(lease, NULL);
    if (err != ESP_OK) {
        return err;
    }
    call_t c;
    call_setup(&c, lease);
    for (size_t i = 0; i < n_skip; i++) {
        ESP_LOGI(TAG, "RTCM%u left out of the restore: the receiver refused it in the failed apply", (unsigned)skip[i]);
    }
    um98x_res_t res = um98x_exec_restore_rtcm(&c.ex, profile, skip, n_skip, result);
    call_end();
    if (res != UM98X_OK) {
        ESP_LOGW(TAG, "RTCM output not fully restored: \"%s\": %s", result->failed_cmd, result->reason);
    }
    return to_esp_err(res);
}

esp_err_t um98x_ctl_reset_receiver(gnss_link_lease_t lease)
{
    if (lease == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (call_begin(lease, NULL) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;   /* nothing sent */
    }
    call_t c;
    call_setup(&c, lease);
    um98x_res_t res = um98x_exec_reset(&c.ex);
    call_end();
    if (res == UM98X_E_LINK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (res != UM98X_OK) {
        ESP_LOGW(TAG, "no answer within 20 s of the receiver's restart");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t um98x_ctl_sat_view(gnss_link_lease_t lease, bool on, uint16_t period_s, char *reason, size_t reason_len)
{
    if (reason != NULL && reason_len > 0) {
        reason[0] = '\0';
    }
    if (on && (period_s == 0 || period_s > UM98X_SAT_VIEW_PERIOD_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lease == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = call_begin(lease, NULL);
    if (err != ESP_OK) {
        return err == ESP_ERR_TIMEOUT ? ESP_ERR_INVALID_STATE : err;
    }
    call_t c;
    call_setup(&c, lease);
    um98x_res_t res = um98x_exec_sat_view(&c.ex, on, period_s, reason, reason_len);
    call_end();
    if (res != UM98X_OK) {
        ESP_LOGW(TAG, "satellite-in-view output %s: %s", on ? "not turned on" : "not turned off",
                 reason != NULL && reason[0] != '\0' ? reason : "failed");
    }
    return to_esp_err(res);
}

esp_err_t um98x_ctl_get_base_position(um98x_base_position_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    gnss_obs_arp_t arp;
    esp_err_t err = gnss_obs_get_arp(&arp);
    if (err != ESP_OK) {
        return err;
    }
    if (!arp.valid) {
        return ESP_ERR_NOT_FOUND;
    }
    return um98x_arp_to_position(arp.ecef_01mm, out) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t um98x_ctl_lock_position(const um98x_base_profile_t *cur, int64_t now_unix, um98x_base_profile_t *next,
                                  um98x_base_position_t *pos)
{
    if (cur == NULL || next == NULL || pos == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    gnss_obs_arp_t arp;
    esp_err_t err = gnss_obs_get_arp(&arp);
    if (err != ESP_OK) {
        return err;
    }
    if (!arp.valid) {
        return ESP_ERR_NOT_FOUND;
    }
    return um98x_lock_profile(cur, arp.ecef_01mm, now_unix, next, pos) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t um98x_ctl_check_position(const um98x_base_profile_t *profile, uint32_t max_age_ms)
{
    if (!um98x_profile_valid(profile) || profile->base_mode != UM98X_BASE_FIXED) {
        return ESP_ERR_INVALID_ARG;
    }
    gnss_obs_arp_t arp;
    esp_err_t err = gnss_obs_get_arp(&arp);
    if (err != ESP_OK) {
        return err;
    }
    if (!arp.valid || arp.age_ms > max_age_ms) {
        return ESP_ERR_NOT_FOUND;
    }
    return um98x_arp_near_profile(profile, arp.ecef_01mm, UM98X_LOCK_VERIFY_TOL_M) ? ESP_OK
                                                                                   : ESP_ERR_INVALID_RESPONSE;
}

static uint32_t age_ms(int64_t then, int64_t now)
{
    if (then == UM98X_NONE_MS) {
        return UINT32_MAX;
    }
    int64_t d = now - then;
    return d < 0 ? 0 : (d >= UINT32_MAX ? UINT32_MAX - 1 : (uint32_t)d);
}

esp_err_t um98x_ctl_get_stats(um98x_ctl_stats_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    int64_t now = mono_ms();
    portENTER_CRITICAL(&s_lock);
    um98x_chan_stats_t st = s_chan.stats;
    int64_t answer = s_chan.last_answer_ms;
    int64_t restart = s_chan.last_restart_ms;
    bool restarting = um98x_chan_restarting(&s_chan, now);
    int64_t version = s_chan.last_version_ms;
    uint32_t uptime = s_chan.version_uptime_ms;
    portEXIT_CRITICAL(&s_lock);
    if (!s_inited) {
        answer = UM98X_NONE_MS;
        restart = UM98X_NONE_MS;
        restarting = false;
        version = UM98X_NONE_MS;
    }
    if (version == UM98X_NONE_MS) {
        uptime = UINT32_MAX;
    }
    out->lines = st.lines;
    out->matched = st.matched;
    out->unmatched = st.unmatched;
    out->bad = st.bad;
    out->other = st.other;
    out->banners = st.banners;
    out->resetting = st.resetting;
    out->restarts = st.restarts;
    out->restarting = restarting;
    out->last_answer_age_ms = age_ms(answer, now);
    out->last_restart_age_ms = age_ms(restart, now);
    out->version_age_ms = age_ms(version, now);
    out->rx_uptime_ms = uptime;
    return ESP_OK;
}
