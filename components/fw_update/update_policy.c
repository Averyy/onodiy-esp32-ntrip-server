/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "update_policy.h"

#include <stddef.h>
#include <string.h>

#include "semver.h"

void fw_policy_app_offer(const char *running, const char *latest, bool (*is_bad)(const char *v, void *ctx),
                         void *bad_ctx, const char *stage1_version, fw_app_offer_t *out)
{
    memset(out, 0, sizeof(*out));
    semver_t run;
    semver_t lat;
    if (latest == NULL || latest[0] == '\0' || !semver_parse(running, &run) || !semver_parse(latest, &lat) ||
        semver_compare(&lat, &run) <= 0) {
        return;
    }
    if (is_bad != NULL && is_bad(latest, bad_ctx)) {
        out->rolled_back = true;
        return;
    }
    out->available = true;
    out->retry_after_stage1 = stage1_version != NULL && strcmp(stage1_version, latest) == 0;
}

bool fw_policy_app_install_allowed(const char *running, const char *offered)
{
    semver_t off;
    if (!semver_parse(offered, &off)) {
        return false;
    }
    semver_t run;
    if (!semver_parse(running, &run)) {
        return true;   /* dev build: no comparable version */
    }
    return semver_compare(&off, &run) > 0;
}

const fw_manifest_gnss_t *fw_policy_gnss_offer(const fw_manifest_t *m, const char *model, uint32_t current_build)
{
    if (m == NULL || model == NULL || model[0] == '\0' || current_build == 0) {
        return NULL;
    }
    const fw_manifest_gnss_t *best = NULL;
    for (size_t i = 0; i < m->gnss_count && i < FW_MANIFEST_MAX_GNSS; i++) {
        const fw_manifest_gnss_t *g = &m->gnss[i];
        if (strcmp(g->model, model) != 0 || g->build <= current_build) {
            continue;
        }
        if (best == NULL || g->build > best->build) {
            best = g;
        }
    }
    return best;
}

static uint32_t max_u32(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}

/* a + b without wrapping (uptimes are far below the limit; stay safe anyway). */
static uint32_t add_sat(uint32_t a, uint32_t b)
{
    return (a > UINT32_MAX - b) ? UINT32_MAX : a + b;
}

uint32_t fw_sched_wait_s(const fw_sched_t *s)
{
    uint32_t now = s->uptime_s;
    uint32_t due;   /* as an uptime */

    if (s->clock_trusted && s->last_check_unix > 0 && s->last_check_unix <= s->now_unix) {
        int64_t left = s->last_check_unix + (int64_t)s->interval_s - s->now_unix;
        due = (left <= 0) ? now : add_sat(now, (uint32_t)(left > (int64_t)UINT32_MAX ? UINT32_MAX : left));
    } else if (s->clock_trusted) {
        due = now;   /* never checked, or a stored time from the future */
    } else if (s->have_success) {
        due = add_sat(s->last_success_s, s->interval_s);
    } else {
        due = FW_SCHED_NO_CLOCK_WAIT_S;
    }
    /* At most one successful check per interval within a boot, whatever the clock does. */
    if (s->have_success) {
        due = max_u32(due, add_sat(s->last_success_s, s->interval_s));
    }

    uint32_t earliest = s->first_check_s;
    if (s->last_result == FW_CHECK_UNUSABLE) {
        earliest = max_u32(earliest, add_sat(s->last_attempt_s, s->interval_s));
    } else if (s->last_result == FW_CHECK_NO_RESPONSE) {
        earliest = max_u32(earliest, add_sat(s->last_attempt_s, FW_SCHED_RETRY_S));
    }
    due = max_u32(due, earliest);
    return due > now ? due - now : 0;
}

uint32_t fw_sched_jitter_s(const uint8_t mac[6])
{
    /* FNV-1a over the MAC: neighbouring MACs land far apart. */
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) {
        h ^= mac[i];
        h *= 16777619u;
    }
    return FW_SCHED_JITTER_MIN_S + h % (FW_SCHED_JITTER_SPAN_S + 1u);
}
