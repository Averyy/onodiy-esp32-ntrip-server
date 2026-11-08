/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "settings.h"
#include "health.h"
#include "station.h"
#include "ntrip_server.h"
#include "fw_update.h"
#include "web.h"
#include "station_ui.h"
#include "web_internal.h"
#include "web_enums.h"
#include "web_parse.h"

static atomic_uint s_caster_changed;
static atomic_bool s_fw_changed;
/* A BOOT-button admin reset clears the stored password through settings
 * (whose change event reloads it); a password set while the settings were
 * unavailable lives only here, so the reset count is watched too. */
static void check_admin_reset(void)
{
    static uint32_t seen;
    static bool known;
    health_status_t hs;
    if (health_get_status(&hs) != ESP_OK) {
        return;
    }
    if (known && hs.admin_resets != seen) {
        web_auth_admin_reset();
    }
    seen = hs.admin_resets;
    known = true;
}

/* Station coalescing key: the event name and, for caster events, the slot. */
static size_t event_key(const char *json, char *key, size_t cap)
{
    const char *e = strstr(json, "\"event\":\"");
    if (e == NULL) {
        key[0] = '\0';
        return 0;
    }
    e += 9;
    size_t n = strcspn(e, "\"");
    const char *slot = strstr(json, "\"slot\":");
    size_t sl = slot != NULL ? strcspn(slot + 7, ",}") : 0;
    snprintf(key, cap, "%.*s/%.*s", (int)n, e, (int)sl, slot != NULL ? slot + 7 : "");
    return strlen(key);
}

static bool keyed(const char *json)
{
    /* State events replace an older one of the same kind still waiting; the rest are kept in order. */
    return strstr(json, "\"event\":\"station_state\"") != NULL || strstr(json, "\"event\":\"caster_state\"") != NULL ||
           strstr(json, "\"event\":\"update_state\"") != NULL;
}

static bool event_coalesce(void *ctx, const char *json, char *key, size_t cap)
{
    (void)ctx;
    return keyed(json) && event_key(json,key,cap) != 0;
}
static void event_json(const char *event, const char *key, const char *value)
{
    char ev[160];
    if (key) snprintf(ev,sizeof(ev),"{\"type\":\"event\",\"event\":\"%s\",\"%s\":\"%s\"}",event,key,value);
    else snprintf(ev,sizeof(ev),"{\"type\":\"event\",\"event\":\"%s\"}",event);
    web_ws_event(ev);
}
static void on_station(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == STATION_EVENT_STATE_CHANGED && data != NULL) {
        event_json("station_state", "state", web_station_state_name(*(const station_state_t *)data));
    } else if (id == STATION_EVENT_FLASH_PROGRESS && data != NULL) {
        web_ws_note_flash_progress((const um98x_flash_progress_t *)data, false);
    } else if (id == STATION_EVENT_FLASH_ENDED && data != NULL) {
        web_ws_note_flash_progress((const um98x_flash_progress_t *)data, true);
    } else if (id == STATION_EVENT_RECEIVER_RESTARTED) {
        event_json("receiver_restarted", NULL, NULL);
    }
    web_kick();
}

static void on_ntrip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == NTRIP_EVENT_STATE_CHANGED && data != NULL) {
        size_t slot = *(const size_t *)data;
        if (slot < WEB_MAX_SLOTS) {
            atomic_fetch_or(&s_caster_changed, 1u << slot);
            web_kick();
        }
    }
}

static void on_fw(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == FW_UPDATE_EVENT_STATE_CHANGED && data != NULL) {
        event_json("update_state", "state", web_fw_state_name(*(const fw_update_state_t *)data));
        atomic_store(&s_fw_changed, true);
        web_kick();
    }
}


static portMUX_TYPE s_flash_lock = portMUX_INITIALIZER_UNLOCKED;
static um98x_flash_progress_t s_flash;        /* latest progress of the running job */
static bool s_flash_new;
static um98x_flash_progress_t s_flash_final;  /* the job's last message, until it went out */
static bool s_flash_final_new;

