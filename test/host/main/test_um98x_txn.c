/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * One command in flight: timing, resends, late acknowledgements, data
 * completion. Driven with an injected clock that starts past 2^32 ms, so no
 * 32-bit millisecond arithmetic can hide in the core.
 */
#include <stdio.h>
#include <string.h>

#include "um98x_reply.h"
#include "um98x_test_vectors.h"
#include "um98x_txn.h"
#include "unity.h"

#define T0 ((int64_t)UINT32_MAX + 12345)

typedef struct {
    um98x_txn_t t;
    int64_t now;
    int sends;
    int64_t send_at[300];
} rig_t;

static void rig_begin(rig_t *r, um98x_txn_kind_t kind, const char *cmd, char *text, size_t cap)
{
    memset(r, 0, sizeof(*r));
    r->now = T0;
    um98x_txn_begin(&r->t, kind, cmd, T0, text, cap);
}

/* Step the clock to `until`, writing whenever the transaction asks.
 * @return the action of the last poll. */
static um98x_txn_action_t run_until(rig_t *r, int64_t until)
{
    for (;;) {
        int64_t wake;
        um98x_txn_action_t a = um98x_txn_poll(&r->t, r->now, &wake);
        if (a == UM98X_TXN_ACT_DONE) {
            return a;
        }
        if (a == UM98X_TXN_ACT_SEND) {
            TEST_ASSERT_LESS_THAN_INT(300, r->sends);
            r->send_at[r->sends++] = r->now;
            um98x_txn_sent(&r->t, r->now, true);
            continue;
        }
        TEST_ASSERT_GREATER_THAN_INT64(r->now, wake);
        if (wake > until) {
            r->now = until;
            return a;
        }
        r->now = wake;
    }
}

static bool feed(rig_t *r, const char *line)
{
    um98x_line_t l;
    um98x_line_classify(line, strlen(line), &l);
    return um98x_txn_on_line(&r->t, line, strlen(line), &l, r->now);
}

/* "$command,<echo>,response: <resp>*hh". */
static void ack(char *out, size_t cap, const char *echo, const char *resp)
{
    char tmp[200];
    snprintf(tmp, sizeof(tmp), "$command,%s,response: %s", echo, resp);
    snprintf(out, cap, "%s*%02X", tmp, um98x_xor8(tmp, strlen(tmp)));
}

static void dollar(char *out, size_t cap, const char *content)
{
    char tmp[200];
    snprintf(tmp, sizeof(tmp), "$%s", content);
    snprintf(out, cap, "%s*%02X", tmp, um98x_xor8(tmp, strlen(tmp)));
}

TEST_CASE("txn: a command completes on its acknowledgement", "[um98x_txn]")
{
    rig_t r;
    char text[128];
    char line[160];
    rig_begin(&r, UM98X_TXN_COMMAND, "UNLOG COM1", text, sizeof(text));
    run_until(&r, T0);
    TEST_ASSERT_EQUAL_INT(1, r.sends);
    TEST_ASSERT_FALSE(um98x_txn_leading_crlf(&r.t));

    r.now += 40;
    ack(line, sizeof(line), "UNLOG COM2", "OK");   /* another command's ack */
    TEST_ASSERT_FALSE(feed(&r, line));
    ack(line, sizeof(line), "unlog com1", "OK");
    TEST_ASSERT_TRUE(feed(&r, line));
    TEST_ASSERT_EQUAL(UM98X_TXN_OK, r.t.status);
    TEST_ASSERT_EQUAL_STRING(line, text);
    TEST_ASSERT_EQUAL_INT64(T0 + 40, r.t.done_ms);
    int64_t wake;
    TEST_ASSERT_EQUAL(UM98X_TXN_ACT_DONE, um98x_txn_poll(&r.t, r.now, &wake));
}

