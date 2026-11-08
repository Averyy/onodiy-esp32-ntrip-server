/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Product-neutral Wi-Fi and diagnostic JSON builders.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "diag.h"
#include "network.h"
#include "web_json.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const network_status_t *net;
    const network_scan_entry_t *scan;
    size_t scan_count;
    uint32_t scan_age_s;           /**< UINT32_MAX = never scanned */
    bool scan_running;
    bool ap_password_set;
} web_wifi_snap_t;

/** GET /api/wifi. */
void web_build_wifi(wj_t *w, const web_wifi_snap_t *s);

/** GET /api/diag/tasks. */
void web_build_tasks(wj_t *w, const diag_task_info_t *t, size_t count);

/** GET /api/logs (JSON form); `level_max` the most detailed level this build logs ("info"). */
void web_build_logs(wj_t *w, uint32_t cursor, bool more, bool gap, const char *text, size_t len,
                    const char *level_max);

/* Shared formatting primitives for larger product-owned snapshots. */
void web_build_str_arr_or_null(wj_t *w, const char *key, const char *s, size_t cap);
void web_build_ladder_triggers(wj_t *w, const char *key, uint32_t bits);
void web_build_cred_fields(wj_t *w, const network_status_t *n);
void web_build_ap_close_in(wj_t *w, const char *key, const network_status_t *n);
void web_build_ap_reasons(wj_t *w, const char *key, bool active, uint32_t reasons);

#ifdef __cplusplus
}
#endif
