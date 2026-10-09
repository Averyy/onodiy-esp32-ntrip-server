/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Reboot gate: reboot inhibits and the queue/commit decision for reboot
 * requests (pure logic, no IDF includes, host-testable).
 *
 * Every decision ("commit now", "queue behind an inhibit", "refuse") is made
 * in one call, so the inhibit check and the commit can never be separated by
 * another task taking an inhibit in between. The caller holds its own lock
 * around each call (lifecycle uses a spinlock) and acts on the result after
 * releasing it.
 *
 * Rules:
 *  - Once a reboot is committed the gate is "rebooting": no inhibit can be
 *    taken any more, and every later request joins the reboot in progress
 *    (REBOOT_GATE_BUSY) instead of starting a second one.
 *  - Queued requests (reboot_gate_request) never fail: while an inhibit is
 *    held they wait (the first reason wins) and the release of the last
 *    inhibit commits them.
 *  - Immediate requests (reboot_gate_now) are refused while an inhibit is
 *    held, unless forced.
 *  - Flags travel with a request (lifecycle: "prepare the safe-mode retry",
 *    "clear safe mode") and come back in *apply only when the reboot they
 *    belong to is committed or already in progress, never for a refused or a
 *    still-queued request. The caller applies them under the same lock.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REBOOT_GATE_MAX_INHIBITS 16

/** reboot_gate_acquire() results that are not a token. */
#define REBOOT_GATE_TOKEN_FULL      0   /**< every inhibit slot is in use */
#define REBOOT_GATE_TOKEN_REBOOTING (-1) /**< a reboot is already committed */

typedef enum {
    REBOOT_GATE_NONE = 0,   /**< nothing to do */
    REBOOT_GATE_COMMIT,     /**< reboot committed: apply *apply, then run it (reason in gate.reason) */
    REBOOT_GATE_QUEUED,     /**< inhibited: committed by the release of the last inhibit */
    REBOOT_GATE_REFUSED,    /**< inhibited and not forced (or no reason): nothing changed */
    REBOOT_GATE_BUSY,       /**< a reboot is already committed: apply *apply and let that one run */
} reboot_gate_result_t;

typedef struct {
    unsigned limit;                                 /**< inhibit slots in use (<= REBOOT_GATE_MAX_INHIBITS) */
    const char *inhibit[REBOOT_GATE_MAX_INHIBITS];  /**< holder labels, NULL = free */
    uint32_t queued_reason;                         /**< 0 = nothing queued */
    uint32_t queued_flags;
    bool rebooting;                                 /**< a reboot is committed */
    uint32_t reason;                                /**< the committed reason */
} reboot_gate_t;

/** Empty gate with `max_inhibits` slots (clamped to 1..REBOOT_GATE_MAX_INHIBITS). */
void reboot_gate_init(reboot_gate_t *g, unsigned max_inhibits);

/** Take an inhibit. @return a token >= 1, REBOOT_GATE_TOKEN_FULL or REBOOT_GATE_TOKEN_REBOOTING. */
int reboot_gate_acquire(reboot_gate_t *g, const char *what);

/** Release an inhibit (unknown or already released tokens are ignored). COMMIT if a queued reboot is now due. */
reboot_gate_result_t reboot_gate_release(reboot_gate_t *g, int token, uint32_t *apply);

/** Reboot as soon as no inhibit is held: COMMIT, QUEUED or BUSY (REFUSED only for reason 0). */
reboot_gate_result_t reboot_gate_request(reboot_gate_t *g, uint32_t reason, uint32_t flags, uint32_t *apply);

/** Reboot now: COMMIT, BUSY, or REFUSED while inhibited and not forced (nothing queued then). */
reboot_gate_result_t reboot_gate_now(reboot_gate_t *g, uint32_t reason, uint32_t flags, bool force,
                                     uint32_t *apply);

/** Label of one inhibit holder, or NULL if none is held. */
const char *reboot_gate_holder(const reboot_gate_t *g);

#ifdef __cplusplus
}
#endif
