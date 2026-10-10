/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "app_trial.h"

#include <string.h>

#include "settings_codec.h"

#define TRIAL_BLOB_VERSION 1
#define BAD_BLOB_VERSION   1
#define REC_FLAG_BASELINE_KNOWN (1u << 0)

static uint16_t inc16(uint16_t v)
{
    return v == UINT16_MAX ? v : (uint16_t)(v + 1u);
}

static uint32_t add_sat(uint32_t a, uint32_t b)
{
    return (a > UINT32_MAX - b) ? UINT32_MAX : a + b;
}

static bool same(const char *a, const char *b)
{
    return a != NULL && b != NULL && a[0] != '\0' && strcmp(a, b) == 0;
}

/* ---- record codec -------------------------------------------------------------------- */

size_t fw_trial_rec_encode(const fw_trial_rec_t *r, uint8_t *buf, size_t cap)
{
    blob_writer_t w;
    blob_writer_init(&w, buf, cap);
    blob_put_u8(&w, TRIAL_BLOB_VERSION);
    blob_put_str(&w, r->version);
    blob_put_u8(&w, r->stage);
    blob_put_u8(&w, r->attempt);
    blob_put_u8(&w, r->baseline_known ? REC_FLAG_BASELINE_KNOWN : 0);
    blob_put_u8(&w, r->baseline);
    blob_put_u8(&w, r->streaming_slots);
    blob_put_i64(&w, r->installed_unix);
    blob_put_i64(&w, r->watch_start_unix);
    blob_put_u32(&w, r->watch_uptime_s);
    blob_put_u32(&w, r->confirm_uptime_s);
    blob_put_u16(&w, r->boots);
    blob_put_u16(&w, r->crashes);
    return blob_writer_ok(&w) ? w.len : 0;
}

bool fw_trial_rec_decode(const uint8_t *buf, size_t len, fw_trial_rec_t *out)
{
    fw_trial_rec_t r;
    memset(&r, 0, sizeof(r));
    blob_reader_t rd;
    blob_reader_init(&rd, buf, len);
    uint8_t ver = blob_get_u8(&rd);
    blob_get_str(&rd, r.version, sizeof(r.version));
    r.stage = blob_get_u8(&rd);
    r.attempt = blob_get_u8(&rd);
    uint8_t flags = blob_get_u8(&rd);
    r.baseline = blob_get_u8(&rd);
    r.streaming_slots = blob_get_u8(&rd);
    r.installed_unix = blob_get_i64(&rd);
    r.watch_start_unix = blob_get_i64(&rd);
    r.watch_uptime_s = blob_get_u32(&rd);
    r.confirm_uptime_s = blob_get_u32(&rd);
    r.boots = blob_get_u16(&rd);
    r.crashes = blob_get_u16(&rd);
    if (!blob_reader_ok(&rd) || ver < TRIAL_BLOB_VERSION || r.stage >= FW_TRIAL_REC_COUNT) {
        return false;
    }
    r.baseline_known = (flags & REC_FLAG_BASELINE_KNOWN) != 0;
    *out = r;
    return true;
}

void fw_trial_rec_new(fw_trial_rec_t *out, const fw_trial_rec_t *prev, const char *version, uint8_t baseline,
                      uint8_t streaming_slots, int64_t installed_unix)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->version, version != NULL ? version : "", sizeof(out->version));
    out->stage = FW_TRIAL_REC_PENDING;
    out->attempt = 1;
    if (prev != NULL && prev->stage == FW_TRIAL_REC_ROLLED_BACK && same(prev->version, out->version)) {
        out->attempt = 2;
    }
    out->baseline_known = true;
    out->baseline = baseline;
    out->streaming_slots = streaming_slots;
    out->installed_unix = installed_unix;
}

/* ---- rolled-back list ---------------------------------------------------------------- */

size_t fw_bad_encode(const fw_bad_versions_t *b, uint8_t *buf, size_t cap)
{
    blob_writer_t w;
    blob_writer_init(&w, buf, cap);
    uint8_t n = b->count > FW_BAD_VER_MAX ? FW_BAD_VER_MAX : b->count;
    blob_put_u8(&w, BAD_BLOB_VERSION);
    blob_put_u8(&w, n);
    for (uint8_t i = 0; i < n; i++) {
        blob_put_str(&w, b->v[i]);
    }
    return blob_writer_ok(&w) ? w.len : 0;
}

