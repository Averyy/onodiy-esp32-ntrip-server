/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */
#pragma once
#include "web_shell_parse.h"
#include "web_shell_errors.h"

#ifdef __cplusplus
extern "C" {
#endif
/**
 * Why a body was refused: the error, the field it names (NULL = none) and,
 * where a parser words it, the field's rule ("at most 23 bytes"; NULL =
 * none: the code's default message).
 */
typedef struct {
    web_err_t err;
    const char *field;
    const char *rule;
} web_bad_t;

/**
 * Parse a JSON body. NULL (with bad = bad_json) for invalid JSON, nesting
 * deeper than WEB_JSON_MAX_DEPTH, or a non-object root; an empty body is {}.
 */
cJSON *web_parse_body(const char *body, size_t len, web_bad_t *bad);

/* ---- session and setup --------------------------------------------------------------- */

/** {"password": "..."}: a string (its rules are checked by the caller, which words the error). */
bool web_parse_password(const cJSON *root, const char *key, const char **out, web_bad_t *bad);

/** POST /api/session: {"password", "remember"} (remember optional, default false). */
bool web_parse_login(const cJSON *root, const char **pw, bool *remember, web_bad_t *bad);

/** {"confirm": true}. */
bool web_parse_confirm(const cJSON *root, web_bad_t *bad);

#ifdef __cplusplus
}
#endif
