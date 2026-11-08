/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Bounded upload service adapter, executed only by the shared worker.
 * The product owns its service/writer and cancellation policy. Preflight runs
 * before consume (the optional inspected prefix has already been received).
 * Consume must consume the entire chunk or fail. Finalize consumes the writer
 * even on failure, or leaves it abortable; abort must tolerate a NULL/already
 * consumed writer. Every failure after preflight calls abort exactly once.
 * Progress is called after successful consumes; publish the final outcome in
 * the job callback after building the HTTP response. Service callbacks never
 * acquire/complete HTTP requests or retain the buffer. Context lives until the
 * accepted job finishes, including disconnect/stop cleanup.
 *
 * Pure transfer core: read is the runtime effect, returning >0 bytes or <=0
 * on disconnect/timeout. No allocation, RTOS, firmware or HTTP dependencies.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t maximum;
    int (*preflight)(void *ctx, size_t total, const void *prefix, size_t prefix_size);
    int (*consume)(void *ctx, const void *data, size_t size);
    int (*finalize)(void *ctx);
    void (*abort)(void *ctx);
    void (*progress)(void *ctx, size_t received, size_t total);
} web_shell_upload_service_t;

typedef enum {
    WEB_SHELL_TRANSFER_INVALID = 0,
    WEB_SHELL_TRANSFER_PREFLIGHT,
    WEB_SHELL_TRANSFER_READ,
    WEB_SHELL_TRANSFER_CONSUME,
    WEB_SHELL_TRANSFER_FINALIZE,
    WEB_SHELL_TRANSFER_DONE,
} web_shell_transfer_stage_t;

typedef struct {
    web_shell_transfer_stage_t stage;
    int error;              /* service error, zero for success or transport/argument failure */
    size_t received;
    size_t left;            /* announced bytes still unread; drain on a service refusal */
} web_shell_transfer_result_t;

typedef int (*web_shell_read_fn)(void *ctx, void *buffer, size_t want);
web_shell_transfer_result_t web_shell_transfer(const web_shell_upload_service_t *service, void *ctx,
                                               size_t total, size_t prefix_size, void *buffer, size_t cap,
                                               web_shell_read_fn read, void *read_ctx);

#ifdef __cplusplus
}
#endif
