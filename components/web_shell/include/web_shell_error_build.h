/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */
#pragma once
#include "web_json.h"
#include "web_shell_errors.h"

#ifdef __cplusplus
extern "C" {
#endif
/**
 * Error body. `message` NULL = the code's default text; `field`, `by`,
 * `reason` NULL = null / left out (reason only appears when set);
 * retry_in_s UINT32_MAX = null.
 */
void web_build_error(wj_t *w, web_err_t err, const char *message, const char *field, uint32_t retry_in_s,
                     const char *by, const char *reason);

#ifdef __cplusplus
}
#endif
