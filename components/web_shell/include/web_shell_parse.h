/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Product-neutral JSON, typed member, PEM and WebSocket parsing.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Deepest nesting of objects and arrays a request may have (cJSON parses recursively). */
#define WEB_JSON_MAX_DEPTH 8u

/** True if the text nests objects and arrays at most max_depth deep (strings skipped). */
bool web_json_depth_ok(const char *text, size_t len, unsigned max_depth);

/** Typed member access: absent, valid, or invalid type/range. */
typedef enum { WEB_MEMBER_ABSENT, WEB_MEMBER_OK, WEB_MEMBER_BAD } web_member_t;

const cJSON *web_member(const cJSON *obj, const char *key);
web_member_t web_member_bool(const cJSON *obj, const char *key, bool *out);
/** A finite integer within [min, max]; fractions are refused. */
web_member_t web_member_int(const cJSON *obj, const char *key, int64_t min, int64_t max, int64_t *out);
/** A finite number within [min, max]. */
web_member_t web_member_double(const cJSON *obj, const char *key, double min, double max, double *out);
/** A string of at most max_len bytes without control characters. */
web_member_t web_member_str(const cJSON *obj, const char *key, size_t max_len, const char **out);

/** Parse an object, with the depth bound above; blank input is {}. NULL on failure. */
cJSON *web_parse_object(const char *body, size_t len);

/** Largest custom CA accepted (bytes of PEM text). */
#define WEB_CA_PEM_MAX 4096u

/** PEM text made only of CERTIFICATE blocks (and blank space), at most WEB_CA_PEM_MAX bytes. */
bool web_pem_certs_only(const char *pem);

/* ---- WebSocket ---------------------------------------------------------------------------------- */

#define WEB_TOPIC_STATUS   (1u << 0)
#define WEB_TOPIC_PROGRESS (1u << 1)
#define WEB_TOPIC_LOG      (1u << 2)

typedef enum {
    WEB_WS_MSG_INVALID = 0,
    WEB_WS_MSG_SUBSCRIBE,
    WEB_WS_MSG_PING,
} web_ws_msg_t;

/** What a subscribe message asks for. */
typedef struct {
    uint32_t topics;          /**< topic bits (unknown names ignored) */
    bool has_log_cursor;      /**< "log_cursor" given: send log lines after it */
    uint32_t log_cursor;
} web_ws_sub_t;

/**
 * A client text message; for subscribe, *sub gets the topics and the
 * optional log cursor (a "log_cursor" that is not an integer of 0..2^32-1
 * makes the message invalid).
 */
web_ws_msg_t web_parse_ws_message(const char *text, size_t len, web_ws_sub_t *sub);

#ifdef __cplusplus
}
#endif