TEST_CASE("txn: pacing holds the first write, earlier lines are not replies", "[um98x_txn]")
{
    rig_t r;
    char line[160];
    memset(&r, 0, sizeof(r));
    r.now = T0;
    um98x_txn_begin(&r.t, UM98X_TXN_COMMAND, "SAVECONFIG", T0 + 100, NULL, 0);
    ack(line, sizeof(line), "SAVECONFIG", "OK");
    TEST_ASSERT_FALSE(feed(&r, line));   /* before our write: not ours */
    run_until(&r, T0 + 99);
    TEST_ASSERT_EQUAL_INT(0, r.sends);
    run_until(&r, T0 + 100);
    TEST_ASSERT_EQUAL_INT(1, r.sends);
    TEST_ASSERT_EQUAL_INT64(T0 + 100, r.send_at[0]);
    TEST_ASSERT_EQUAL_UINT32(UM98X_SAVE_TIMEOUT_MS, r.t.timeout_ms);
}

TEST_CASE("txn: timeouts resend 1000 ms after expiry, at most 4 sends", "[um98x_txn]")
{
    rig_t r;
    rig_begin(&r, UM98X_TXN_COMMAND, "RTCM1077 COM1 1", NULL, 0);
    TEST_ASSERT_EQUAL(UM98X_TXN_ACT_DONE, run_until(&r, T0 + 60000));
    TEST_ASSERT_EQUAL(UM98X_TXN_TIMEOUT, r.t.status);
    TEST_ASSERT_EQUAL_INT(4, r.sends);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT64(T0 + i * (UM98X_ACK_TIMEOUT_MS + UM98X_RETRY_GAP_MS), r.send_at[i]);
    }
    /* Worst case 4 x 3 s + 3 x 1 s. */
    TEST_ASSERT_EQUAL_INT64(T0 + 15000, r.t.done_ms);

    /* SAVECONFIG: 4 x 5 s + 3 x 1 s. */
    rig_begin(&r, UM98X_TXN_COMMAND, "saveconfig", NULL, 0);
    run_until(&r, T0 + 60000);
    TEST_ASSERT_EQUAL_INT(4, r.sends);
    TEST_ASSERT_EQUAL_INT64(T0 + 23000, r.t.done_ms);
}

TEST_CASE("txn: a late acknowledgement of an earlier send completes the command", "[um98x_txn]")
{
    rig_t r;
    char line[160];
    rig_begin(&r, UM98X_TXN_COMMAND, "RTCM1087 COM1 1", NULL, 0);
    run_until(&r, T0 + 4500);   /* first window expired, second send at T0+4000 */
    TEST_ASSERT_EQUAL_INT(2, r.sends);
    ack(line, sizeof(line), "RTCM1087 COM1 1", "OK");
    TEST_ASSERT_TRUE(feed(&r, line));
    TEST_ASSERT_EQUAL(UM98X_TXN_OK, r.t.status);

    /* Also between a timeout and its resend. */
    rig_begin(&r, UM98X_TXN_COMMAND, "RTCM1087 COM1 1", NULL, 0);
    run_until(&r, T0 + 3500);
    TEST_ASSERT_EQUAL_INT(1, r.sends);
    TEST_ASSERT_TRUE(feed(&r, line));
    TEST_ASSERT_EQUAL(UM98X_TXN_OK, r.t.status);
    TEST_ASSERT_EQUAL_INT(1, r.sends);
}

