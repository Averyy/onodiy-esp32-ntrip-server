/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */
#include "web_shell_error_build.h"
void web_build_error(wj_t *w, web_err_t err, const char *message, const char *field, uint32_t retry_in_s,
                     const char *by, const char *reason)
{
    wj_obj_open(w, NULL);
    wj_obj_open(w, "error");
    wj_str(w, "code", web_err_code(err));
    wj_str(w, "message", message != NULL ? message : web_err_message(err));
    wj_str(w, "field", field);
    wj_u32_or_null(w, "retry_in_s", retry_in_s);
    wj_str(w, "by", by);
    if (reason != NULL) {
        wj_str(w, "reason", reason);
    }
    wj_obj_close(w);
    wj_obj_close(w);
}