void web_ws_note_flash_progress(const um98x_flash_progress_t *p, bool final)
{
    portENTER_CRITICAL(&s_flash_lock);
    if (final) {
        s_flash_final = *p;
        s_flash_final_new = true;
        s_flash_new = false;   /* progress before the end is superseded by it */
    } else {
        s_flash = *p;
        s_flash_new = true;
    }
    portEXIT_CRITICAL(&s_flash_lock);
}

static void push_flash_message(const um98x_flash_progress_t *p, bool final)
{
    /* Builds without receiver flashing only check packages: their job is gnss_check. */
    bool check = !station_flash_available();
    /* The final message says how the session (or the check) ended. */
    static EXT_RAM_BSS_ATTR station_status_t s_fst;
    const station_flash_last_t *last = NULL;
    if (final && station_get_status(&s_fst) == ESP_OK && s_fst.flash_last.outcome != STATION_FLASH_OUT_NONE) {
        last = &s_fst.flash_last;
    }
    web_progress_t m;
    web_flash_progress_msg(p, final, check, last, &m);
    web_ws_progress(&m);
}

static void push_flash_progress(void)
{
    um98x_flash_progress_t fin;
    um98x_flash_progress_t p;
    portENTER_CRITICAL(&s_flash_lock);
    bool have_final = s_flash_final_new;
    bool fresh = s_flash_new;
    s_flash_final_new = false;
    s_flash_new = false;
    fin = s_flash_final;
    p = s_flash;
    portEXIT_CRITICAL(&s_flash_lock);
    if (have_final) {
        push_flash_message(&fin, true);
    }
    if (fresh) {
        push_flash_message(&p, false);   /* a next job already reports */
    }
}

static uint32_t s_upload_seq;

static void push_upload_progress(void)
{
    web_upload_progress_t u;
    web_upload_progress(&u);
    if (u.job == NULL || u.seq == s_upload_seq) {
        return;
    }
    s_upload_seq = u.seq;
    web_progress_t m = {
        .job = u.job,
        .phase = u.phase,
        .pct = u.total > 0 ? (int)((uint64_t)u.bytes * 100u / u.total) : -1,
        .bytes = u.bytes,
        .total = u.total,
        .done = u.done,
        .result = u.result,
        .err = u.err,
    };
    web_ws_progress(&m);
}