TEST_CASE("txn: a negative reply is resent once, the second is final", "[um98x_txn]")
{
    rig_t r;
    char line[160];
    rig_begin(&r, UM98X_TXN_COMMAND, "RTCM1230 COM1 1", NULL, 0);
    run_until(&r, T0);
    r.now += 50;
    ack(line, sizeof(line), "RTCM1230 COM1 1", "PARSING FAILD NO MATCHING FUNC  RTCM1230");
    TEST_ASSERT_TRUE(feed(&r, line));
    TEST_ASSERT_EQUAL(UM98X_TXN_PENDING, r.t.status);
    run_until(&r, T0 + 149);
    TEST_ASSERT_EQUAL_INT(1, r.sends);
    run_until(&r, T0 + 150);   /* 100 ms after the reply */
    TEST_ASSERT_EQUAL_INT(2, r.sends);
    r.now += 50;
    TEST_ASSERT_TRUE(feed(&r, line));
    TEST_ASSERT_EQUAL(UM98X_TXN_REJECTED, r.t.status);
    TEST_ASSERT_EQUAL_STRING("PARSING FAILD NO MATCHING FUNC  RTCM1230", r.t.reason);

    /* A negative reply then a positive one: accepted. */
    rig_begin(&r, UM98X_TXN_COMMAND, "MODE BASE 1 TIME 600 0", NULL, 0);
    run_until(&r, T0);
    ack(line, sizeof(line), "MODE BASE 1 TIME 600 0", "PARSING FAILD");
    feed(&r, line);
    run_until(&r, T0 + 100);
    ack(line, sizeof(line), "MODE BASE 1 TIME 600 0", "OK");
    feed(&r, line);
    TEST_ASSERT_EQUAL(UM98X_TXN_OK, r.t.status);

    /* A negative reply after the last allowed send is final. */
    rig_begin(&r, UM98X_TXN_COMMAND, "RTCM1005 COM1 1", NULL, 0);
    run_until(&r, T0 + 12100);   /* fourth send at T0+12000 */
    TEST_ASSERT_EQUAL_INT(4, r.sends);
    ack(line, sizeof(line), "RTCM1005 COM1 1", "PARSING FAILD");
    feed(&r, line);
    TEST_ASSERT_EQUAL(UM98X_TXN_REJECTED, r.t.status);
}

TEST_CASE("txn: negative replies do not end a version query early", "[um98x_txn]")
{
    rig_t r;
    char line[160];
    /* After a restart the receiver's parser answers the first polls with a
     * failure text while it boots: polling goes on until the log or end_ms. */
    rig_begin(&r, UM98X_TXN_VERSION_RESTART, "VERSIONA", NULL, 0);
    um98x_txn_set_end(&r.t, T0 + 20000);
    ack(line, sizeof(line), "VERSIONA", "PARSING FAILD NO MATCHING FUNC");
    run_until(&r, T0 + 50);
    TEST_ASSERT_TRUE(feed(&r, line));
    run_until(&r, T0 + 1050);
    TEST_ASSERT_TRUE(feed(&r, line));
    run_until(&r, T0 + 2050);
    TEST_ASSERT_TRUE(feed(&r, line));
    TEST_ASSERT_EQUAL(UM98X_TXN_PENDING, r.t.status);
    TEST_ASSERT_EQUAL_STRING("PARSING FAILD NO MATCHING FUNC", r.t.reason);
    run_until(&r, T0 + 3100);
    TEST_ASSERT_EQUAL_INT(4, r.sends);   /* still one poll a second */
    TEST_ASSERT_TRUE(feed(&r, TV_VERSIONA_UM982));
    TEST_ASSERT_EQUAL(UM98X_TXN_OK, r.t.status);

    /* Only failure texts until end_ms: REJECTED (the receiver answers). */
    rig_begin(&r, UM98X_TXN_VERSION_RESTART, "VERSIONA", NULL, 0);
    um98x_txn_set_end(&r.t, T0 + 5000);
    run_until(&r, T0 + 50);
    feed(&r, line);
    run_until(&r, T0 + 1050);
    feed(&r, line);
    TEST_ASSERT_EQUAL(UM98X_TXN_ACT_DONE, run_until(&r, T0 + 6000));
    TEST_ASSERT_EQUAL(UM98X_TXN_REJECTED, r.t.status);

    /* The plain query keeps its 3 tries as well. */
    rig_begin(&r, UM98X_TXN_VERSION, "VERSIONA", NULL, 0);
    run_until(&r, T0 + 50);
    feed(&r, line);
    run_until(&r, T0 + 3050);   /* second try at T0 + 3000 */
    feed(&r, line);
    TEST_ASSERT_EQUAL(UM98X_TXN_PENDING, r.t.status);
    TEST_ASSERT_EQUAL(UM98X_TXN_ACT_DONE, run_until(&r, T0 + 20000));
    TEST_ASSERT_EQUAL_INT(3, r.sends);
    TEST_ASSERT_EQUAL(UM98X_TXN_REJECTED, r.t.status);
}

