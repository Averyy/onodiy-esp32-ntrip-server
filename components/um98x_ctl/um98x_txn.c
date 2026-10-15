/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "um98x_txn.h"

#include <string.h>

static int64_t min64(int64_t a, int64_t b)
{
    return a < b ? a : b;
}

static void finish(um98x_txn_t *t, um98x_txn_status_t status, int64_t now_ms)
{
    t->status = status;
    t->done_ms = now_ms;
    t->next_send_ms = UM98X_NEVER;
    t->deadline_ms = UM98X_NEVER;
}

static void append_text(um98x_txn_t *t, const char *line, size_t len)
{
    if (t->text == NULL || t->text_cap == 0) {
        return;
    }
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        len--;
    }
    size_t room = t->text_cap - 1u - t->text_len;   /* text_len < text_cap always */
    if (t->text_len > 0 && room > 0) {
        t->text[t->text_len++] = '\n';
        room--;
    }
    if (len > room) {
        len = room;
    }
    memcpy(t->text + t->text_len, line, len);
    t->text_len += len;
    t->text[t->text_len] = '\0';
}

static void reset_text(um98x_txn_t *t)
{
    t->text_len = 0;
    if (t->text != NULL && t->text_cap > 0) {
        t->text[0] = '\0';
    }
}

static bool is_saveconfig(const char *cmd)
{
    static const char save[] = "SAVECONFIG";
    if (strlen(cmd) != sizeof(save) - 1u) {
        return false;
    }
    for (size_t i = 0; i < sizeof(save) - 1u; i++) {
        char c = cmd[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if (c != save[i]) {
            return false;
        }
    }
    return true;
}

void um98x_txn_begin(um98x_txn_t *t, um98x_txn_kind_t kind, const char *cmd, int64_t not_before_ms, char *text,
                     size_t text_cap)
{
    memset(t, 0, sizeof(*t));
    t->kind = kind;
    size_t n = strlen(cmd);
    if (n >= sizeof(t->cmd)) {
        n = sizeof(t->cmd) - 1u;
    }
    memcpy(t->cmd, cmd, n);
    t->cmd[n] = '\0';

    switch (kind) {
    case UM98X_TXN_VERSION:
        t->timeout_ms = UM98X_VERSION_WINDOW_MS;
        t->max_sends = UM98X_VERSION_TRIES;
        break;
    case UM98X_TXN_VERSION_RESTART:
        t->timeout_ms = UM98X_VERSION_RESTART_PERIOD_MS;
        t->max_sends = UINT8_MAX;   /* bounded by end_ms */
        break;
    case UM98X_TXN_PROBE:
        t->timeout_ms = UM98X_PROBE_WINDOW_MS;
        t->max_sends = UM98X_PROBE_SENDS;
        break;
    case UM98X_TXN_GROUPS:
        t->timeout_ms = UM98X_ACK_TIMEOUT_MS;
        t->max_sends = UM98X_GROUPS_SENDS;
        break;
    default:
        t->timeout_ms = is_saveconfig(t->cmd) ? UM98X_SAVE_TIMEOUT_MS : UM98X_ACK_TIMEOUT_MS;
        t->max_sends = UM98X_MAX_SENDS;
        break;
    }
    t->status = UM98X_TXN_PENDING;
    t->first_sent_ms = UM98X_NEVER;
    t->next_send_ms = not_before_ms;
    t->deadline_ms = UM98X_NEVER;
    t->end_ms = UM98X_NEVER;
    t->ack_ms = UM98X_NEVER;
    t->last_item_ms = UM98X_NEVER;
    t->done_ms = UM98X_NEVER;
    t->text = text;
    t->text_cap = text_cap;
    reset_text(t);
}

void um98x_txn_set_end(um98x_txn_t *t, int64_t end_ms)
{
    t->end_ms = end_ms;
}

void um98x_txn_abort(um98x_txn_t *t, int64_t now_ms)
{
    if (t->status == UM98X_TXN_PENDING) {
        finish(t, UM98X_TXN_TIMEOUT, now_ms);
    }
}

bool um98x_txn_leading_crlf(const um98x_txn_t *t)
{
    return t->kind == UM98X_TXN_VERSION || t->kind == UM98X_TXN_VERSION_RESTART || t->kind == UM98X_TXN_PROBE;
}

static bool command_like(const um98x_txn_t *t)
{
    return t->kind == UM98X_TXN_COMMAND || t->kind == UM98X_TXN_MODE || t->kind == UM98X_TXN_CONFIG ||
           t->kind == UM98X_TXN_GROUPS;
}

um98x_txn_action_t um98x_txn_poll(um98x_txn_t *t, int64_t now_ms, int64_t *wake_ms)
{
    *wake_ms = UM98X_NEVER;
    if (t->status != UM98X_TXN_PENDING) {
        return UM98X_TXN_ACT_DONE;
    }

    /* Configuration dump: no end marker, so it is complete once the item
     * lines stop for a moment, and at the latest some time after the ack. */
    if (t->kind == UM98X_TXN_CONFIG && t->config.items > 0) {
        if (now_ms - t->last_item_ms >= UM98X_CONFIG_QUIET_MS ||
            (t->ack_ms != UM98X_NEVER && now_ms - t->ack_ms >= UM98X_CONFIG_MAX_MS)) {
            finish(t, UM98X_TXN_OK, now_ms);
            return UM98X_TXN_ACT_DONE;
        }
    }

    if (t->kind == UM98X_TXN_VERSION_RESTART && now_ms >= t->end_ms) {
        finish(t, t->rejects > 0 ? UM98X_TXN_REJECTED : UM98X_TXN_TIMEOUT, now_ms);
        return UM98X_TXN_ACT_DONE;
    }

    if (t->deadline_ms != UM98X_NEVER && now_ms >= t->deadline_ms) {
        /* The reply window of the last send expired. Retries are timed from
         * the expiry itself, not from when this poll noticed it. */
        int64_t expired = t->deadline_ms;
        t->deadline_ms = UM98X_NEVER;
        if (t->kind == UM98X_TXN_GROUPS && t->ack_seen) {
            /* Accepted, but no log this firmware reads: asking again
             * would only get the same. */
            finish(t, UM98X_TXN_TIMEOUT, now_ms);
            return UM98X_TXN_ACT_DONE;
        }
        if (t->sends >= t->max_sends) {
            /* A version query that only drew negative replies says so. */
            finish(t, (!command_like(t) && t->rejects > 0) ? UM98X_TXN_REJECTED : UM98X_TXN_TIMEOUT, now_ms);
            return UM98X_TXN_ACT_DONE;
        }
        if (command_like(t)) {
            t->next_send_ms = expired + UM98X_RETRY_GAP_MS;
        } else if (t->kind == UM98X_TXN_VERSION) {
            t->next_send_ms = expired + UM98X_VERSION_GAP_MS;
        } else {
            t->next_send_ms = expired;   /* probe: send once more at once */
        }
    }

    if (t->next_send_ms != UM98X_NEVER && now_ms >= t->next_send_ms) {
        return UM98X_TXN_ACT_SEND;
    }

    int64_t wake = min64(t->next_send_ms, t->deadline_ms);
    if (t->kind == UM98X_TXN_VERSION_RESTART) {
        wake = min64(wake, t->end_ms);
    }
    if (t->kind == UM98X_TXN_CONFIG && t->config.items > 0) {
        wake = min64(wake, t->last_item_ms + UM98X_CONFIG_QUIET_MS);
        if (t->ack_ms != UM98X_NEVER) {
            wake = min64(wake, t->ack_ms + UM98X_CONFIG_MAX_MS);
        }
    }
    if (wake == UM98X_NEVER) {
        /* Cannot happen for a pending transaction; never wait forever. */
        finish(t, UM98X_TXN_TIMEOUT, now_ms);
        return UM98X_TXN_ACT_DONE;
    }
    *wake_ms = wake;
    return UM98X_TXN_ACT_WAIT;
}

void um98x_txn_sent(um98x_txn_t *t, int64_t now_ms, bool ok)
{
    if (t->status != UM98X_TXN_PENDING) {
        return;
    }
    if (!ok) {
        finish(t, UM98X_TXN_LINK_ERROR, now_ms);
        return;
    }
    if (t->sends < UINT8_MAX) {
        t->sends++;
    }
    if (t->first_sent_ms == UM98X_NEVER) {
        t->first_sent_ms = now_ms;
    }
    t->next_send_ms = UM98X_NEVER;
    switch (t->kind) {
    case UM98X_TXN_VERSION_RESTART:
        t->deadline_ms = UM98X_NEVER;
        t->next_send_ms = now_ms + UM98X_VERSION_RESTART_PERIOD_MS;
        break;
    case UM98X_TXN_CONFIG:
        if (t->sends > 1) {
            /* A resend starts the read-back over. */
            memset(&t->config, 0, sizeof(t->config));
            reset_text(t);
            t->ack_ms = UM98X_NEVER;
            t->last_item_ms = UM98X_NEVER;
        }
        t->deadline_ms = now_ms + t->timeout_ms;
        break;
    default:
        t->deadline_ms = now_ms + t->timeout_ms;
        break;
    }
}

static void on_reject(um98x_txn_t *t, const um98x_line_t *l, int64_t now_ms)
{
    size_t n = l->text_len < sizeof(t->reason) - 1u ? l->text_len : sizeof(t->reason) - 1u;
    memcpy(t->reason, l->text, n);
    t->reason[n] = '\0';
    if (t->rejects < UINT8_MAX) {
        t->rejects++;
    }
    if (t->kind == UM98X_TXN_GROUPS) {
        finish(t, UM98X_TXN_REJECTED, now_ms);   /* a firmware without the query: every send alike */
        return;
    }
    if (!command_like(t)) {
        /* Version queries keep their own schedule (tries, or polling until
         * end_ms after a restart, whose parser may answer with a failure
         * text while it boots): they end on the log, the try count or
         * end_ms; the reason is kept. */
        return;
    }
    if (t->rejects >= UM98X_MAX_REJECTS || t->sends >= t->max_sends) {
        finish(t, UM98X_TXN_REJECTED, now_ms);
        return;
    }
    /* Possibly garbled on the way in: send it once more. */
    t->deadline_ms = UM98X_NEVER;
    t->next_send_ms = now_ms + UM98X_PACING_MS;
}

bool um98x_txn_on_line(um98x_txn_t *t, const char *line, size_t len, const um98x_line_t *l, int64_t now_ms)
{
    /* Nothing that arrived before our first write is a reply to it. */
    if (t->status != UM98X_TXN_PENDING || t->first_sent_ms == UM98X_NEVER) {
        return false;
    }
    switch (l->kind) {
    case UM98X_LINE_ACK:
        if (!um98x_echo_matches(l->echo, l->echo_len, t->cmd)) {
            return false;
        }
        t->ack_seen = true;
        if (t->kind == UM98X_TXN_COMMAND || t->kind == UM98X_TXN_PROBE) {
            append_text(t, line, len);
        }
        if (t->kind == UM98X_TXN_PROBE) {
            finish(t, UM98X_TXN_OK, now_ms);   /* any answer: a receiver is at this baud */
        } else if (!l->ok) {
            on_reject(t, l, now_ms);
        } else if (t->kind == UM98X_TXN_COMMAND) {
            finish(t, UM98X_TXN_OK, now_ms);
        } else if (t->kind == UM98X_TXN_GROUPS) {
            t->deadline_ms = now_ms + UM98X_GROUPS_LOG_MS;   /* the log follows the ack */
        } else if (t->kind == UM98X_TXN_CONFIG && t->ack_ms == UM98X_NEVER) {
            t->ack_ms = now_ms;
            if (t->config.items == 0) {
                t->deadline_ms = now_ms + UM98X_CONFIG_MAX_MS;
            }
        }
        return true;

    case UM98X_LINE_CONFIG:
        if (t->kind != UM98X_TXN_CONFIG) {
            return false;
        }
        um98x_config_item(&t->config, l->item, l->item_len, l->text, l->text_len);
        append_text(t, line, len);
        t->last_item_ms = now_ms;
        if (t->ack_ms == UM98X_NEVER) {
            t->ack_ms = now_ms;   /* the ack was lost: the first item stands in for it */
        }
        t->deadline_ms = UM98X_NEVER;   /* completion by the quiet gap or the cap */
        return true;

    case UM98X_LINE_MODE:
        if (t->kind != UM98X_TXN_MODE) {
            return false;
        }
        {
            size_t n = l->text_len < sizeof(t->mode_body) - 1u ? l->text_len : sizeof(t->mode_body) - 1u;
            memcpy(t->mode_body, l->text, n);
            t->mode_body[n] = '\0';
        }
        append_text(t, line, len);
        finish(t, UM98X_TXN_OK, now_ms);
        return true;

    case UM98X_LINE_VERSION:
        if (t->kind != UM98X_TXN_VERSION && t->kind != UM98X_TXN_VERSION_RESTART && t->kind != UM98X_TXN_PROBE) {
            return false;
        }
        t->version = l->version;
        t->version_valid = true;
        append_text(t, line, len);
        finish(t, UM98X_TXN_OK, now_ms);
        return true;

    case UM98X_LINE_BANNER:
        if (t->kind == UM98X_TXN_COMMAND && t->restart_ends) {
            append_text(t, line, len);
            finish(t, UM98X_TXN_RESTARTED, now_ms);
            return true;
        }
        return false;

    case UM98X_LINE_GROUPS:
        if (t->kind != UM98X_TXN_GROUPS || l->ant != 1) {
            return false;
        }
        {
            size_t n = l->text_len < sizeof(t->groups_body) - 1u ? l->text_len : sizeof(t->groups_body) - 1u;
            memcpy(t->groups_body, l->text, n);
            t->groups_body[n] = '\0';
        }
        append_text(t, line, len);
        finish(t, UM98X_TXN_OK, now_ms);
        return true;

    case UM98X_LINE_RESETTING:
        /* No echo: it answers whatever was sent while the receiver restarts. */
        t->restarting = true;
        append_text(t, line, len);
        if (t->kind == UM98X_TXN_PROBE) {
            finish(t, UM98X_TXN_OK, now_ms);   /* a receiver is at this baud */
        } else if (t->kind == UM98X_TXN_COMMAND && t->restart_ends) {
            finish(t, UM98X_TXN_RESTARTED, now_ms);   /* the restart is under way: no resend */
        }
        return true;

    default:
        return false;
    }
}