bool fw_bad_decode(const uint8_t *buf, size_t len, fw_bad_versions_t *out)
{
    fw_bad_versions_t b;
    memset(&b, 0, sizeof(b));
    blob_reader_t rd;
    blob_reader_init(&rd, buf, len);
    uint8_t ver = blob_get_u8(&rd);
    uint8_t n = blob_get_u8(&rd);
    if (!blob_reader_ok(&rd) || ver < BAD_BLOB_VERSION || n > FW_BAD_VER_MAX) {
        return false;
    }
    for (uint8_t i = 0; i < n; i++) {
        blob_get_str(&rd, b.v[i], sizeof(b.v[i]));
    }
    if (!blob_reader_ok(&rd)) {
        return false;
    }
    b.count = n;
    *out = b;
    return true;
}

bool fw_bad_contains(const fw_bad_versions_t *b, const char *version)
{
    if (b == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < b->count && i < FW_BAD_VER_MAX; i++) {
        if (same(b->v[i], version)) {
            return true;
        }
    }
    return false;
}

bool fw_bad_add(fw_bad_versions_t *b, const char *version)
{
    if (version == NULL || version[0] == '\0' || strlen(version) >= sizeof(b->v[0]) || fw_bad_contains(b, version)) {
        return false;
    }
    if (b->count >= FW_BAD_VER_MAX) {
        memmove(&b->v[0], &b->v[1], sizeof(b->v[0]) * (FW_BAD_VER_MAX - 1));
        b->count = FW_BAD_VER_MAX - 1;
    }
    strlcpy(b->v[b->count], version, sizeof(b->v[0]));
    b->count++;
    return true;
}

size_t fw_bad_newest_first(const fw_bad_versions_t *b, char out[][32], size_t cap)
{
    if (b == NULL || out == NULL) {
        return 0;
    }
    size_t n = b->count < FW_BAD_VER_MAX ? b->count : FW_BAD_VER_MAX;
    size_t k = 0;
    for (size_t i = n; i > 0 && k < cap; i--) {
        strlcpy(out[k++], b->v[i - 1], sizeof(out[0]));
    }
    return k;
}

/* ---- boot ------------------------------------------------------------------------- */

static void clear_rec(fw_trial_rec_t *rec)
{
    memset(rec, 0, sizeof(*rec));
}

/* Another boot of the image in stage 2 (it is running and confirmed). */
static void stage2_boot(fw_trial_rec_t *rec, const fw_trial_boot_in_t *in, fw_trial_boot_out_t *out)
{
    rec->boots = inc16(rec->boots);
    if (in->prev_uptime_s != UINT32_MAX && in->prev_uptime_s > rec->confirm_uptime_s) {
        rec->watch_uptime_s = add_sat(rec->watch_uptime_s, in->prev_uptime_s - rec->confirm_uptime_s);
    }
    rec->confirm_uptime_s = 0;
    if (in->crash_reset) {
        rec->crashes = inc16(rec->crashes);
    }
    out->persist = true;
    out->run = (rec->stage == FW_TRIAL_REC_WATCHING) ? FW_TRIAL_RUN_WATCHING : FW_TRIAL_RUN_VERIFYING;
    out->revert = in->crash_revert_count > 0 && rec->crashes >= in->crash_revert_count;
}

void fw_trial_on_boot(fw_trial_rec_t *rec, const fw_trial_boot_in_t *in, fw_trial_boot_out_t *out)
{
    memset(out, 0, sizeof(*out));
    const char *running = in->running_version;
    bool is_running = same(rec->version, running);

    if (in->pending_verify) {
        /* First boot of a new image: stage 1. Any reset before confirmation
         * rolls it back, so this is always the image's first boot. */
        if (rec->stage != FW_TRIAL_REC_PENDING || !is_running) {
            /* Activated without a matching record (power cut between
             * activation and the record write, or installed another way):
             * stage 2 then has no baseline to reach. Written at confirmation. */
            fw_trial_rec_t prev = *rec;
            fw_trial_rec_new(rec, &prev, running, 0, 0, 0);
            rec->baseline_known = false;
        }
        rec->boots = inc16(rec->boots);
        out->run = FW_TRIAL_RUN_CONFIRMING;
        return;
    }

    switch ((fw_trial_rec_stage_t)rec->stage) {
    case FW_TRIAL_REC_NONE:
    case FW_TRIAL_REC_COUNT:
        break;

    case FW_TRIAL_REC_PENDING:
        if (is_running) {
            /* Confirmed, but the record was not advanced (power cut right
             * after the confirmation): continue with stage 2. */
            rec->stage = FW_TRIAL_REC_VERIFYING;
            stage2_boot(rec, in, out);
        } else if (same(in->last_invalid_version, rec->version)) {
            /* The update this image installed was rolled back by the
             * bootloader during its first minute: a crash or any reset. */
            out->stage1_rollback = true;
            out->add_bad = rec->attempt >= 2;
            rec->stage = FW_TRIAL_REC_ROLLED_BACK;
            out->persist = true;
        } else {
            /* The activation never took effect (cut before the reboot). */
            clear_rec(rec);
            out->persist = true;
        }
        break;

    case FW_TRIAL_REC_VERIFYING:
    case FW_TRIAL_REC_WATCHING:
        if (is_running) {
            stage2_boot(rec, in, out);
        } else {
            /* Another image runs while this one was on trial: reverted
             * before its record was updated, or replaced another way. */
            if (same(in->last_invalid_version, rec->version)) {
                rec->stage = FW_TRIAL_REC_REVERTED;
                out->add_bad = true;
            } else {
                clear_rec(rec);
            }
            out->persist = true;
        }
        break;

    case FW_TRIAL_REC_REVERTED:
    case FW_TRIAL_REC_ROLLED_BACK:
        if (is_running) {
            /* The version given up on runs again (installed another way). */
            clear_rec(rec);
            out->persist = true;
        }
        break;
    }
}

