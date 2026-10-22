/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Commands that older receiver builds refuse (um98x_caps.h).
 */
#include "um98x_caps.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    const char *model;
    uint8_t cap;
    uint32_t min_build;
} cap_row_t;

/* UM980 11833: "RTCM1230 COM1 1" and "ANT1GROUPA" refused; 23980 takes both. */
static const cap_row_t k_rows[] = {
    { "UM980", UM98X_CAP_RTCM1230, 23980u },
    { "UM980", UM98X_CAP_GROUPS, 23980u },
};

static const char *const k_names[UM98X_CAP_COUNT] = {
    [UM98X_CAP_RTCM1230] = "rtcm1230",
    [UM98X_CAP_GROUPS] = "ant1groupa",
};

uint32_t um98x_cap_min_build(const char *model, um98x_cap_t cap)
{
    if (model == NULL) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(k_rows) / sizeof(k_rows[0]); i++) {
        if (k_rows[i].cap == (uint8_t)cap && strcmp(k_rows[i].model, model) == 0) {
            return k_rows[i].min_build;
        }
    }
    return 0;
}

bool um98x_cap_ok(const char *model, uint32_t build, um98x_cap_t cap)
{
    return build == 0 || build >= um98x_cap_min_build(model, cap);
}

const char *um98x_cap_name(um98x_cap_t cap)
{
    return (unsigned)cap < UM98X_CAP_COUNT ? k_names[cap] : NULL;
}
