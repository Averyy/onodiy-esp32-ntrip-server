/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Whole flash sessions against the simulated receiver: loader entry (baud
 * change, verification, restore, reset, stop key, limits, attempts), the
 * menu table, the post-transfer outcome table, the failure policy after the
 * point of no return, leaving the loader, the journal's state sequence, the
 * boot-time recovery table and its procedures, and the user's choices.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fl_session.h"
#include "fl_sim.h"
#include "flash_journal.h"
#include "unity.h"

static sim_t s_sim;
static fl_io_t s_io;
static fl_sess_t s_sess;
static uint8_t s_pkg[200u * 1024u + 77u];

#define MAX_STATES 256
static uint8_t s_states[MAX_STATES];
static unsigned s_nstates;
static uint8_t s_fail_state;      /* journal_put fails for this state (0 = never) */
static bool s_fail_always;
static bool s_copy_ok;
static bool s_cancel;
static unsigned s_prepares;
static bool s_prepare_ok;

static bool j_put(void *ctx, const um98x_flash_journal_t *j)
{
    (void)ctx;
    if (s_nstates < MAX_STATES) {
        s_states[s_nstates++] = j->state;
    }
    if (j->state == s_fail_state) {
        if (!s_fail_always) {
            s_fail_state = 0;
        }
        return false;
    }
    return true;
}

/* As on the target: without a RAM copy there is nothing whose digest could match. */
static bool copy_ok(void *ctx)
{
    (void)ctx;
    return s_copy_ok && s_sess.pkg != NULL;
}

static bool cancelled(void *ctx)
{
    (void)ctx;
    return s_cancel;
}

static bool recovery_prepare(void *ctx)
{
    (void)ctx;
    s_prepares++;
    if (!s_prepare_ok) {
        return false;   /* the pinned package fails the recovery gate: no copy */
    }
    s_sess.pkg = s_pkg;
    s_sess.pkg_len = sizeof(s_pkg);
    return true;
}

static fl_sess_hooks_t hooks(void)
{
    fl_sess_hooks_t h = {
        .journal_put = j_put,
        .copy_ok = copy_ok,
        .recovery_prepare = recovery_prepare,
        .cancelled = cancelled,
    };
    return h;
}

static void journal_fields(um98x_flash_journal_t *j)
{
    memset(j, 0, sizeof(*j));
    j->version = 1;
    j->size = sizeof(s_pkg);
    strcpy(j->expected_fw, "R4.10Build19152");
    j->expected_build = 19152;
    strcpy(j->pre_model, "UM982");
    strcpy(j->pre_fw, "R4.10Build18975");
    j->pre_build = 18975;
    j->app_baud = 115200;
}

static void note_print(void *ctx, const char *text)
{
    printf("  [%u] %s\n", (unsigned)sim_now_ms((sim_t *)ctx), text);
}

static void setup(void)
{
    sim_init(&s_sim);
    sim_io(&s_sim, &s_io);
    if (getenv("FL_VERBOSE") != NULL) {
        s_io.note = note_print;   /* the session log, for a failing test */
    }
    for (size_t i = 0; i < sizeof(s_pkg); i++) {
        s_pkg[i] = (uint8_t)(i * 13u + 7u);
    }
    s_nstates = 0;
    s_fail_state = 0;
    s_fail_always = false;
    s_copy_ok = true;
    s_cancel = false;
    s_prepares = 0;
    s_prepare_ok = true;
    journal_fields(&s_sess.j);
    fl_sess_hooks_t h = hooks();
    fl_sess_init(&s_sess, &s_io, s_pkg, sizeof(s_pkg), 460800, false, &h);
}

static bool states_include(uint8_t st)
{
    for (unsigned i = 0; i < s_nstates; i++) {
        if (s_states[i] == st) {
            return true;
        }
    }
    return false;
}

/* The main states in the order they were first written. */
static void assert_order(const uint8_t *want, unsigned n)
{
    unsigned k = 0;
    for (unsigned i = 0; i < s_nstates && k < n; i++) {
        if (s_states[i] == want[k]) {
            k++;
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(n, k, "journal states out of order");
}

static void assert_never_dangerous(void)
{
    static const int bad[] = { 3, 4, 5, 8, 9, 10 };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, s_sim.rx_menu[bad[i]], "a forbidden menu item was sent");
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, s_sim.rx_six_after_ponr, "\"6\" after a failed transfer");
}

static void assert_image(void)
{
    TEST_ASSERT_TRUE(s_sim.img_len >= sizeof(s_pkg));
    TEST_ASSERT_EQUAL_MEMORY(s_pkg, s_sim.img, sizeof(s_pkg));
}

TEST_CASE("session: a full update, start to the new version", "[flash]")
{
    setup();
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT32(19152, s_sess.out.ver.build);
    TEST_ASSERT_EQUAL_STRING("UM982", s_sess.out.ver.model);
    TEST_ASSERT_TRUE(s_sess.out.outcome_valid);
    TEST_ASSERT_EQUAL(FL_OUT_S, s_sess.out.outcome);
    TEST_ASSERT_EQUAL(XM_MODE_CRC, s_sess.out.xm_mode);
    static const uint8_t order[] = { UM98X_JOURNAL_PREPARED,     UM98X_JOURNAL_BAUD_CHANGING, UM98X_JOURNAL_RESET_SENT,
                                     UM98X_JOURNAL_MENU2_SENT,   UM98X_JOURNAL_TRANSFERRING,  UM98X_JOURNAL_EOT_SENT,
                                     UM98X_JOURNAL_VERIFYING };
    assert_order(order, sizeof(order));
    TEST_ASSERT_FALSE(states_include(UM98X_JOURNAL_FAILED));
    TEST_ASSERT_EQUAL_UINT8(1, s_sess.j.transfers);
    TEST_ASSERT_EQUAL_UINT8(1, s_sess.j.entry_attempts);
    TEST_ASSERT_EQUAL_UINT32(1, s_sim.rx_config_com1);   /* COM1 only, once, never saved */
    TEST_ASSERT_EQUAL_UINT32(1, s_sim.rx_resets);
    TEST_ASSERT_EQUAL_UINT32(32, s_sim.rx_max_burst);    /* 32-byte stop-key bursts at 460800 */
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_bursts_merged); /* each after the previous one drained, with a gap */
    TEST_ASSERT_EQUAL_UINT32(1, s_sim.rx_menu[2]);
    TEST_ASSERT_TRUE(s_sim.rx_six >= 1);
    assert_never_dangerous();
    assert_image();
    TEST_ASSERT_EQUAL(UM98X_FLASH_FINISHING, s_sess.prog.phase);
    TEST_ASSERT_EQUAL_UINT32(xm_blocks(sizeof(s_pkg)), s_sess.prog.frames_acked);
    sim_free(&s_sim);
}

