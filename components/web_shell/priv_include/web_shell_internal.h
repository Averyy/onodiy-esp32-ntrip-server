/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */
#pragma once
#include <stdatomic.h>
#include "web_shell.h"
#include "web_shell_auth_parse.h"
#include "web_shell_enums.h"
#include "web_conn.h"
#include "web_shell_registry.h"
#include "web_shell_auth.h"

/* ---- WebSocket (web_shell_ws.c) ---------------------------------------------------------------------------------- */

esp_err_t web_ws_init(void);

/** /ws route: pre-handshake check, post-handshake setup and the frame handler. */
esp_err_t web_ws_pre_handshake(httpd_req_t *req);
esp_err_t web_ws_post_handshake(httpd_req_t *req);
esp_err_t web_ws_handler(httpd_req_t *req);

/** A session closed (httpd close callback). */
void web_ws_on_close(int fd);

/** Close the clients of these sessions (bit mask of indexes) with 4001. */
void web_ws_close_sessions(uint32_t mask);

/** Close every client without a session (1008): public status was turned off. */
void web_ws_close_public(void);

/**
 * The sockets ran out and `fd` was chosen to make room: if it is a WebSocket
 * client, close it with 1013 (try again later). @return false if it is not one.
 */
bool web_ws_evict(int fd);

/** Periodic work on the server task: status at 1 Hz, progress at 2 Hz, log every 500 ms, events. */
void web_ws_tick(bool second, bool half_second);

/** Queue an event message for admin clients (server task or any task: copied). */
void web_ws_event(const char *json);

/** Send a progress message to the progress subscribers (server task). */

esp_err_t web_ws_publish(const char *topic, const char *json);

/** Reboot under way: send `rebooting` and close every client with 1012. */
void web_ws_rebooting(int reason);

/** Clients connected now. */
size_t web_ws_client_count(void);


const web_shell_config_t *web_shell_config(void);
void web_shell_admin_activity(void);
const web_shell_registry_t *web_shell_registry(void);
esp_err_t web_shell_worker_init(void);
int web_worker_pending_responses(void);
bool web_shell_worker_idle(void);
void web_shell_worker_accept(bool accept);
void web_shell_worker_interrupt(void);
esp_err_t web_shell_status_rest(web_req_t *r);