TEST_CASE("txn: the base-mode query completes on its log, not on the ack", "[um98x_txn]")
{
    rig_t r;
    char line[160];
    rig_begin(&r, UM98X_TXN_MODE, "MODE", NULL, 0);
    run_until(&r, T0);
    r.now += 30;
    ack(line, sizeof(line), "MODE", "OK");
    TEST_ASSERT_TRUE(feed(&r, line));
    TEST_ASSERT_EQUAL(UM98X_TXN_PENDING, r.t.status);
    r.now += 20;
    TEST_ASSERT_TRUE(feed(&r, TV_MODE_SURVEY));
    TEST_ASSERT_EQUAL(UM98X_TXN_OK, r.t.status);
    TEST_ASSERT_EQUAL_STRING("MODE BASE 1234 TIME 600 0", r.t.mode_body);

    /* Ack but no log: resent like a command. */
    rig_begin(&r, UM98X_TXN_MODE, "MODE", NULL, 0);
    run_until(&r, T0 + 10);
    feed(&r, line);
    run_until(&r, T0 + 4000);
    TEST_ASSERT_EQUAL_INT(2, r.sends);

    /* A version log is not a base-mode reply. */
    TEST_ASSERT_FALSE(feed(&r, TV_VERSIONA_UM982));
}

TEST_CASE("txn: the configuration dump completes 500 ms after its last item", "[um98x_txn]")
{
    rig_t r;
    char text[512];
    char line[160];
    char item[160];
    rig_begin(&r, UM98X_TXN_CONFIG, "CONFIG", text, sizeof(text));
    run_until(&r, T0);
    r.now += 50;
    ack(line, sizeof(line), "CONFIG", "OK");
    TEST_ASSERT_TRUE(feed(&r, line));
    for (int i = 0; i < 3; i++) {
        r.now += 200;
        TEST_ASSERT_TRUE(feed(&r, TV_CONFIG_SG));
        dollar(item, sizeof(item), "CONFIG,COM1,CONFIG COM1 115200");
        TEST_ASSERT_TRUE(feed(&r, item));
    }
    int64_t last = r.now;
    TEST_ASSERT_EQUAL(UM98X_TXN_ACT_WAIT, run_until(&r, last + 499));
    TEST_ASSERT_EQUAL(UM98X_TXN_ACT_DONE, run_until(&r, last + 500));
    TEST_ASSERT_EQUAL(UM98X_TXN_OK, r.t.status);
    TEST_ASSERT_EQUAL_UINT16(6, r.t.config.items);
    TEST_ASSERT_TRUE(r.t.config.signal_group_known);
    TEST_ASSERT_EQUAL_UINT32(115200, r.t.config.com1_baud);
    TEST_ASSERT_NOT_NULL(strstr(text, "SIGNALGROUP 3 6"));
    TEST_ASSERT_EQUAL_INT(1, r.sends);
}