TEST_CASE("session: an application already at the loader baud skips the baud change", "[flash]")
{
    setup();
    s_sim.app_baud = 460800;
    s_sim.com1_baud = 460800;
    s_sim.host_baud = 460800;
    s_sess.j.app_baud = 460800;
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_run(&s_sess));
    TEST_ASSERT_FALSE(states_include(UM98X_JOURNAL_BAUD_CHANGING));
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_config_com1);
    sim_free(&s_sim);
}

TEST_CASE("session: the prompt split across reads is still found", "[flash]")
{
    setup();
    s_sim.split_prompt = true;
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_run(&s_sess));
    sim_free(&s_sim);
}

TEST_CASE("session: the application restarts instead of the loader: three attempts, then the abort path", "[flash]")
{
    setup();
    s_sim.loader_reachable = false;
    TEST_ASSERT_EQUAL(FL_SESS_ABORTED, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT8(3, s_sess.j.entry_attempts);
    TEST_ASSERT_EQUAL_UINT32(3, s_sim.rx_resets);
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_frames);
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_menu[2]);
    TEST_ASSERT_FALSE(states_include(UM98X_JOURNAL_MENU2_SENT));
    TEST_ASSERT_EQUAL(FL_ENTRY_APP, FL_ENTRY_APP);
    TEST_ASSERT_EQUAL_UINT32(18975, s_sess.out.ver.build);
    TEST_ASSERT_EQUAL(UM98X_FLASH_FAIL_ENTRY, s_sess.out.failure);
    sim_free(&s_sim);
}