/* ---- running trial -------------------------------------------------------------------- */

bool fw_trial_baseline_reached(const fw_trial_rec_t *rec, const fw_trial_obs_t *obs)
{
    if (!rec->baseline_known) {
        return true;
    }
    if ((rec->baseline & FW_BASELINE_STA_IP) && !obs->sta_online_seen) {
        return false;
    }
    if ((rec->baseline & FW_BASELINE_HUB_FED) && !obs->hub_fed_seen) {
        return false;
    }
    return (rec->streaming_slots & (uint8_t)~obs->streaming_seen) == 0;
}

bool fw_trial_baseline_excused(const fw_trial_rec_t *rec, const fw_trial_obs_t *obs)
{
    if (fw_trial_baseline_reached(rec, obs)) {
        return true;
    }
    if ((rec->baseline & FW_BASELINE_STA_IP) && !obs->sta_online_seen) {
        return false;   /* the network is not the receiver's fault */
    }
    /* No RTCM because of the receiver: the hub and the casters cannot be
     * judged, so they do not count against the image. */
    return !obs->hub_fed_seen && obs->receiver_problem;
}

fw_trial_action_t fw_trial_tick(const fw_trial_tick_in_t *in, const fw_trial_rec_t *rec, const fw_trial_obs_t *obs)
{
    switch (in->run) {
    case FW_TRIAL_RUN_CONFIRMING:
        return (in->init_complete && in->uptime_s >= in->confirm_s) ? FW_TRIAL_ACT_CONFIRM : FW_TRIAL_ACT_NONE;
    case FW_TRIAL_RUN_VERIFYING:
        if (fw_trial_baseline_reached(rec, obs)) {
            return FW_TRIAL_ACT_VERIFIED;
        }
        if (in->uptime_s >= in->window_start_s && in->uptime_s - in->window_start_s >= in->verify_window_s) {
            return fw_trial_baseline_excused(rec, obs) ? FW_TRIAL_ACT_VERIFIED : FW_TRIAL_ACT_REVERT;
        }
        return FW_TRIAL_ACT_NONE;
    case FW_TRIAL_RUN_WATCHING:
        return in->watch_elapsed_s >= in->watch_s ? FW_TRIAL_ACT_DONE : FW_TRIAL_ACT_NONE;
    case FW_TRIAL_RUN_NONE:
        break;
    }
    return FW_TRIAL_ACT_NONE;
}

uint32_t fw_trial_watch_elapsed_s(const fw_trial_rec_t *rec, uint32_t uptime_s, uint32_t stage2_start_s,
                                  bool clock_trusted, int64_t now_unix)
{
    uint32_t this_boot = uptime_s > stage2_start_s ? uptime_s - stage2_start_s : 0;
    uint32_t by_uptime = add_sat(rec->watch_uptime_s, this_boot);
    if (clock_trusted && rec->watch_start_unix > 0 && now_unix > rec->watch_start_unix) {
        int64_t wall = now_unix - rec->watch_start_unix;
        uint32_t by_clock = wall > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)wall;
        return by_clock > by_uptime ? by_clock : by_uptime;
    }
    return by_uptime;
}

uint32_t fw_trial_left_s(fw_trial_run_t run, uint32_t uptime_s, uint32_t confirm_s, uint32_t watch_elapsed_s,
                         uint32_t watch_s)
{
    uint32_t watch_left = watch_elapsed_s < watch_s ? watch_s - watch_elapsed_s : 0;
    switch (run) {
    case FW_TRIAL_RUN_CONFIRMING:
        return add_sat(uptime_s < confirm_s ? confirm_s - uptime_s : 0, watch_s);
    case FW_TRIAL_RUN_VERIFYING:
    case FW_TRIAL_RUN_WATCHING:
        return watch_left;
    case FW_TRIAL_RUN_NONE:
        break;
    }
    return 0;
}