TEST_CASE("txn: the dump is capped 3000 ms after the ack and resent without items", "[um98x_txn]")
{
    rig_t r;
    char line[160];
    /* Items keep coming: cut at ack + 3000. */
    rig_begin(&r, UM98X_TXN_CONFIG, "CONFIG", NULL, 0);
    run_until(&r, T0);
    ack(line, sizeof(line), "CONFIG", "OK");
    feed(&r, line);
    int64_t t_ack = r.now;
    while (r.t.status == UM98X_TXN_PENDING && r.now < t_ack + 10000) {
        run_until(&r, r.now + 300);
        if (r.t.status == UM98X_TXN_PENDING) {
            feed(&r, TV_CONFIG_SG);
        }
    }
    TEST_ASSERT_EQUAL(UM98X_TXN_OK, r.t.status);
    TEST_ASSERT_EQUAL_INT64(t_ack + UM98X_CONFIG_MAX_MS, r.t.done_ms);

    /* Ack, no items: a timeout 3000 ms after the ack, then a resend. */
    rig_begin(&r, UM98X_TXN_CONFIG, "CONFIG", NULL, 0);
    run_until(&r, T0 + 100);
    feed(&r, line);
    run_until(&r, T0 + 100 + 3000 + 1000);
    TEST_ASSERT_EQUAL_INT(2, r.sends);
    TEST_ASSERT_EQUAL_INT64(T0 + 4100, r.send_at[1]);

    /* The ack was lost but the items arrive: still complete. */
    rig_begin(&r, UM98X_TXN_CONFIG, "CONFIG", NULL, 0);
    run_until(&r, T0 + 100);
    feed(&r, TV_CONFIG_SG);
    run_until(&r, T0 + 700);
    TEST_ASSERT_EQUAL(UM98X_TXN_OK, r.t.status);
}

TEST_CASE("txn: version query has 3 tries of 2000 ms, 1000 ms apart", "[um98x_txn]")
{
    rig_t r;
    char line[160];
    rig_begin(&r, UM98X_TXN_VERSION, "VERSIONA", NULL, 0);
    TEST_ASSERT_TRUE(um98x_txn_leading_crlf(&r.t));
    run_until(&r, T0 + 30000);
    TEST_ASSERT_EQUAL(UM98X_TXN_TIMEOUT, r.t.status);
    TEST_ASSERT_EQUAL_INT(3, r.sends);
    TEST_ASSERT_EQUAL_INT64(T0 + 3000, r.send_at[1]);
    TEST_ASSERT_EQUAL_INT64(T0 + 6000, r.send_at[2]);
    TEST_ASSERT_EQUAL_INT64(T0 + 8000, r.t.done_ms);   /* worst case about 8 s */
    TEST_ASSERT_FALSE(r.t.ack_seen);                   /* "no answer" */

    /* The ack alone does not complete it ("not ready"); the log does, also late. */
    rig_begin(&r, UM98X_TXN_VERSION, "VERSIONA", NULL, 0);
    run_until(&r, T0 + 10);
    ack(line, sizeof(line), "VERSIONA", "OK");
    TEST_ASSERT_TRUE(feed(&r, line));
    TEST_ASSERT_EQUAL(UM98X_TXN_PENDING, r.t.status);
    TEST_ASSERT_TRUE(r.t.ack_seen);
    run_until(&r, T0 + 2500);   /* in the gap after the first window */
    TEST_ASSERT_TRUE(feed(&r, TV_VERSIONA_UM982));
    TEST_ASSERT_EQUAL(UM98X_TXN_OK, r.t.status);
    TEST_ASSERT_TRUE(r.t.version_valid);
    TEST_ASSERT_EQUAL_STRING("UM982", r.t.version.model);
    TEST_ASSERT_EQUAL_INT(1, r.sends);
}

TEST_CASE("txn: after a restart the version is polled every second until the end", "[um98x_txn]")
{
    rig_t r;
    rig_begin(&r, UM98X_TXN_VERSION_RESTART, "VERSIONA", NULL, 0);
    um98x_txn_set_end(&r.t, T0 + UM98X_VERSION_RESTART_SPAN_MS);
    run_until(&r, T0 + 60000);
    TEST_ASSERT_EQUAL(UM98X_TXN_TIMEOUT, r.t.status);
    TEST_ASSERT_EQUAL_INT(20, r.sends);
    for (int i = 0; i < r.sends; i++) {
        TEST_ASSERT_EQUAL_INT64(T0 + i * 1000, r.send_at[i]);
    }
    TEST_ASSERT_EQUAL_INT64(T0 + 20000, r.t.done_ms);

    rig_begin(&r, UM98X_TXN_VERSION_RESTART, "VERSIONA", NULL, 0);
    um98x_txn_set_end(&r.t, T0 + 20000);
    run_until(&r, T0 + 7500);
    TEST_ASSERT_TRUE(feed(&r, TV_VERSIONA_UM982));
    TEST_ASSERT_EQUAL(UM98X_TXN_OK, r.t.status);
    TEST_ASSERT_EQUAL_INT(8, r.sends);
}