TEST_CASE("session: data 8 s after the reset without the prompt abandons the attempt early", "[flash]")
{
    setup();
    s_sim.loader_reachable = false;
    s_sim.late_text_after_reset = true;
    s_sim.banner = false;
    uint32_t t0 = sim_now_ms(&s_sim);
    TEST_ASSERT_EQUAL(FL_SESS_ABORTED, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT32(3, s_sim.rx_resets);
    /* Three attempts of ~9 s each (not 30 s), plus the waits for the application. */
    TEST_ASSERT_TRUE(sim_now_ms(&s_sim) - t0 < 3u * 30000u);
    sim_free(&s_sim);
}

TEST_CASE("session: nothing at all after the reset: 30 s per attempt, receiver gone: needs the user", "[flash]")
{
    setup();
    s_sim.reset_silent = true;
    s_sim.loader_reachable = false;
    uint32_t t0 = sim_now_ms(&s_sim);
    TEST_ASSERT_EQUAL(FL_SESS_NEEDS_USER, fl_sess_run(&s_sess));
    TEST_ASSERT_TRUE(sim_now_ms(&s_sim) - t0 >= 30000u);
    TEST_ASSERT_EQUAL_UINT32(1, s_sim.rx_resets);
    TEST_ASSERT_EQUAL(UM98X_JOURNAL_NEEDS_USER, s_sess.j.state);
    TEST_ASSERT_EQUAL(UM98X_JOURNAL_RESET_SENT, s_sess.j.phase);
    sim_free(&s_sim);
}

TEST_CASE("session: the loader baud cannot be verified: restored, no reset, session ends", "[flash]")
{
    setup();
    s_sim.ignore_baud_change = true;
    TEST_ASSERT_EQUAL(FL_SESS_ABORTED, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_resets);
    TEST_ASSERT_EQUAL(UM98X_FLASH_FAIL_ENTRY, s_sess.out.failure);
    sim_free(&s_sim);
}

TEST_CASE("session: COM1 stranded at the unsaved baud: needs the user (power cycle)", "[flash]")
{
    setup();
    s_sim.mute_after_config = true;
    TEST_ASSERT_EQUAL(FL_SESS_NEEDS_USER, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_resets);
    TEST_ASSERT_NOT_NULL(strstr(s_sess.out.reason, "power cycle"));
    TEST_ASSERT_EQUAL(UM98X_JOURNAL_BAUD_CHANGING, s_sess.j.phase);
    sim_free(&s_sim);
}

TEST_CASE("session: menu table rows", "[flash]")
{
    /* Firmware submenu: "q", then "6", the session ends. */
    setup();
    s_sim.menu2 = SIM_M2_SUBMENU;
    TEST_ASSERT_EQUAL(FL_SESS_ABORTED, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT32(1, s_sim.rx_q);
    TEST_ASSERT_TRUE(s_sim.rx_six >= 1);
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_frames);
    TEST_ASSERT_EQUAL_UINT32(1, s_sim.rx_menu[2]);
    assert_never_dangerous();
    sim_free(&s_sim);

    /* "Invalid input" once: the item is sent again and the update goes on. */
    setup();
    s_sim.menu2 = SIM_M2_INVALID_ONCE;
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT32(2, s_sim.rx_menu[2]);
    sim_free(&s_sim);

    /* Twice: the attempt fails; three attempts, then the abort path. */
    setup();
    s_sim.menu2 = SIM_M2_INVALID_ALWAYS;
    TEST_ASSERT_EQUAL(FL_SESS_ABORTED, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT32(6, s_sim.rx_menu[2]);
    TEST_ASSERT_EQUAL_UINT8(3, s_sess.j.entry_attempts);
    assert_never_dangerous();
    sim_free(&s_sim);

    const sim_menu2_t fails[] = { SIM_M2_INPUT_TIMEOUT, SIM_M2_ABORTED, SIM_M2_PROMPT_ONLY };
    for (size_t i = 0; i < sizeof(fails) / sizeof(fails[0]); i++) {
        setup();
        s_sim.menu2 = fails[i];
        TEST_ASSERT_EQUAL(FL_SESS_ABORTED, fl_sess_run(&s_sess));
        TEST_ASSERT_EQUAL_UINT32(3, s_sim.rx_menu[2]);   /* once per attempt */
        TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_frames);
        TEST_ASSERT_EQUAL(UM98X_FLASH_FAIL_ENTRY, s_sess.out.failure);
        sim_free(&s_sim);
    }

    /* Silence after the item: sent at most three times, then CAN CAN. */
    setup();
    s_sim.menu2 = SIM_M2_SILENT;
    s_sim.skip_leading_crlf = true;
    TEST_ASSERT_EQUAL(FL_SESS_ABORTED, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT32(9, s_sim.rx_menu[2]);
    TEST_ASSERT_TRUE(s_sim.rx_cans >= 6);
    sim_free(&s_sim);

    /* The same loader printing a prompt for the item's leading "\r\n": text
     * arrived, so the item is not sent again; the lone prompt fails the attempt. */
    setup();
    s_sim.menu2 = SIM_M2_SILENT;
    TEST_ASSERT_EQUAL(FL_SESS_ABORTED, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT32(3, s_sim.rx_menu[2]);
    sim_free(&s_sim);

    /* No answer to the first item only, ready after the resend. */
    setup();
    s_sim.menu2 = SIM_M2_SILENT_FIRST;
    s_sim.skip_leading_crlf = true;
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT32(2, s_sim.rx_menu[2]);
    sim_free(&s_sim);

    /* A loader that skips the leading "\r\n" goes straight to the ready line. */
    setup();
    s_sim.skip_leading_crlf = true;
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_run(&s_sess));
    sim_free(&s_sim);

    /* "begin to update flash" is progress, not the go-signal. */
    setup();
    s_sim.begin_text = true;
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_run(&s_sess));
    sim_free(&s_sim);
}

TEST_CASE("session: a failed handshake is an entry attempt, before the point of no return", "[flash]")
{
    setup();
    s_sim.start_delay_ms = 20000;
    TEST_ASSERT_EQUAL(FL_SESS_ABORTED, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT8(3, s_sess.j.entry_attempts);
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_frames);
    TEST_ASSERT_EQUAL_UINT8(0, s_sess.j.transfers);
    TEST_ASSERT_FALSE(s_sess.out.ponr);
    assert_never_dangerous();
    sim_free(&s_sim);
}

TEST_CASE("session: CAN CAN mid-transfer: recorded, back at the prompt, the same package again", "[flash]")
{
    setup();
    s_sim.can_block = 5;
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT8(2, s_sess.j.transfers);
    TEST_ASSERT_TRUE(states_include(UM98X_JOURNAL_FAILED));
    static const uint8_t order[] = { UM98X_JOURNAL_TRANSFERRING, UM98X_JOURNAL_FAILED, UM98X_JOURNAL_MENU2_SENT,
                                     UM98X_JOURNAL_TRANSFERRING, UM98X_JOURNAL_EOT_SENT, UM98X_JOURNAL_VERIFYING };
    assert_order(order, sizeof(order));
    assert_never_dangerous();
    assert_image();
    sim_free(&s_sim);
}

TEST_CASE("session: a transfer that always stops: three transfers, then the user (never \"6\")", "[flash]")
{
    setup();
    s_sim.silent_block = 3;
    s_sim.silent_always = true;
    TEST_ASSERT_EQUAL(FL_SESS_NEEDS_USER, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT8(3, s_sess.j.transfers);
    TEST_ASSERT_EQUAL(UM98X_FLASH_FAIL_TRANSPORT, s_sess.j.failure_class);
    TEST_ASSERT_EQUAL(UM98X_JOURNAL_NEEDS_USER, s_sess.j.state);
    assert_never_dangerous();
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_six);
    sim_free(&s_sim);
}

TEST_CASE("session: text abort and the baud-mismatch pattern after the point of no return", "[flash]")
{
    setup();
    s_sim.text_block = 4;
    s_sim.text = "## Binary download aborted";
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT8(2, s_sess.j.transfers);
    assert_never_dangerous();
    sim_free(&s_sim);

    /* The receiver left the loader: its application answers (journalled
     * model), so the loader is entered again and the same package sent. */
    setup();
    s_sim.mismatch_block = 3;
    s_sim.app_period_ms = 100;
    fl_sess_result_t r = fl_sess_run(&s_sess);
    TEST_ASSERT_EQUAL_MESSAGE(FL_SESS_DONE, r, s_sess.out.reason);
    TEST_ASSERT_EQUAL_UINT8(2, s_sess.j.transfers);
    TEST_ASSERT_EQUAL_UINT8(2, s_sess.j.entry_attempts);
    TEST_ASSERT_EQUAL_UINT32(2, s_sim.rx_resets);
    assert_never_dangerous();
    assert_image();
    sim_free(&s_sim);

    /* ... but with another model answering, nothing more is sent. */
    setup();
    s_sim.mismatch_block = 3;
    s_sim.app_period_ms = 100;
    strcpy(s_sess.j.pre_model, "UM980");
    TEST_ASSERT_EQUAL(FL_SESS_NEEDS_USER, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT32(1, s_sim.rx_resets);
    sim_free(&s_sim);
}

TEST_CASE("session: post-transfer outcome table", "[flash]")
{
    /* E, not retried: package content. */
    setup();
    s_sim.post = SIM_POST_ERROR;
    s_sim.post_error = "wrong pkg header";
    TEST_ASSERT_EQUAL(FL_SESS_NEEDS_USER, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL(FL_OUT_E, s_sess.out.outcome);
    TEST_ASSERT_EQUAL(UM98X_FLASH_FAIL_PACKAGE, s_sess.j.failure_class);
    TEST_ASSERT_EQUAL_UINT8(1, s_sess.j.transfers);
    assert_never_dangerous();
    sim_free(&s_sim);

    /* E, a transport report: the same package again, then success. */
    setup();
    s_sim.post = SIM_POST_ERROR_THEN_OK;
    s_sim.post_error = "xmodem error";
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT8(2, s_sess.j.transfers);
    assert_never_dangerous();
    sim_free(&s_sim);

    /* E from a write error: not retried automatically. */
    setup();
    s_sim.post = SIM_POST_ERROR;
    s_sim.post_error = "backup fail";
    TEST_ASSERT_EQUAL(FL_SESS_NEEDS_USER, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL(UM98X_FLASH_FAIL_RECEIVER, s_sess.j.failure_class);
    sim_free(&s_sim);

    /* U: the prompt without the success text. */
    setup();
    s_sim.post = SIM_POST_PROMPT_ONLY;
    TEST_ASSERT_EQUAL(FL_SESS_NEEDS_USER, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL(FL_OUT_U, s_sess.out.outcome);
    TEST_ASSERT_EQUAL(UM98X_FLASH_FAIL_UNCONFIRMED, s_sess.j.failure_class);
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_six);   /* no automatic "6" */
    sim_free(&s_sim);

    /* L: success after T1, before T2. */
    setup();
    s_sim.post = SIM_POST_LATE;
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL(FL_OUT_L, s_sess.out.outcome);
    sim_free(&s_sim);

    /* A/B recovery warning: logged, not an error. */
    setup();
    s_sim.post_recover_warn = true;
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL(FL_OUT_S, s_sess.out.outcome);
    sim_free(&s_sim);

    /* Unknown text only by T2: U. */
    setup();
    s_sim.post = SIM_POST_TEXT_ONLY;
    TEST_ASSERT_EQUAL(FL_SESS_NEEDS_USER, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL(FL_OUT_U, s_sess.out.outcome);
    sim_free(&s_sim);

    /* N: nothing by T2 and nothing after the nudge. */
    setup();
    s_sim.post = SIM_POST_SILENT;
    uint32_t t0 = sim_now_ms(&s_sim);
    TEST_ASSERT_EQUAL(FL_SESS_NEEDS_USER, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL(FL_OUT_N_SILENT, s_sess.out.outcome);
    TEST_ASSERT_TRUE(sim_now_ms(&s_sim) - t0 >= 600000u);   /* never before T2 */
    sim_free(&s_sim);

    /* N: the application answers on its own: version check, done. */
    setup();
    s_sim.post = SIM_POST_SILENT;
    s_sim.post_app_after_ms = 100000;
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL(FL_OUT_N_APP, s_sess.out.outcome);
    TEST_ASSERT_EQUAL_UINT32(19152, s_sess.out.ver.build);
    sim_free(&s_sim);
}

TEST_CASE("session: leaving the loader: missing reset text, a second \"6\", an application that does not start", "[flash]")
{
    setup();
    s_sim.no_resetting_text = true;
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_run(&s_sess));
    sim_free(&s_sim);

    setup();
    s_sim.exit_needs_second_6 = true;
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_run(&s_sess));
    TEST_ASSERT_TRUE(s_sim.rx_six >= 2);
    sim_free(&s_sim);

    setup();
    s_sim.app_dead_after = true;
    TEST_ASSERT_EQUAL(FL_SESS_NEEDS_USER, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL(UM98X_FLASH_FAIL_APP_NOT_STARTING, s_sess.j.failure_class);
    sim_free(&s_sim);
}

TEST_CASE("session: after the update only another baud's pattern answers: silent, no version query at the loader baud", "[flash]")
{
    /* The new application talks at a baud none of the scan's candidates
     * decodes: at the loader baud that is the mismatch pattern, which is not
     * an application, so nothing is sent there. */
    setup();
    s_sim.app_baud_after = 230400;
    fl_sess_result_t r = fl_sess_run(&s_sess);
    TEST_ASSERT_EQUAL_MESSAGE(FL_SESS_NEEDS_USER, r, s_sess.out.reason);
    TEST_ASSERT_EQUAL(UM98X_FLASH_FAIL_NO_ANSWER, s_sess.out.failure);
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.versiona_at_loader_baud);
    sim_free(&s_sim);
}

TEST_CASE("session: journal write failures before and at the point of no return", "[flash]")
{
    /* The first write: nothing is sent at all. */
    setup();
    s_fail_state = UM98X_JOURNAL_PREPARED;
    TEST_ASSERT_EQUAL(FL_SESS_NOT_STARTED, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_config_com1);
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_resets);
    sim_free(&s_sim);

    /* "reset-sent": no reset; COM1 restored; the abort path. */
    setup();
    s_fail_state = UM98X_JOURNAL_RESET_SENT;
    s_fail_always = true;
    TEST_ASSERT_EQUAL(FL_SESS_ABORTED, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_resets);
    TEST_ASSERT_EQUAL_UINT32(115200, s_sim.com1_baud);
    sim_free(&s_sim);

    /* "transferring" must be written before the first frame. */
    setup();
    s_fail_state = UM98X_JOURNAL_TRANSFERRING;
    s_fail_always = true;
    TEST_ASSERT_EQUAL(FL_SESS_ABORTED, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_frames);
    TEST_ASSERT_EQUAL_UINT8(0, s_sess.j.transfers);
    TEST_ASSERT_FALSE(s_sess.out.ponr);
    sim_free(&s_sim);

    /* Later writes failing do not stop the flash. */
    setup();
    s_fail_state = UM98X_JOURNAL_EOT_SENT;
    s_fail_always = true;
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_run(&s_sess));
    sim_free(&s_sim);
}

TEST_CASE("session: the RAM copy changed or the user cancelled before the reset", "[flash]")
{
    setup();
    s_copy_ok = false;
    TEST_ASSERT_EQUAL(FL_SESS_ABORTED, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_frames);
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_menu[2]);
    TEST_ASSERT_TRUE(s_sim.rx_six >= 1);
    sim_free(&s_sim);

    setup();
    s_cancel = true;
    TEST_ASSERT_EQUAL(FL_SESS_ABORTED, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_resets);
    sim_free(&s_sim);
}

TEST_CASE("session: 115200 fallback attempt only when built in, after an attempt without the prompt", "[flash]")
{
    setup();
    s_sim.loader_only_baud = 115200;
    fl_sess_hooks_t h = hooks();
    journal_fields(&s_sess.j);
    fl_sess_init(&s_sess, &s_io, s_pkg, sizeof(s_pkg), 460800, true, &h);
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT32(115200, s_sess.j.boot_baud);
    TEST_ASSERT_EQUAL_UINT8(2, s_sess.j.entry_attempts);
    TEST_ASSERT_EQUAL_UINT32(8, s_sim.rx_last_burst);   /* 8-byte bursts at 115200 */
    TEST_ASSERT_EQUAL_UINT32(1, s_sim.rx_config_com1);  /* the fallback attempt skips the baud change */
    sim_free(&s_sim);

    setup();
    s_sim.loader_only_baud = 115200;
    TEST_ASSERT_EQUAL(FL_SESS_ABORTED, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT8(3, s_sess.j.entry_attempts);
    sim_free(&s_sim);
}

/* ---- the boot-time table ------------------------------------------------------------- */

static um98x_flash_journal_t journal_in(uint8_t state, uint8_t phase, uint8_t transfers)
{
    um98x_flash_journal_t j;
    journal_fields(&j);
    j.state = state;
    j.phase = phase;
    j.transfers = transfers;
    j.boot_baud = 460800;
    return j;
}

TEST_CASE("recovery table: every journal state by reset kind", "[flash]")
{
    struct {
        uint8_t state;
        uint8_t phase;
        uint8_t transfers;
        fl_plan_t esp_only;
        fl_plan_t power_on;
    } rows[] = {
        { UM98X_JOURNAL_PREPARED, UM98X_JOURNAL_PREPARED, 0, FL_PLAN_RESTORE_CHECK, FL_PLAN_APP_CHECK },
        { UM98X_JOURNAL_BAUD_CHANGING, UM98X_JOURNAL_BAUD_CHANGING, 0, FL_PLAN_RESTORE_CHECK, FL_PLAN_APP_CHECK },
        { UM98X_JOURNAL_RESET_SENT, UM98X_JOURNAL_RESET_SENT, 0, FL_PLAN_LOADER_PEEK, FL_PLAN_APP_CHECK },
        { UM98X_JOURNAL_MENU2_SENT, UM98X_JOURNAL_MENU2_SENT, 0, FL_PLAN_LOADER_PEEK, FL_PLAN_RACE_LEAVE },
        { UM98X_JOURNAL_TRANSFERRING, UM98X_JOURNAL_TRANSFERRING, 1, FL_PLAN_LISTEN_TRANSFER, FL_PLAN_RACE_RETRY },
        { UM98X_JOURNAL_FAILED, UM98X_JOURNAL_TRANSFERRING, 1, FL_PLAN_LISTEN_TRANSFER, FL_PLAN_RACE_RETRY },
        { UM98X_JOURNAL_EOT_SENT, UM98X_JOURNAL_EOT_SENT, 1, FL_PLAN_LISTEN_EOT, FL_PLAN_RACE_RETRY },
        { UM98X_JOURNAL_VERIFYING, UM98X_JOURNAL_VERIFYING, 1, FL_PLAN_VERIFY_CHECK, FL_PLAN_RACE_VERIFY },
        { UM98X_JOURNAL_NEEDS_USER, UM98X_JOURNAL_TRANSFERRING, 1, FL_PLAN_WAIT_USER, FL_PLAN_RACE_STOP },
        { UM98X_JOURNAL_NEEDS_USER, UM98X_JOURNAL_MENU2_SENT, 0, FL_PLAN_WAIT_USER, FL_PLAN_RACE_STOP },
        { UM98X_JOURNAL_NEEDS_USER, UM98X_JOURNAL_BAUD_CHANGING, 0, FL_PLAN_WAIT_USER, FL_PLAN_CHECK_WAIT },
        { UM98X_JOURNAL_NEEDS_USER, UM98X_JOURNAL_RESET_SENT, 0, FL_PLAN_WAIT_USER, FL_PLAN_CHECK_WAIT },
        /* A post-PoNR re-entry that stopped before the menu: the flash was modified, the race is the way in. */
        { UM98X_JOURNAL_NEEDS_USER, UM98X_JOURNAL_RESET_SENT, 1, FL_PLAN_WAIT_USER, FL_PLAN_RACE_STOP },
        { UM98X_JOURNAL_NEEDS_USER, UM98X_JOURNAL_PREPARED, 2, FL_PLAN_WAIT_USER, FL_PLAN_RACE_STOP },
        /* A retry after the point of no return records entry and menu states again: still past it. */
        { UM98X_JOURNAL_MENU2_SENT, UM98X_JOURNAL_MENU2_SENT, 1, FL_PLAN_LISTEN_TRANSFER, FL_PLAN_RACE_RETRY },
        { UM98X_JOURNAL_RESET_SENT, UM98X_JOURNAL_RESET_SENT, 2, FL_PLAN_LISTEN_TRANSFER, FL_PLAN_RACE_RETRY },
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        um98x_flash_journal_t j = journal_in(rows[i].state, rows[i].phase, rows[i].transfers);
        TEST_ASSERT_EQUAL_MESSAGE(rows[i].esp_only, flash_journal_plan(FJ_OPEN, &j, false),
                                  flash_journal_state_name(rows[i].state));
        TEST_ASSERT_EQUAL_MESSAGE(rows[i].power_on, flash_journal_plan(FJ_OPEN, &j, true),
                                  flash_journal_state_name(rows[i].state));
        /* The early race runs exactly for the plans that race, never after an ESP32-only reset. */
        TEST_ASSERT_EQUAL(flash_journal_plan_races(rows[i].power_on), flash_journal_needs_race(&j, true));
        TEST_ASSERT_FALSE(flash_journal_needs_race(&j, false));
        TEST_ASSERT_NOT_EQUAL(0, strlen(flash_journal_plan_text(rows[i].power_on)));
    }
    TEST_ASSERT_EQUAL(FL_PLAN_NONE, flash_journal_plan(FJ_NONE, NULL, true));
    TEST_ASSERT_EQUAL(FL_PLAN_WAIT_USER, flash_journal_plan(FJ_UNREADABLE, NULL, true));
    TEST_ASSERT_EQUAL(FL_PLAN_WAIT_USER, flash_journal_plan(FJ_NEWER, NULL, false));
    TEST_ASSERT_EQUAL(FL_PLAN_UNKNOWN, flash_journal_plan(FJ_UNKNOWN, NULL, true));
    um98x_flash_journal_t bad = journal_in(UM98X_JOURNAL_TRANSFERRING, UM98X_JOURNAL_MENU2_SENT, 1);
    TEST_ASSERT_EQUAL(FL_PLAN_WAIT_USER, flash_journal_plan(FJ_OPEN, &bad, true));
}

static void setup_recovery(uint8_t state, uint8_t phase, uint8_t transfers, bool with_pkg)
{
    setup();
    s_sess.j = journal_in(state, phase, transfers);
    fl_sess_hooks_t h = hooks();
    fl_sess_init(&s_sess, &s_io, with_pkg ? s_pkg : NULL, with_pkg ? sizeof(s_pkg) : 0, 460800, false, &h);
}

TEST_CASE("recovery: the loader prompt before the point of no return: \"6\", unchanged", "[flash]")
{
    setup_recovery(UM98X_JOURNAL_MENU2_SENT, UM98X_JOURNAL_MENU2_SENT, 0, false);
    sim_at_prompt(&s_sim, 460800);
    s_sim.host_baud = 115200;
    TEST_ASSERT_EQUAL(FL_SESS_ABORTED, fl_sess_recover(&s_sess, FL_PLAN_LOADER_PEEK, false));
    TEST_ASSERT_TRUE(s_sim.rx_six >= 1);
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_frames);
    sim_free(&s_sim);
}

TEST_CASE("recovery: a loader still waiting for a transfer: CAN CAN, prompt, same package again", "[flash]")
{
    setup_recovery(UM98X_JOURNAL_TRANSFERRING, UM98X_JOURNAL_TRANSFERRING, 1, false);
    sim_at_xrx(&s_sim, 460800);
    s_sim.host_baud = 115200;
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_recover(&s_sess, FL_PLAN_LISTEN_TRANSFER, false));
    TEST_ASSERT_EQUAL_UINT32(1, s_prepares);   /* the recovery gate ran before the transfer */
    TEST_ASSERT_EQUAL_UINT8(2, s_sess.j.transfers);
    TEST_ASSERT_TRUE(s_sim.rx_cans >= 2);
    assert_image();
    assert_never_dangerous();
    sim_free(&s_sim);
}