void web_station_tick(void *ctx, bool second, bool half)
{
    (void)ctx;
    bool fw_event = atomic_exchange(&s_fw_changed, false);
    if (fw_event || half) {
        web_track_fw_state(fw_event);
    }
    unsigned changed = atomic_exchange(&s_caster_changed, 0u);
    for (size_t slot = 0; changed != 0 && slot < WEB_MAX_SLOTS; slot++) {
        if ((changed & (1u << slot)) == 0) {
            continue;
        }
        ntrip_caster_status_t *st = heap_caps_malloc(sizeof(*st), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (st == NULL) {
            st = malloc(sizeof(*st));
        }
        if (st != NULL && ntrip_server_get_status(slot, st) == ESP_OK) {
            char ev[96];
            snprintf(ev, sizeof(ev), "{\"type\":\"event\",\"event\":\"caster_state\",\"slot\":%u,\"state\":\"%s\"}",
                     (unsigned)slot, web_ntrip_state_name(st->state));
            web_ws_event(ev);
        }
        free(st);
    }
    if (second) {
        check_admin_reset();
    }
}
static void station_progress(void *ctx)
{
    (void)ctx;
    push_upload_progress();
    push_flash_progress();
}
void web_ws_progress(const web_progress_t *p)
{
    static EXT_RAM_BSS_ATTR char text[WEB_SHELL_STATUS_MAX]; wj_t w;
    wj_init(&w,text,sizeof(text),NULL,NULL);
    web_build_progress(&w,p);
    if (wj_ok(&w)) web_shell_http_progress(text);
}
static void admin_activity(void *ctx) { (void)ctx; health_note_admin_activity(); }
static void scan_done(void *ctx) { (void)ctx; web_note_scan_done(); }
static const char *setup_step_name(void *ctx, uint8_t step) { (void)ctx; return web_setup_step_name(step); }

static EXT_RAM_BSS_ATTR web_status_snap_t s_snapshot;
static bool capture(void *ctx, const void **snapshot, uint32_t *uptime)
{
    (void)ctx; web_snap_status(&s_snapshot); *snapshot=&s_snapshot; *uptime=s_snapshot.uptime_s; return true;
}
static void release(void *ctx, const void *snapshot) { (void)ctx; (void)snapshot; }
static bool caster_path(void *ctx, const char *path) { (void)ctx; return web_api_path_known(path); }

esp_err_t web_init(void)
{
    static const char *const origins[]={WEB_MAP_TILE_ORIGIN};
    web_shell_config_t cfg={
        .assets=station_ui_assets,
        .title=STATION_UI_TITLE,.default_brand=STATION_UI_DEFAULT_BRAND,
        .product_noun=STATION_UI_PRODUCT_NOUN,.project_name=STATION_UI_PROJECT_NAME,
        .browser_storage_prefix=STATION_UI_BROWSER_STORAGE_PREFIX,
        .storage={
            .password={SETTINGS_NS_SYS,SETTINGS_KEY_ADMIN_HASH},
            .sessions={SETTINGS_NS_WEB,SETTINGS_KEY_WEB_SESSIONS},
            .public_status={SETTINGS_NS_WEB,SETTINGS_KEY_WEB_PUBLIC},
            .setup_done={SETTINGS_NS_SYS,SETTINGS_KEY_SETUP_DONE},
            .setup_step={SETTINGS_NS_SYS,SETTINGS_KEY_SETUP_STEP},
            .hostname={SETTINGS_NS_SYS,SETTINGS_KEY_HOSTNAME},
        },
        .route_capacity=WEB_MAX_URI_HANDLERS-8,.section_capacity=WEB_SEC_COUNT+1,
        .status_bytes=WEB_SHELL_STATUS_MAX,.json_body_bytes=WEB_JSON_MAX_CA,
        .img_origins=origins,.img_origin_count=1,.connect_origins=origins,.connect_origin_count=1,
        .event_key=event_coalesce,.admin_activity=admin_activity,.tick=web_station_tick,.progress=station_progress,.scan_done=scan_done,.setup_step_name=setup_step_name,
    };
    esp_err_t err=web_shell_init(&cfg);
    if (err != ESP_OK) return err;
    web_shell_status_provider_t provider={.capture=capture,.release=release};
    err=web_shell_register_status(&provider);
    if (err != ESP_OK) return err;
    err=web_shell_register_sections(web_station_sections,web_station_section_count);
    if (err != ESP_OK) return err;
    for (size_t i=0;i<web_api_route_count;i++) {
        web_route_t route=web_api_routes[i];
        if (strchr(route.uri,'*')) route.path_known=caster_path;
        err=web_shell_register_routes(&route,1);
        if (err != ESP_OK) return err;
    }
    err=web_station_jobs_init();
    if (err != ESP_OK) return err;
    static const struct { const esp_event_base_t *base; esp_event_handler_t fn; } subs[]={
        {&STATION_EVENT,on_station},{&NTRIP_EVENT,on_ntrip},{&FW_UPDATE_EVENT,on_fw},
    };
    for (size_t i=0;i<sizeof(subs)/sizeof(subs[0]);i++) {
        err=esp_event_handler_register(*subs[i].base,ESP_EVENT_ANY_ID,subs[i].fn,NULL);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
esp_err_t web_start(void) { return web_shell_start(); }
esp_err_t web_stop(void) { return web_shell_stop(); }
