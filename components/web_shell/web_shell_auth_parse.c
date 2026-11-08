/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */
#include "web_shell_auth_parse.h"
static bool fail(web_bad_t *bad, web_err_t err, const char *field)
{
    if (bad != NULL) {
        bad->err = err;
        bad->field = field;
    }
    return false;
}

static bool invalid(web_bad_t *bad, const char *field)
{
    return fail(bad, WEB_ERR_INVALID_FIELD, field);
}

cJSON *web_parse_body(const char *body, size_t len, web_bad_t *bad)
{
    cJSON *root = web_parse_object(body, len);
    if (root == NULL) {
        fail(bad, WEB_ERR_BAD_JSON, NULL);
    }
    return root;
}

/* ---- session and setup -------------------------------------------------------------------- */

bool web_parse_password(const cJSON *root, const char *key, const char **out, web_bad_t *bad)
{
    const cJSON *it = web_member(root, key);
    if (it == NULL || !cJSON_IsString(it) || it->valuestring == NULL) {
        return invalid(bad, key);
    }
    *out = it->valuestring;
    return true;
}

bool web_parse_login(const cJSON *root, const char **pw, bool *remember, web_bad_t *bad)
{
    if (!web_parse_password(root, "password", pw, bad)) {
        return false;
    }
    *remember = false;
    if (web_member_bool(root, "remember", remember) == WEB_MEMBER_BAD) {
        return invalid(bad, "remember");
    }
    return true;
}

bool web_parse_confirm(const cJSON *root, web_bad_t *bad)
{
    bool confirm = false;
    if (web_member_bool(root, "confirm", &confirm) != WEB_MEMBER_OK || !confirm) {
        return fail(bad, WEB_ERR_CONFIRM_REQUIRED, "confirm");
    }
    return true;
}

