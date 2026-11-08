/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */
#include "web_shell_service.h"

web_shell_transfer_result_t web_shell_transfer(const web_shell_upload_service_t *s, void *ctx,
                                               size_t total, size_t prefix_size, void *buffer, size_t cap,
                                               web_shell_read_fn read, void *read_ctx)
{
    web_shell_transfer_result_t r = {.stage = WEB_SHELL_TRANSFER_INVALID, .left = total};
    if (prefix_size <= total) { r.received = prefix_size; r.left = total - prefix_size; }
    if (!s || !s->maximum || !s->preflight || !s->consume || !s->finalize || !s->abort || !read ||
        !buffer || !cap || !total || total > s->maximum || prefix_size > cap || prefix_size > total) return r;
    r.received = prefix_size;
    r.left = total - prefix_size;
    r.stage = WEB_SHELL_TRANSFER_PREFLIGHT;
    r.error = s->preflight(ctx, total, buffer, prefix_size);
    if (r.error) goto fail;
    if (prefix_size) {
        r.stage = WEB_SHELL_TRANSFER_CONSUME;
        r.error = s->consume(ctx, buffer, prefix_size);
        if (r.error) goto fail;
    }
    while (r.left) {
        r.stage = WEB_SHELL_TRANSFER_READ;
        size_t want = r.left < cap ? r.left : cap;
        int n = read(read_ctx, buffer, want);
        if (n <= 0 || (size_t)n > want) goto fail;
        r.left -= (size_t)n;
        r.received += (size_t)n;
        r.stage = WEB_SHELL_TRANSFER_CONSUME;
        r.error = s->consume(ctx, buffer, (size_t)n);
        if (r.error) goto fail;
        if (s->progress) s->progress(ctx, r.received, total);
    }
    r.stage = WEB_SHELL_TRANSFER_FINALIZE;
    r.error = s->finalize(ctx);
    if (r.error) goto fail;
    r.stage = WEB_SHELL_TRANSFER_DONE;
    return r;
fail:
    s->abort(ctx);
    return r;
}
