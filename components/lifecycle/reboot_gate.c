/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "reboot_gate.h"

#include <stddef.h>
#include <string.h>

void reboot_gate_init(reboot_gate_t *g, unsigned max_inhibits)
{
    memset(g, 0, sizeof(*g));
    if (max_inhibits < 1) {
        max_inhibits = 1;
    }
    if (max_inhibits > REBOOT_GATE_MAX_INHIBITS) {
        max_inhibits = REBOOT_GATE_MAX_INHIBITS;
    }
    g->limit = max_inhibits;
}

const char *reboot_gate_holder(const reboot_gate_t *g)
{
    for (unsigned i = 0; i < g->limit; i++) {
        if (g->inhibit[i] != NULL) {
            return g->inhibit[i];
        }
    }
    return NULL;
}

static reboot_gate_result_t commit(reboot_gate_t *g, uint32_t reason, uint32_t flags, uint32_t *apply)
{
    g->rebooting = true;
    g->reason = reason;
    *apply = flags | g->queued_flags;   /* a queued request is part of this reboot */
    g->queued_reason = 0;
    g->queued_flags = 0;
    return REBOOT_GATE_COMMIT;
}

int reboot_gate_acquire(reboot_gate_t *g, const char *what)
{
    if (g->rebooting) {
        return REBOOT_GATE_TOKEN_REBOOTING;
    }
    for (unsigned i = 0; i < g->limit; i++) {
        if (g->inhibit[i] == NULL) {
            g->inhibit[i] = what;
            return (int)i + 1;
        }
    }
    return REBOOT_GATE_TOKEN_FULL;
}

reboot_gate_result_t reboot_gate_release(reboot_gate_t *g, int token, uint32_t *apply)
{
    *apply = 0;
    if (token < 1 || (unsigned)token > g->limit || g->inhibit[token - 1] == NULL) {
        return REBOOT_GATE_NONE;
    }
    g->inhibit[token - 1] = NULL;
    if (g->rebooting || g->queued_reason == 0 || reboot_gate_holder(g) != NULL) {
        return REBOOT_GATE_NONE;
    }
    return commit(g, g->queued_reason, 0, apply);
}

reboot_gate_result_t reboot_gate_request(reboot_gate_t *g, uint32_t reason, uint32_t flags, uint32_t *apply)
{
    *apply = 0;
    if (reason == 0) {
        return REBOOT_GATE_REFUSED;
    }
    if (g->rebooting) {
        *apply = flags;
        return REBOOT_GATE_BUSY;
    }
    if (reboot_gate_holder(g) != NULL) {
        if (g->queued_reason == 0) {
            g->queued_reason = reason;
        }
        g->queued_flags |= flags;
        return REBOOT_GATE_QUEUED;
    }
    return commit(g, reason, flags, apply);
}

reboot_gate_result_t reboot_gate_now(reboot_gate_t *g, uint32_t reason, uint32_t flags, bool force,
                                     uint32_t *apply)
{
    *apply = 0;
    if (reason == 0) {
        return REBOOT_GATE_REFUSED;
    }
    if (g->rebooting) {
        *apply = flags;
        return REBOOT_GATE_BUSY;
    }
    if (!force && reboot_gate_holder(g) != NULL) {
        return REBOOT_GATE_REFUSED;
    }
    return commit(g, reason, flags, apply);
}