TEST_CASE("recovery: the effective state of an entry state after a transfer is failed", "[flash]")
{
    um98x_flash_journal_t j = journal_in(UM98X_JOURNAL_RESET_SENT, UM98X_JOURNAL_RESET_SENT, 1);
    TEST_ASSERT_EQUAL(UM98X_JOURNAL_FAILED, flash_journal_effective_state(&j));
    j = journal_in(UM98X_JOURNAL_RESET_SENT, UM98X_JOURNAL_RESET_SENT, 0);
    TEST_ASSERT_EQUAL(UM98X_JOURNAL_RESET_SENT, flash_journal_effective_state(&j));
    j = journal_in(UM98X_JOURNAL_TRANSFERRING, UM98X_JOURNAL_TRANSFERRING, 1);
    TEST_ASSERT_EQUAL(UM98X_JOURNAL_TRANSFERRING, flash_journal_effective_state(&j));
    j = journal_in(UM98X_JOURNAL_NEEDS_USER, UM98X_JOURNAL_RESET_SENT, 1);
    TEST_ASSERT_EQUAL(UM98X_JOURNAL_NEEDS_USER, flash_journal_effective_state(&j));
}

TEST_CASE("recovery: an ESP32 reset during a re-entry after a transfer listens 10 s, not T2", "[flash]")
{
    /* The receiver sits silent at its prompt (the re-entry reached it); the
     * journal says reset-sent with a transfer behind it. */
    setup_recovery(UM98X_JOURNAL_RESET_SENT, UM98X_JOURNAL_RESET_SENT, 1, false);
    s_sess.j.failure_class = UM98X_FLASH_FAIL_TRANSPORT;
    TEST_ASSERT_EQUAL(FL_PLAN_LISTEN_TRANSFER, flash_journal_plan(FJ_OPEN, &s_sess.j, false));
    sim_at_prompt(&s_sim, 460800);
    s_sim.host_baud = 115200;
    uint32_t t0 = sim_now_ms(&s_sim);
    fl_sess_result_t r = fl_sess_recover(&s_sess, FL_PLAN_LISTEN_TRANSFER, false);
    TEST_ASSERT_EQUAL_MESSAGE(FL_SESS_DONE, r, s_sess.out.reason);
    /* 10 s of listening, the nudge, then the retry (transfer, commit, exit): far below T2. */
    TEST_ASSERT_TRUE(sim_now_ms(&s_sim) - t0 < 120000u);
    TEST_ASSERT_EQUAL_UINT(1, s_prepares);
    TEST_ASSERT_EQUAL_UINT8(2, s_sess.j.transfers);
    assert_image();
    assert_never_dangerous();
    sim_free(&s_sim);
}

