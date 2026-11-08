/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 * One shared worker, queue capacity four, internal-RAM stack. HTTP task submits
 * a request or any task submits request=NULL work. The bounded payload is
 * copied before return, context is borrowed for firmware lifetime. Only the
 * shared worker API acquires/completes async requests and restores deadlines.
 * Async acquisition runs on the submitting HTTP task; completion runs on the
 * worker, or on the submitting task when enqueue fails.
 *
 * Callbacks run sequentially on the worker. They may use the owned request
 * for I/O until return; never complete it, queue it elsewhere, or retain it.
 * Return true when the answer/body is incomplete and the connection must end.
 * Submission failure leaves the original request with the caller; no callback
 * runs. Successful submission is accepted work, even after client disconnect.
 * Stop rejects new HTTP jobs, shuts request sockets to unblock I/O, executes
 * queued callbacks, and waits for cleanup before stopping HTTP. Background
 * jobs without a request may continue (for example, staged-resource expiry).
 * Callbacks must bound waits.
 *
 * Upload admission is reserved once before product preflight. Submit with
 * upload=true transfers that reservation; failed submission or callback return
 * releases it. If no submission is attempted, the caller releases admission.
 * The service owns firmware cancellation and must abort its writer on I/O or
 * finalization failure. Shared receive retries are bounded, refused uploads
 * drain before replying, and final progress should be published before return.
 */
#pragma once
#include "web_shell_http.h"

#ifdef __cplusplus
extern "C" {
#endif
#define WEB_SHELL_JOB_BYTES 64u
#define WEB_SHELL_WORKER_QUEUE 4u

typedef bool (*web_shell_job_fn)(void *ctx, httpd_req_t *owned_request, const void *payload, size_t size);
esp_err_t web_shell_worker_submit(httpd_req_t *request, web_shell_job_fn fn, void *ctx,
                                 const void *payload, size_t size, bool upload);
bool web_shell_worker_upload_begin(void);
void web_shell_worker_upload_release(void);
bool web_shell_worker_upload_busy(void);
void web_shell_worker_response_hold(bool hold); /* a service may trigger reboot while finalizing */
int web_shell_worker_recv(httpd_req_t *req, void *buf, size_t want);
bool web_shell_worker_recv_exact(httpd_req_t *req, void *buf, size_t len);
bool web_shell_worker_drain(httpd_req_t *req, uint32_t left, void *buf, size_t cap);

#ifdef __cplusplus
}
#endif
