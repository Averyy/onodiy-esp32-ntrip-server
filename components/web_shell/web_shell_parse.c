/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "web_shell_parse.h"

#include <math.h>
#include <string.h>

#include "web_shell_core.h"

bool web_json_depth_ok(const char *text, size_t len, unsigned max_depth)
{
    unsigned depth = 0;
    bool in_str = false;
    bool esc = false;
    for (size_t i = 0; text != NULL && i < len; i++) {
        char c = text[i];
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
        } else if (c == '{' || c == '[') {
            if (++depth > max_depth) {
                return false;
            }
        } else if ((c == '}' || c == ']') && depth > 0) {
            depth--;
        }
    }
    return true;
}

cJSON *web_parse_object(const char *body, size_t len)
{
    bool blank = true;
    for (size_t i = 0; body != NULL && i < len; i++) {
        if (body[i] != ' ' && body[i] != '\t' && body[i] != '\r' && body[i] != '\n') {
            blank = false;
            break;
        }
    }
    /* cJSON recurses per level: a deep body must not reach the server's stack. */
    if (!blank && !web_json_depth_ok(body, len, WEB_JSON_MAX_DEPTH)) {
        return NULL;
    }
    cJSON *root = blank ? cJSON_CreateObject() : cJSON_ParseWithLength(body, len);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

const cJSON *web_member(const cJSON *obj, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

web_member_t web_member_bool(const cJSON *obj, const char *key, bool *out)
{
    const cJSON *it = web_member(obj, key);
    if (it == NULL) {
        return WEB_MEMBER_ABSENT;
    }
    if (!cJSON_IsBool(it)) {
        return WEB_MEMBER_BAD;
    }
    *out = cJSON_IsTrue(it);
    return WEB_MEMBER_OK;
}

web_member_t web_member_int(const cJSON *obj, const char *key, int64_t min, int64_t max, int64_t *out)
{
    const cJSON *it = web_member(obj, key);
    if (it == NULL) {
        return WEB_MEMBER_ABSENT;
    }
    if (!cJSON_IsNumber(it)) {
        return WEB_MEMBER_BAD;
    }
    double v = it->valuedouble;
    if (!isfinite(v) || v != floor(v) || v < (double)min || v > (double)max) {
        return WEB_MEMBER_BAD;
    }
    *out = (int64_t)v;
    return WEB_MEMBER_OK;
}

web_member_t web_member_double(const cJSON *obj, const char *key, double min, double max, double *out)
{
    const cJSON *it = web_member(obj, key);
    if (it == NULL) {
        return WEB_MEMBER_ABSENT;
    }
    if (!cJSON_IsNumber(it) || !isfinite(it->valuedouble) || it->valuedouble < min || it->valuedouble > max) {
        return WEB_MEMBER_BAD;
    }
    *out = it->valuedouble;
    return WEB_MEMBER_OK;
}

web_member_t web_member_str(const cJSON *obj, const char *key, size_t max_len, const char **out)
{
    const cJSON *it = web_member(obj, key);
    if (it == NULL) {
        return WEB_MEMBER_ABSENT;
    }
    if (!cJSON_IsString(it) || it->valuestring == NULL || strlen(it->valuestring) > max_len ||
        !web_no_ctrl(it->valuestring)) {
        return WEB_MEMBER_BAD;
    }
    *out = it->valuestring;
    return WEB_MEMBER_OK;
}

bool web_pem_certs_only(const char *pem)
{
    static const char begin[] = "-----BEGIN CERTIFICATE-----";
    static const char end[] = "-----END CERTIFICATE-----";
    if (pem == NULL) {
        return false;
    }
    size_t len = strlen(pem);
    if (len == 0 || len > WEB_CA_PEM_MAX) {
        return false;
    }
    size_t certs = 0;
    const char *p = pem;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (strncmp(p, begin, sizeof(begin) - 1) != 0) {
            return false;
        }
        p += sizeof(begin) - 1;
        const char *e = strstr(p, end);
        if (e == NULL) {
            return false;
        }
        /* The body is base64 and line breaks only. */
        for (const char *q = p; q < e; q++) {
            char ch = *q;
            bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '+' ||
                      ch == '/' || ch == '=' || ch == '\r' || ch == '\n';
            if (!ok) {
                return false;
            }
        }
        p = e + sizeof(end) - 1;
        certs++;
    }
    return certs > 0;
}

web_ws_msg_t web_parse_ws_message(const char *text, size_t len, web_ws_sub_t *sub)
{
    if (!web_json_depth_ok(text, len, WEB_JSON_MAX_DEPTH)) {
        return WEB_WS_MSG_INVALID;
    }
    cJSON *root = cJSON_ParseWithLength(text, len);
    web_ws_msg_t kind = WEB_WS_MSG_INVALID;
    const char *type = NULL;
    if (root != NULL && cJSON_IsObject(root) && web_member_str(root, "type", 16, &type) == WEB_MEMBER_OK) {
        if (strcmp(type, "ping") == 0) {
            kind = WEB_WS_MSG_PING;
        } else if (strcmp(type, "subscribe") == 0) {
            const cJSON *list = web_member(root, "topics");
            if (cJSON_IsArray(list)) {
                uint32_t bits = 0;
                const cJSON *t;
                cJSON_ArrayForEach(t, list) {
                    if (!cJSON_IsString(t)) {
                        continue;
                    }
                    if (strcmp(t->valuestring, "status") == 0) {
                        bits |= WEB_TOPIC_STATUS;
                    } else if (strcmp(t->valuestring, "progress") == 0) {
                        bits |= WEB_TOPIC_PROGRESS;
                    } else if (strcmp(t->valuestring, "log") == 0) {
                        bits |= WEB_TOPIC_LOG;
                    }
                }
                int64_t cur = 0;
                web_member_t mc = web_member_int(root, "log_cursor", 0, UINT32_MAX, &cur);
                if (mc != WEB_MEMBER_BAD) {
                    if (sub != NULL) {
                        sub->topics = bits;
                        sub->has_log_cursor = mc == WEB_MEMBER_OK;
                        sub->log_cursor = (uint32_t)cur;
                    }
                    kind = WEB_WS_MSG_SUBSCRIBE;
                }
            }
        }
    }
    cJSON_Delete(root);
    return kind;
}