TEST_CASE("recovery: the cap holds across reboots", "[flash]")
{
    setup_recovery(UM98X_JOURNAL_FAILED, UM98X_JOURNAL_TRANSFERRING, 3, false);
    s_sess.j.failure_class = UM98X_FLASH_FAIL_TRANSPORT;
    sim_at_prompt(&s_sim, 460800);
    TEST_ASSERT_EQUAL(FL_SESS_NEEDS_USER, fl_sess_recover(&s_sess, FL_PLAN_RACE_RETRY, true));
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_menu[2]);
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_six);
    sim_free(&s_sim);
}

TEST_CASE("recovery: no prompt after a power cut: the application answers: exit checks", "[flash]")
{
    setup_recovery(UM98X_JOURNAL_EOT_SENT, UM98X_JOURNAL_EOT_SENT, 1, false);
    s_sim.st = SIM_APP;
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_recover(&s_sess, FL_PLAN_RACE_RETRY, false));
    TEST_ASSERT_EQUAL_UINT32(18975, s_sess.out.ver.build);   /* the old firmware still runs */
    sim_free(&s_sim);
}

TEST_CASE("recovery: an ESP32 reset during the commit: the outcome table in this boot", "[flash]")
{
    setup_recovery(UM98X_JOURNAL_EOT_SENT, UM98X_JOURNAL_EOT_SENT, 1, false);
    sim_at_prompt(&s_sim, 460800);
    s_sim.st = SIM_COMMIT;
    s_sim.flashed = true;
    /* The success text went out before the reset; only the prompt is seen now: U. */
    TEST_ASSERT_EQUAL(FL_SESS_NEEDS_USER, fl_sess_recover(&s_sess, FL_PLAN_LISTEN_EOT, false));
    TEST_ASSERT_EQUAL(FL_OUT_N_SILENT, s_sess.out.outcome);
    sim_free(&s_sim);
}