TEST_CASE("txn: a detection probe ends on any ack or the log, 2 sends", "[um98x_txn]")
{
    rig_t r;
    char line[160];
    rig_begin(&r, UM98X_TXN_PROBE, "VERSIONA", NULL, 0);
    run_until(&r, T0 + 10000);
    TEST_ASSERT_EQUAL(UM98X_TXN_TIMEOUT, r.t.status);
    TEST_ASSERT_EQUAL_INT(2, r.sends);
    TEST_ASSERT_EQUAL_INT64(T0 + 2000, r.send_at[1]);
    TEST_ASSERT_EQUAL_INT64(T0 + 4000, r.t.done_ms);

    rig_begin(&r, UM98X_TXN_PROBE, "VERSIONA", NULL, 0);
    run_until(&r, T0 + 10);
    ack(line, sizeof(line), "VERSIONA", "PARSING FAILD");
    TEST_ASSERT_TRUE(feed(&r, line));
    TEST_ASSERT_EQUAL(UM98X_TXN_OK, r.t.status);
}

TEST_CASE("txn: a restart banner ends a command that restarts the receiver", "[um98x_txn]")
{
    rig_t r;
    rig_begin(&r, UM98X_TXN_COMMAND, "CONFIG SIGNALGROUP 3 6", NULL, 0);
    r.t.restart_ends = true;
    run_until(&r, T0 + 500);
    TEST_ASSERT_TRUE(feed(&r, TV_BANNER));
    TEST_ASSERT_EQUAL(UM98X_TXN_RESTARTED, r.t.status);

    rig_begin(&r, UM98X_TXN_COMMAND, "RTCM1005 COM1 1", NULL, 0);
    run_until(&r, T0 + 500);
    TEST_ASSERT_FALSE(feed(&r, TV_BANNER));
    TEST_ASSERT_EQUAL(UM98X_TXN_PENDING, r.t.status);
}

TEST_CASE("txn: a failed write ends the transaction, abort stops it", "[um98x_txn]")
{
    rig_t r;
    rig_begin(&r, UM98X_TXN_COMMAND, "UNLOG COM1", NULL, 0);
    int64_t wake;
    TEST_ASSERT_EQUAL(UM98X_TXN_ACT_SEND, um98x_txn_poll(&r.t, r.now, &wake));
    um98x_txn_sent(&r.t, r.now, false);
    TEST_ASSERT_EQUAL(UM98X_TXN_LINK_ERROR, r.t.status);

    rig_begin(&r, UM98X_TXN_PROBE, "VERSIONA", NULL, 0);
    um98x_txn_abort(&r.t, r.now);
    TEST_ASSERT_EQUAL(UM98X_TXN_TIMEOUT, r.t.status);
    TEST_ASSERT_EQUAL(UM98X_TXN_ACT_DONE, um98x_txn_poll(&r.t, r.now, &wake));
}

TEST_CASE("txn: reply text is bounded by the caller's buffer", "[um98x_txn]")
{
    rig_t r;
    char text[16];
    char line[160];
    rig_begin(&r, UM98X_TXN_COMMAND, "UNLOG COM1", text, sizeof(text));
    run_until(&r, T0);
    ack(line, sizeof(line), "UNLOG COM1", "OK");
    feed(&r, line);
    TEST_ASSERT_EQUAL_size_t(sizeof(text) - 1, strlen(text));
    TEST_ASSERT_EQUAL_MEMORY(line, text, sizeof(text) - 1);
}