TEST_CASE("recovery: verifying: the application answers, or the loader prompt means it does not start", "[flash]")
{
    setup_recovery(UM98X_JOURNAL_VERIFYING, UM98X_JOURNAL_VERIFYING, 1, false);
    s_sim.st = SIM_APP;
    s_sim.flashed = true;
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_recover(&s_sess, FL_PLAN_VERIFY_CHECK, false));
    TEST_ASSERT_EQUAL_UINT32(19152, s_sess.out.ver.build);
    sim_free(&s_sim);

    setup_recovery(UM98X_JOURNAL_VERIFYING, UM98X_JOURNAL_VERIFYING, 1, false);
    sim_at_prompt(&s_sim, 460800);
    s_sim.host_baud = 115200;
    s_sim.split_prompt = false;
    /* The loader reprints its prompt only when asked; make it talk: */
    TEST_ASSERT_EQUAL(FL_SESS_NEEDS_USER, fl_sess_recover(&s_sess, FL_PLAN_VERIFY_CHECK, false));
    sim_free(&s_sim);

    /* After a power cut with the prompt reached by the race: "6" and the exit checks. */
    setup_recovery(UM98X_JOURNAL_VERIFYING, UM98X_JOURNAL_VERIFYING, 1, false);
    sim_at_prompt(&s_sim, 460800);
    s_sim.flashed = true;
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_recover(&s_sess, FL_PLAN_RACE_VERIFY, true));
    sim_free(&s_sim);
}

TEST_CASE("recovery: needs-user plans send nothing but the race and the listen", "[flash]")
{
    setup_recovery(UM98X_JOURNAL_NEEDS_USER, UM98X_JOURNAL_TRANSFERRING, 1, false);
    TEST_ASSERT_EQUAL(FL_SESS_NEEDS_USER, fl_sess_recover(&s_sess, FL_PLAN_RACE_STOP, true));
    TEST_ASSERT_EQUAL_UINT(0, s_nstates);
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_six);
    sim_free(&s_sim);

    setup_recovery(UM98X_JOURNAL_NEEDS_USER, UM98X_JOURNAL_BAUD_CHANGING, 0, false);
    s_sim.st = SIM_APP;
    TEST_ASSERT_EQUAL(FL_SESS_NEEDS_USER, fl_sess_recover(&s_sess, FL_PLAN_CHECK_WAIT, true));
    TEST_ASSERT_EQUAL_UINT(0, s_nstates);
    sim_free(&s_sim);
}

TEST_CASE("recovery: an interrupted baud change: restore ladder, application check", "[flash]")
{
    setup_recovery(UM98X_JOURNAL_BAUD_CHANGING, UM98X_JOURNAL_BAUD_CHANGING, 0, false);
    s_sim.st = SIM_APP;
    s_sim.com1_baud = 460800;   /* the change took, unsaved */
    TEST_ASSERT_EQUAL(FL_SESS_ABORTED, fl_sess_recover(&s_sess, FL_PLAN_RESTORE_CHECK, false));
    TEST_ASSERT_EQUAL_UINT32(115200, s_sim.com1_baud);
    sim_free(&s_sim);
}

TEST_CASE("user choices: retry at the prompt after the point of no return, and restart", "[flash]")
{
    setup_recovery(UM98X_JOURNAL_NEEDS_USER, UM98X_JOURNAL_TRANSFERRING, 3, false);
    s_sess.j.failure_class = UM98X_FLASH_FAIL_TRANSPORT;
    sim_at_prompt(&s_sim, 460800);
    s_sim.host_baud = 115200;
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_user_retry(&s_sess));
    TEST_ASSERT_EQUAL_UINT8(4, s_sess.j.transfers);   /* one more, confirmed by the user */
    assert_image();
    sim_free(&s_sim);

    setup_recovery(UM98X_JOURNAL_NEEDS_USER, UM98X_JOURNAL_EOT_SENT, 1, false);
    sim_at_prompt(&s_sim, 460800);
    s_sim.flashed = true;
    TEST_ASSERT_EQUAL(FL_SESS_DONE, fl_sess_user_restart(&s_sess));
    TEST_ASSERT_EQUAL_UINT32(19152, s_sess.out.ver.build);
    sim_free(&s_sim);
}

TEST_CASE("user choices: retry after the point of no return with the application answering: gate, then entry", "[flash]")
{
    /* Needs-user after a failed transfer, and the receiver went back to its
     * application. The retry starts without a RAM copy: the recovery gate
     * must run before the menu, or the copy check stops the session at boot>. */
    setup_recovery(UM98X_JOURNAL_NEEDS_USER, UM98X_JOURNAL_TRANSFERRING, 1, false);
    s_sess.j.failure_class = UM98X_FLASH_FAIL_TRANSPORT;
    s_sim.st = SIM_APP;
    fl_sess_result_t r = fl_sess_user_retry(&s_sess);
    TEST_ASSERT_EQUAL_MESSAGE(FL_SESS_DONE, r, s_sess.out.reason);
    TEST_ASSERT_EQUAL_UINT(1, s_prepares);
    TEST_ASSERT_EQUAL_UINT8(2, s_sess.j.transfers);
    TEST_ASSERT_EQUAL_UINT32(1, s_sim.rx_resets);
    TEST_ASSERT_EQUAL_UINT32(19152, s_sess.out.ver.build);
    assert_never_dangerous();
    assert_image();
    sim_free(&s_sim);
}

TEST_CASE("user choices: retry before the point of no return, at the prompt and from the application", "[flash]")
{
    /* At the loader prompt (a session that stopped between the menu and the first frame). */
    setup_recovery(UM98X_JOURNAL_NEEDS_USER, UM98X_JOURNAL_MENU2_SENT, 0, false);
    s_sess.j.failure_class = UM98X_FLASH_FAIL_NO_ANSWER;
    sim_at_prompt(&s_sim, 460800);
    s_sim.host_baud = 115200;
    fl_sess_result_t r = fl_sess_user_retry(&s_sess);
    TEST_ASSERT_EQUAL_MESSAGE(FL_SESS_DONE, r, s_sess.out.reason);
    TEST_ASSERT_EQUAL_UINT(1, s_prepares);
    TEST_ASSERT_EQUAL_UINT8(1, s_sess.j.transfers);
    TEST_ASSERT_EQUAL_UINT32(1, s_sim.rx_menu[2]);
    assert_never_dangerous();
    assert_image();
    sim_free(&s_sim);

    /* The application answers: a full loader entry. */
    setup_recovery(UM98X_JOURNAL_NEEDS_USER, UM98X_JOURNAL_RESET_SENT, 0, false);
    s_sess.j.failure_class = UM98X_FLASH_FAIL_ENTRY;
    s_sim.st = SIM_APP;
    r = fl_sess_user_retry(&s_sess);
    TEST_ASSERT_EQUAL_MESSAGE(FL_SESS_DONE, r, s_sess.out.reason);
    TEST_ASSERT_EQUAL_UINT(1, s_prepares);
    TEST_ASSERT_EQUAL_UINT8(1, s_sess.j.transfers);
    TEST_ASSERT_EQUAL_UINT32(1, s_sim.rx_resets);
    assert_never_dangerous();
    assert_image();
    sim_free(&s_sim);
}

TEST_CASE("user choices: a retry whose package fails the recovery gate sends nothing", "[flash]")
{
    setup_recovery(UM98X_JOURNAL_NEEDS_USER, UM98X_JOURNAL_TRANSFERRING, 1, false);
    s_sess.j.failure_class = UM98X_FLASH_FAIL_TRANSPORT;
    s_prepare_ok = false;
    s_sim.st = SIM_APP;
    TEST_ASSERT_EQUAL(FL_SESS_NEEDS_USER, fl_sess_user_retry(&s_sess));
    TEST_ASSERT_EQUAL_UINT(1, s_prepares);
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.host_bytes);
    TEST_ASSERT_EQUAL(UM98X_FLASH_FAIL_TRANSPORT, s_sess.j.failure_class);   /* not turned into a package fault */
    TEST_ASSERT_EQUAL(UM98X_JOURNAL_NEEDS_USER, s_sess.j.state);
    sim_free(&s_sim);
}

TEST_CASE("fresh prompt: a prompt that answers \"\\r\\n\", none, or the receiver gone", "[flash]")
{
    static fl_lis_t lis;
    setup();
    sim_at_prompt(&s_sim, 460800);
    fl_lis_init(&lis, &s_io, 460800);
    bool io_failed = true;
    TEST_ASSERT_EQUAL(FL_PROMPT_SEEN, fl_fresh_prompt(&lis, &io_failed));
    TEST_ASSERT_FALSE(io_failed);
    sim_free(&s_sim);

    /* The loader does not answer an empty line (its menu timed out, or the
     * stop key left it in another input state): no prompt, and nothing but
     * the one "\r\n" was sent. */
    setup();
    sim_at_prompt(&s_sim, 460800);
    s_sim.no_prompt_on_empty = true;
    fl_lis_init(&lis, &s_io, 460800);
    TEST_ASSERT_EQUAL(FL_PROMPT_TIMEOUT, fl_fresh_prompt(&lis, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, s_sim.host_bytes);
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_menu[2]);
    sim_free(&s_sim);

    /* The application talks at the loader baud: it left the loader. */
    setup();
    s_sim.com1_baud = 460800;
    s_sim.host_baud = 460800;
    fl_lis_init(&lis, &s_io, 460800);
    TEST_ASSERT_EQUAL(FL_PROMPT_LEFT, fl_fresh_prompt(&lis, NULL));
    sim_free(&s_sim);
}

TEST_CASE("session: no fresh prompt before menu item 2: the item is never sent, \"6\" ends the attempt", "[flash]")
{
    setup();
    s_sim.no_prompt_on_empty = true;
    TEST_ASSERT_EQUAL(FL_SESS_ABORTED, fl_sess_run(&s_sess));
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_menu[2]);
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_frames);
    TEST_ASSERT_TRUE(s_sim.rx_six >= 1);
    assert_never_dangerous();
    sim_free(&s_sim);
}

TEST_CASE("bench observation: start bytes recorded for a while, no frame sent", "[flash]")
{
    setup();
    sim_at_xrx(&s_sim, 460800);
    static fl_lis_t lis;
    static fl_rx_t rx;
    fl_lis_init(&lis, &s_io, 460800);
    fl_rx_init(&rx, &s_io);
    fl_startbytes_t sb;
    fl_watch_start_bytes(&lis, &rx, 20000, &sb);
    TEST_ASSERT_TRUE(sb.c_count >= 19);
    TEST_ASSERT_EQUAL_UINT32(0, sb.nak_count);
    TEST_ASSERT_TRUE(sb.first_ms < 200);
    TEST_ASSERT_UINT32_WITHIN(5, 1000, sb.interval_ms);
    TEST_ASSERT_EQUAL_UINT32(0, s_sim.rx_frames);
    sim_free(&s_sim);
}
