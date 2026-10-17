/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Bench console (see bench.h). The commands follow the receiver flasher's
 * bench plan one step at a time; each prints one PASS/FAIL line.
 *
 * Receiver link hooks (with CONFIG_GNSS_LINK_BENCH_HOOKS): an RX task stall
 * (`stall`; past the task watchdog only with TWDT-PANIC), a stall inside the
 * next RAW-lease read (`rawstall`), the frame and drain-pass time recorder
 * (`ftimes <s>` starts it in the background and returns, so `stall`,
 * `rawstall` and `listen` can run inside a recording; `ftimes status`,
 * `ftimes stop`, `ftimes summary`, `ftimes dump`), the loader prompt
 * detector in report-only mode
 * (`promptdet`, and `promptwatch` at a chosen baud under the station's lease:
 * nothing is transmitted), deleting the UART driver for the link's own
 * reopen (`uartclose`). Link restart helpers: a fragmented internal heap
 * (`heapfrag`) and the ladder's link restart on demand (`linkrestart`).
 * Same refusals as the receiver commands (maintenance, an open journal,
 * another receiver session or job).
 *
 * Wi-Fi: the station's own move to the strongest access point of its
 * network now (`wifimove`), watched until it is over: PASS when the address
 * and the connections were kept.
 */
#include "bench.h"

#include "sdkconfig.h"

#if CONFIG_BENCH_CONSOLE

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "diag.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_attr.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gnss_link.h"
#include "lifecycle.h"
#include "linenoise/linenoise.h"
#include "network.h"
#include "pkg_store.h"
#include "settings.h"
#include "station.h"
#include "um98x_flash.h"

static const char *TAG = "bench";

#define TOK_SEND    "SEND"
#define TOK_RESET   "RESET-RECEIVER"
#define TOK_FLASH   "FLASH-RECEIVER"
#define TOK_RACE    "RACE-AT-BOOT"
#define TOK_JOURNAL "WRITE-JOURNAL"
#define TOK_CLEAR   "CLEAR-JOURNAL"
#define TOK_RESTART "RESTART-ESP32"
#define TOK_TWDT    "TWDT-PANIC"
#define TOK_HEAP    "FRAGMENT-HEAP"
#define TOK_UART    "CLOSE-UART"
#define TOK_SAFE    "ENTER-SAFE-MODE"

#define RUN_TIMEOUT_MS (20u * 60u * 1000u)
#define RACE_MAX_MS    10000u

static char s_race_line[200];

/* ---- replies ------------------------------------------------------------------------ */

/* Every reply goes through diag's console writer a whole line at a time
 * (lines printed in parts are gathered here first), so a log line from
 * another task never lands inside one: the bench tools parse these lines.
 * One writer: the command line's task (and bench_boot_early before it
 * exists). */
EXT_RAM_BSS_ATTR static diag_console_lines_t s_out;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_console_vprintf(&s_out, fmt, ap);
    va_end(ap);
}

/* The end of a reply: its last line ended and sent. */
static void say_end(void)
{
    diag_console_flush(&s_out, true);
}

/* ---- helpers ------------------------------------------------------------------------ */

static bool confirmed(int argc, char **argv, const char *token)
{
    if (argc >= 2 && strcmp(argv[argc - 1], token) == 0) {
        return true;
    }
    say("BENCH REFUSED: this writes to the receiver or the journal. Repeat it with the word %s at the end.\n",
        token);
    return false;
}

static bool parse_u32(const char *s, uint32_t lo, uint32_t hi, uint32_t *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0' || v < lo || v > hi) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool parse_baud(const char *s, uint32_t *out)
{
    uint32_t b;
    if (!parse_u32(s, 9600, 921600, &b) || (b != 115200 && b != 230400 && b != 460800 && b != 921600)) {
        say("BENCH baud: 115200, 230400, 460800 or 921600\n");
        return false;
    }
    *out = b;
    return true;
}

static int run(uint8_t op, uint32_t baud, uint32_t seconds, bool record_only)
{
    station_bench_req_t req = { .op = op, .baud = baud, .seconds = seconds, .record_only = record_only };
    station_bench_res_t res;
    esp_err_t err = station_bench_run(&req, &res, RUN_TIMEOUT_MS);
    say("BENCH %s\n", res.line[0] != '\0' ? res.line : esp_err_to_name(err));
    return (err == ESP_OK && res.pass) ? 0 : 1;
}

/* The staged package, confirmed by the first 8 hex digits of its SHA-256 and the build it should produce. */
static bool flash_request(const char *sha8, const char *expected_fw, station_flash_request_t *req)
{
    pkg_store_info_t info;
    if (pkg_store_get_info(&info) != ESP_OK || !info.valid) {
        say("BENCH REFUSED: no package staged (upload it on the web UI's receiver update page first)\n");
        return false;
    }
    char hex[9];
    for (int i = 0; i < 4; i++) {
        snprintf(hex + 2 * i, 3, "%02x", (unsigned)info.sha256[i]);
    }
    char want[9];
    snprintf(want, sizeof(want), "%s", sha8);
    for (char *p = want; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }
    if (strlen(want) != 8 || strcmp(want, hex) != 0) {
        say("BENCH REFUSED: the staged package's SHA-256 starts with %s, not %s\n", hex, sha8);
        return false;
    }
    const char *b = strstr(expected_fw, "Build");
    uint32_t build = 0;
    if (b == NULL || strlen(expected_fw) >= sizeof(req->expected_fw) || !parse_u32(b + 5, 1, 0x7FFFFFFF, &build)) {
        say("BENCH REFUSED: the expected firmware must look like R4.10Build18975\n");
        return false;
    }
    memset(req, 0, sizeof(*req));
    memcpy(req->sha256, info.sha256, sizeof(req->sha256));
    req->size = info.size;
    snprintf(req->expected_fw, sizeof(req->expected_fw), "%s", expected_fw);
    req->expected_build = build;
    return true;
}

static const char *const k_states[] = { "", "prepared", "baud-changing", "reset-sent", "menu-2-sent", "transferring",
                                        "eot-sent", "verifying", "failed", "needs-user" };

static uint8_t parse_state(const char *s)
{
    for (uint8_t i = 1; i < sizeof(k_states) / sizeof(k_states[0]); i++) {
        if (strcmp(s, k_states[i]) == 0) {
            return i;
        }
    }
    say("BENCH state: prepared, baud-changing, reset-sent, menu-2-sent, transferring, eot-sent, verifying, "
        "failed, needs-user\n");
    return 0;
}

/* ---- commands ------------------------------------------------------------------------ */

static int cmd_listen(int argc, char **argv)
{
    uint32_t baud;
    uint32_t secs;
    if (argc != 3 || !parse_baud(argv[1], &baud) || !parse_u32(argv[2], 1, 600, &secs)) {
        say("usage: listen <baud> <seconds 1-600>\n");
        return 1;
    }
    return run(STATION_BENCH_LISTEN, baud, secs, false);
}

static int cmd_listen_pc(int argc, char **argv)
{
    uint32_t baud;
    uint32_t secs;
    if (argc != 3 || !parse_baud(argv[1], &baud) || !parse_u32(argv[2], 1, 3600, &secs)) {
        say("usage: listen_pc <baud> <seconds 1-3600>   (while a PC flashes the receiver over its other port)\n");
        return 1;
    }
    return run(STATION_BENCH_LISTEN, baud, secs, true);
}

static int cmd_version(int argc, char **argv)
{
    uint32_t baud;
    if (argc != 3 || !parse_baud(argv[1], &baud)) {
        say("usage: version <baud> " TOK_SEND "\n");
        return 1;
    }
    if (!confirmed(argc, argv, TOK_SEND)) {
        return 1;
    }
    return run(STATION_BENCH_VERSION, baud, 0, false);
}

static int cmd_entry(int argc, char **argv)
{
    uint32_t baud;
    if (argc != 3 || !parse_baud(argv[1], &baud) || (baud != 460800 && baud != 115200)) {
        say("usage: entry <460800|115200> " TOK_RESET "\n");
        return 1;
    }
#if !CONFIG_UM98X_FLASH_FALLBACK_115200
    if (baud == 115200) {
        say("BENCH REFUSED: the 115200 entry is built in only with UM98X_FLASH_FALLBACK_115200\n");
        return 1;
    }
#endif
    if (!confirmed(argc, argv, TOK_RESET)) {
        return 1;
    }
    return run(STATION_BENCH_ENTRY, baud, 0, false);
}

static int cmd_menu2(int argc, char **argv)
{
    uint32_t secs = 20;
    if (argc != 3 || !parse_u32(argv[1], 5, 120, &secs)) {
        say("usage: menu2 <seconds of start bytes 5-120, normally 20> " TOK_RESET "\n");
        return 1;
    }
    if (!confirmed(argc, argv, TOK_RESET)) {
        return 1;
    }
    return run(STATION_BENCH_MENU2, UM98X_FLASH_BAUD, secs, false);
}

static int cmd_idle(int argc, char **argv)
{
    uint32_t secs = 300;
    if (argc != 3 || !parse_u32(argv[1], 10, 1800, &secs)) {
        say("usage: idle <seconds 10-1800, normally 300> " TOK_RESET "\n");
        return 1;
    }
    if (!confirmed(argc, argv, TOK_RESET)) {
        return 1;
    }
    return run(STATION_BENCH_IDLE, UM98X_FLASH_BAUD, secs, false);
}

static int cmd_race_arm(int argc, char **argv)
{
    if (argc != 2 || !confirmed(argc, argv, TOK_RACE)) {
        if (argc != 2) {
            say("usage: race_arm " TOK_RACE "\n");
        }
        return 1;
    }
    /* Maintenance is the hands-off mode (the receiver may be flashed over
     * its other port): the stop key at the next boot is not armed during it.
     * A maintenance started after arming is caught at that boot. */
    static station_status_t st;
    if (station_get_status(&st) == ESP_OK && st.rx_maintenance) {
        say("BENCH REFUSED: receiver maintenance is on (end it first)\n");
        return 1;
    }
    esp_err_t err = settings_set_state_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_BENCH_RACE, 1);
    say("BENCH %s\n", err == ESP_OK ? "armed for the next boot only: power-cycle the board now (the result prints "
                                         "at boot and with race_result)"
                                       : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_race_result(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    say("BENCH %s\n", s_race_line[0] != '\0' ? s_race_line : "no stop-key-from-boot run this boot");
    return 0;
}

static int start_flash(const char *sha8, const char *fw, uint32_t stop_after, uint8_t reset_at)
{
    /* As the web's 409 signal_test_running: the comparison owns the
     * receiver until it ends (the station refuses the flash too). */
    if (station_sg_test_running()) {
        say("BENCH REFUSED: a signal-group comparison is running (stop it on the receiver page first)\n");
        return 1;
    }
    station_flash_request_t req;
    if (!flash_request(sha8, fw, &req)) {
        return 1;
    }
    esp_err_t err = station_bench_flash(&req, stop_after, reset_at);
    if (err != ESP_OK) {
        say("BENCH REFUSED: %s (the station must stream, no journal, no app update or trial, no maintenance)\n",
            esp_err_to_name(err));
        return 1;
    }
    say("BENCH started: every byte is logged; `status` shows the outcome when it ends\n");
    return 0;
}

static int cmd_flash(int argc, char **argv)
{
    if (argc != 4) {
        say("usage: flash <first 8 hex of the staged SHA-256> <expected firmware, e.g. R4.10Build18975> " TOK_FLASH
            "\n");
        return 1;
    }
    if (!confirmed(argc, argv, TOK_FLASH)) {
        return 1;
    }
    return start_flash(argv[1], argv[2], 0, 0);
}

static int cmd_abort(int argc, char **argv)
{
    uint32_t blocks;
    if (argc != 5 || !parse_u32(argv[1], 1, 100000, &blocks)) {
        say("usage: abort <blocks before stopping> <first 8 hex of the SHA-256> <expected firmware> " TOK_FLASH "\n");
        return 1;
    }
    if (!confirmed(argc, argv, TOK_FLASH)) {
        return 1;
    }
    return start_flash(argv[2], argv[3], blocks, 0);
}

static int cmd_reset_at(int argc, char **argv)
{
    if (argc != 5) {
        say("usage: reset_at <journal state> <first 8 hex of the SHA-256> <expected firmware> " TOK_FLASH "\n");
        return 1;
    }
    uint8_t st = parse_state(argv[1]);
    if (st == 0 || !confirmed(argc, argv, TOK_FLASH)) {
        return 1;
    }
    return start_flash(argv[2], argv[3], 0, st);
}

static int cmd_jstate(int argc, char **argv)
{
#if CONFIG_UM98X_FLASH_RECOVERY_ACTIONS
    /* The next boot would act on the journal written here (sending to the receiver's loader). */
    say("BENCH REFUSED: jstate writes a journal for the report-only boot table; this build acts on journals "
        "(UM98X_FLASH_RECOVERY_ACTIONS)\n");
    return 1;
#endif
    if (argc != 3) {
        say("usage: jstate <journal state> " TOK_JOURNAL "\n");
        return 1;
    }
    uint8_t st = parse_state(argv[1]);
    if (st == 0 || !confirmed(argc, argv, TOK_JOURNAL)) {
        return 1;
    }
    esp_err_t err = station_bench_journal_set(st);
    say("BENCH %s\n", err == ESP_OK ? "journal written (nothing sent to the receiver): restart the ESP32 "
                                         "(restart " TOK_RESTART ") or power-cycle, then read the boot log"
                                       : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_jclear(int argc, char **argv)
{
    if (argc != 2 || !confirmed(argc, argv, TOK_CLEAR)) {
        return 1;
    }
    esp_err_t err = station_flash_user_action(STATION_FLASH_ACTION_CLEAR_JOURNAL);
    say("BENCH %s\n", err == ESP_OK ? "journal cleared" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_restart(int argc, char **argv)
{
    if (argc != 2 || !confirmed(argc, argv, TOK_RESTART)) {
        return 1;
    }
    esp_err_t err = lifecycle_request_reboot(LIFECYCLE_REBOOT_USER);
    say("BENCH %s\n", err == ESP_OK ? "restart requested (it waits for a held reboot inhibit)" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_safemode(int argc, char **argv)
{
    if (argc != 2 || !confirmed(argc, argv, TOK_SAFE)) {
        return 1;
    }
    esp_err_t err = lifecycle_bench_reboot_into_safe_mode();
    say("BENCH %s\n", err == ESP_OK ? "restart into safe mode requested (a failed boot at the crash limit)"
                                    : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

/* Names next to the numbers in `status`: the station state, the update phase
 * and the last outcome as the web API names them, the journal states as
 * `jstate` and `reset_at` take them. */
static const char *const k_station_states[] = {
    [STATION_STATE_IDLE] = "idle",
    [STATION_STATE_DETECTING] = "detecting",
    [STATION_STATE_CONFIGURING] = "configuring",
    [STATION_STATE_STREAMING] = "streaming",
    [STATION_STATE_FLASHING] = "flashing",
    [STATION_STATE_FLASH_RECOVERY] = "flash_recovery",
    [STATION_STATE_RECOVERING] = "recovering",
    [STATION_STATE_NO_RECEIVER] = "no_receiver",
};

static const char *const k_flash_phases[] = {
    [UM98X_FLASH_IDLE] = "idle",
    [UM98X_FLASH_CHECKING] = "checking",
    [UM98X_FLASH_ENTERING] = "entering",
    [UM98X_FLASH_TRANSFERRING] = "transferring",
    [UM98X_FLASH_COMMITTING] = "committing",
    [UM98X_FLASH_FINISHING] = "finishing",
    [UM98X_FLASH_DONE] = "done",
    [UM98X_FLASH_FAILED] = "failed",
    [UM98X_FLASH_NEEDS_USER] = "needs_user",
};

static const char *const k_flash_outcomes[] = {
    [STATION_FLASH_OUT_NONE] = "none",
    [STATION_FLASH_OUT_SUCCESS] = "success",
    [STATION_FLASH_OUT_OLD_FIRMWARE] = "old_firmware",
    [STATION_FLASH_OUT_UNEXPECTED] = "unexpected_build",
    [STATION_FLASH_OUT_NOT_DONE] = "not_done",
    [STATION_FLASH_OUT_REJECTED] = "rejected",
    [STATION_FLASH_OUT_NOT_STARTED] = "not_started",
    [STATION_FLASH_OUT_NEEDS_USER] = "needs_user",
    [STATION_FLASH_OUT_CHECKED] = "checked",
};

#define NAME_OF(table, v) name_of((table), sizeof(table) / sizeof((table)[0]), (int)(v))

static const char *name_of(const char *const *table, size_t count, int v)
{
    return (v >= 0 && (size_t)v < count && table[v] != NULL) ? table[v] : "unknown";
}

/* A journal state by its `jstate` name; 0 (no state) is "none". */
static const char *journal_state_name(uint8_t v)
{
    return v == 0 ? "none" : um98x_flash_journal_state_name(v);
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static station_status_t st;
    if (station_get_status(&st) != ESP_OK) {
        say("BENCH no status\n");
        return 1;
    }
    say("BENCH state %d (%s), receiver ", (int)st.state, NAME_OF(k_station_states, st.state));
    if (st.receiver.model[0] == '\0') {
        say("none");
    } else {
        say("%s %s", st.receiver.model, st.receiver.firmware[0] != '\0' ? st.receiver.firmware : "(firmware unknown)");
    }
    say(", journal %s (state %u %s, phase %u %s, %u transfer(s)), flashing %s\n",
        st.flash_journal_settings_unavailable ? "unknown" : st.flash_journal_open ? "open" : "none",
        (unsigned)st.flash_journal.state, journal_state_name(st.flash_journal.state),
        (unsigned)st.flash_journal.phase, journal_state_name(st.flash_journal.phase),
        (unsigned)st.flash_journal.transfers, st.flash_available ? "built in" : "not built in");
    say("BENCH progress: phase %d (%s), %u/%u bytes, %u/%u frames, %u retries; last: outcome %u (%s), rule %u, "
        "reason %s\n",
        (int)st.flash.phase, NAME_OF(k_flash_phases, st.flash.phase), (unsigned)st.flash.bytes_sent,
        (unsigned)st.flash.bytes_total, (unsigned)st.flash.frames_acked, (unsigned)st.flash.frames_total,
        (unsigned)st.flash.retries, (unsigned)st.flash_last.outcome, NAME_OF(k_flash_outcomes, st.flash_last.outcome),
        (unsigned)st.flash_last.failed_rule, st.flash_last.reason[0] != '\0' ? st.flash_last.reason : "none");
    return 0;
}

static int cmd_capture(int argc, char **argv)
{
    uint32_t kb = 4;
    if (argc == 2 && !parse_u32(argv[1], 1, 256, &kb)) {
        say("usage: capture [last KB, 1-256]\n");
        return 1;
    }
    if (!um98x_flash_capture_available()) {
        say("BENCH no capture in this build\n");
        return 1;
    }
    /* Skip to the last kb KB. */
    uint32_t cursor = 0;
    static char buf[512];
    uint32_t total = 0;
    while (um98x_flash_capture_read(&cursor, buf, sizeof(buf)) > 0) {
    }
    total = cursor;
    cursor = total > kb * 1024u ? total - kb * 1024u : 0;
    size_t n;
    while ((n = um98x_flash_capture_read(&cursor, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        diag_console_put(&s_out, buf, strlen(buf));
    }
    say("\nBENCH capture end (download it all from the web UI: Diagnostics)\n");
    return 0;
}

/* ---- receiver link: stalls, timing, prompt observer, restart helpers ----------------------- */

/* The refusals of the receiver commands, for the link's bench hooks: receiver
 * maintenance, an open receiver update journal, another receiver session or job. */
static bool link_free(void)
{
    static station_status_t st;
    if (station_get_status(&st) != ESP_OK) {
        say("BENCH REFUSED: no station status\n");
        return false;
    }
    if (st.rx_maintenance) {
        say("BENCH REFUSED: receiver maintenance is on (end it first)\n");
        return false;
    }
    if (st.flash_journal_open || st.flash_journal_settings_unavailable) {
        say("BENCH REFUSED: a receiver update journal is open\n");
        return false;
    }
    if (st.lease_held || st.flash_session_running || st.state == STATION_STATE_CONFIGURING ||
        st.state == STATION_STATE_FLASHING || st.state == STATION_STATE_RECOVERING ||
        st.state == STATION_STATE_FLASH_RECOVERY) {
        say("BENCH REFUSED: another receiver session or job runs; try again when it ends\n");
        return false;
    }
#if CONFIG_GNSS_LINK_BENCH_HOOKS
    if (gnss_link_bench_stall_active()) {
        say("BENCH REFUSED: a stall runs\n");
        return false;
    }
#endif
    return true;
}

static void print_heap(const char *when)
{
    say("BENCH heap %s: internal free %u B, largest internal block %u B\n", when,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

#if CONFIG_GNSS_LINK_BENCH_HOOKS

static int cmd_stall(int argc, char **argv)
{
    uint32_t ms = 0;
    bool panic = argc == 3 && strcmp(argv[2], TOK_TWDT) == 0;
    if ((argc != 2 && !panic) || !parse_u32(argv[1], 1, GNSS_LINK_BENCH_STALL_PANIC_MS, &ms) ||
        (ms > GNSS_LINK_BENCH_STALL_SAFE_MS && ms < 10000u)) {
        say("usage: stall <ms 1-9000>   |   stall <ms 10000-30000> " TOK_TWDT "\n");
        return 1;
    }
    if (ms > GNSS_LINK_BENCH_STALL_SAFE_MS && !panic) {
        say("BENCH REFUSED: a stall of 10 s or more trips the task watchdog (a panic and a restart): repeat it "
            "with the word " TOK_TWDT " at the end\n");
        return 1;
    }
    if (!link_free()) {
        return 1;
    }
    gnss_link_stats_t a;
    gnss_link_stats_t b;
    (void)gnss_link_get_stats(&a);
    esp_err_t err = gnss_link_bench_stall(ms, panic);
    if (err != ESP_OK) {
        say("BENCH FAIL stall: %s\n", esp_err_to_name(err));
        return 1;
    }
    if (panic) {
        say("BENCH stall %u ms: the RX task sleeps without feeding the task watchdog; a task-watchdog panic "
            "and a restart are expected (read the boot log afterwards)\n",
            (unsigned)ms);
    }
    int64_t t0 = esp_timer_get_time();
    while (gnss_link_bench_stall_active() && esp_timer_get_time() - t0 < (int64_t)(ms + 5000u) * 1000) {
        vTaskDelay(pdMS_TO_TICKS(100));   /* this console keeps running meanwhile */
    }
    (void)gnss_link_get_stats(&b);
    say("BENCH stall %u ms done after %u ms: +%u buffer-full, +%u FIFO overflows, +%u event-queue-full, "
        "+%llu bytes discarded by the framer, +%llu RTCM frames, +%llu CRC errors\n",
        (unsigned)ms, (unsigned)((esp_timer_get_time() - t0) / 1000), (unsigned)(b.rx_buffer_full - a.rx_buffer_full),
        (unsigned)(b.hw_fifo_overflows - a.hw_fifo_overflows), (unsigned)(b.rx_events_dropped - a.rx_events_dropped),
        (unsigned long long)(b.framing.bytes_discarded - a.framing.bytes_discarded),
        (unsigned long long)(b.framing.rtcm_frames - a.framing.rtcm_frames),
        (unsigned long long)(b.framing.rtcm_crc_errors - a.framing.rtcm_crc_errors));
    return 0;
}

static int cmd_rawstall(int argc, char **argv)
{
    uint32_t ms = 0;
    if (argc != 2 || !parse_u32(argv[1], 1, GNSS_LINK_BENCH_STALL_SAFE_MS, &ms)) {
        say("usage: rawstall <ms 1-9000>\n");
        return 1;
    }
    if (!link_free()) {
        return 1;
    }
    esp_err_t err = gnss_link_bench_raw_stall_arm(ms);
    if (err != ESP_OK) {
        say("BENCH FAIL rawstall: %s\n", esp_err_to_name(err));
        return 1;
    }
    say("BENCH rawstall armed: the next read under a RAW lease (listen, version, entry, flash ...) within %u s "
        "waits %u ms once\n",
        (unsigned)GNSS_LINK_BENCH_RAW_ARM_S, (unsigned)ms);
    return 0;
}

/* The system clock as SNTP set it (nothing else sets it), and unix minus esp_timer. */
static bool sntp_clock(int64_t *unix_minus_timer_us)
{
    static network_status_t ns;
    if (network_get_status(&ns) != ESP_OK || !ns.time_synced) {
        return false;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *unix_minus_timer_us = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec - esp_timer_get_time();
    return true;
}

static void print_pct(const char *name, const gnss_link_pct_t *p, const char *unit)
{
    if (p->n == 0) {
        say("; %s: none", name);
        return;
    }
    say("; %s p50 %d p95 %d p99 %d p99.9 %d max %d %s (n %u)", name, (int)p->p50, (int)p->p95, (int)p->p99,
        (int)p->p999, (int)p->max, unit, (unsigned)p->n);
}

static int ftimes_dump(void)
{
    static char buf[1024];
    int64_t off = 0;
    bool clock = sntp_clock(&off);
    gnss_link_timing_info_t info;
    (void)gnss_link_bench_timing_read(0, NULL, 0, &info);
    int64_t now = esp_timer_get_time();
    say("# esp_timer_us=%lld,unix_ms=", (long long)now);
    if (clock) {
        say("%lld", (long long)((now + off) / 1000));
    }
    say(",clock=%s,records=%u,full=%u,running=%u\n", clock ? "sntp" : "none", (unsigned)info.count,
        info.full ? 1u : 0u, info.running ? 1u : 0u);
    say("%s", GNSS_LINK_CSV_HEADER);
    size_t cursor = 0;
    size_t n;
    while ((n = gnss_link_bench_timing_csv(&cursor, buf, sizeof(buf))) > 0) {
        diag_console_put(&s_out, buf, n);
    }
    say("BENCH ftimes dump end (%u records; also GET /api/diag/frame-times)\n", (unsigned)info.count);
    return 0;
}

/* The latest recording as `ftimes <s>` asked for it: its planned length, and
 * when `ftimes stop` ended it (0 = it ran to its end, or still runs). */
static uint32_t s_ft_secs;
static int64_t s_ft_stopped_us;

/* The summary line of the latest recording (running, stopped or ended by itself). */
static void ftimes_summary(void)
{
    gnss_link_timing_info_t info;
    (void)gnss_link_bench_timing_read(0, NULL, 0, &info);
    int64_t off = 0;
    bool clock = sntp_clock(&off);
    static gnss_link_timing_summary_t sum;
    (void)gnss_link_bench_timing_summary(clock, off, &sum);
    say("BENCH ftimes %u s", (unsigned)s_ft_secs);
    if (info.running) {
        int64_t left = info.until_us - esp_timer_get_time();
        say(" (still recording, %u s left)", (unsigned)(left > 0 ? (left + 999999) / 1000000 : 0));
    } else if (s_ft_stopped_us != 0) {
        say(" (stopped after %u s)", (unsigned)((s_ft_stopped_us - info.started_us) / 1000000));
    }
    say(": %u records%s, %u frames, %u passes with data, %u stalls", (unsigned)sum.records,
        sum.full ? " (ring full: recording ended early)" : "", (unsigned)sum.frames, (unsigned)sum.passes,
        (unsigned)sum.stalls);
    print_pct("read to published", &sum.read_to_pub_us, "us");
    print_pct("arrival to read", &sum.arrival_to_read_us, "us");
    say("; %u frames without an interrupt time", (unsigned)sum.frames_inexact);
    say("; longest reader gap with data waiting %u us", (unsigned)sum.longest_gap_us);
    if (sum.stalls > 0) {
        say("; after a stall: %u zero-wait capped passes, ring empty %u us after it, first frame %s%u us after it",
            (unsigned)sum.stall_capped_passes, (unsigned)sum.stall_drain_us,
            sum.stall_first_frame_valid ? "" : "(none) ", (unsigned)sum.stall_first_frame_us);
    }
    if (!sum.clock) {
        say("; epoch delay: no clock (the system time is not set by SNTP)");
    } else {
        static const char *const names[GNSS_SYS_COUNT] = { "gps", "glonass", "galileo", "sbas", "qzss", "beidou",
                                                           "navic" };
        for (unsigned s = 0; s < GNSS_SYS_COUNT; s++) {
            const gnss_link_pct_t *p = &sum.epoch_delay_ms[s];
            if (p->n > 0) {
                say("; epoch delay %s p50 %d p95 %d max %d ms (n %u)", names[s], (int)p->p50, (int)p->p95,
                    (int)p->max, (unsigned)p->n);
            }
        }
    }
    say("\n");
    /* The link's interrupt-time pairing since boot (its own line: the summary line above keeps its form). */
    gnss_link_arrival_stats_t a;
    if (gnss_link_get_arrival_stats(&a) == ESP_OK) {
        say("BENCH ftimes interrupt times since boot: %u frames with one, %u without, %u resyncs, %u mismatches, "
            "%u pairings dropped by a known cause\n",
            (unsigned)a.exact_frames, (unsigned)a.inexact_frames, (unsigned)a.resyncs, (unsigned)a.mismatches,
            (unsigned)a.degrades);
    }
}

static int cmd_ftimes(int argc, char **argv)
{
    gnss_link_timing_info_t info;
    (void)gnss_link_bench_timing_read(0, NULL, 0, &info);
    if (argc == 2 && strcmp(argv[1], "dump") == 0) {
        return ftimes_dump();
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        if (info.running) {
            int64_t left = info.until_us - esp_timer_get_time();
            say("BENCH ftimes status: running, %u records, %u s left\n", (unsigned)info.count,
                (unsigned)(left > 0 ? (left + 999999) / 1000000 : 0));
        } else {
            say("BENCH ftimes status: not running, %u records%s\n", (unsigned)info.count,
                info.full ? " (ring full: the recording ended early)" : "");
        }
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "stop") == 0) {
        if (info.running) {
            gnss_link_bench_timing_stop();
            s_ft_stopped_us = esp_timer_get_time();
        }
        ftimes_summary();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "summary") == 0) {
        ftimes_summary();
        return 0;
    }
    uint32_t secs = 0;
    if (argc != 2 || !parse_u32(argv[1], 1, GNSS_LINK_BENCH_TIMING_MAX_S, &secs)) {
        say("usage: ftimes <seconds 1-600>   |   ftimes status | stop | summary | dump\n");
        return 1;
    }
    if (info.running) {
        say("BENCH REFUSED: a recording runs (ftimes status, ftimes stop)\n");
        return 1;
    }
    if (!link_free()) {
        return 1;
    }
    esp_err_t err = gnss_link_bench_timing_start(secs);
    if (err != ESP_OK) {
        say("BENCH FAIL ftimes: %s\n", esp_err_to_name(err));
        return 1;
    }
    s_ft_secs = secs;
    s_ft_stopped_us = 0;
    say("BENCH ftimes: recording frame and drain-pass times for %u s in the background (stall, rawstall and "
        "listen may run meanwhile; ftimes status, stop, summary)\n",
        (unsigned)secs);
    return 0;
}

static void print_context(const gnss_link_prompt_obs_t *o)
{
    for (size_t i = 0; i < o->context_len; i++) {
        uint8_t c = o->context[i];
        if (c >= 0x20 && c < 0x7F && c != '\\') {
            say("%c", (char)c);
        } else {
            say("\\x%02x", c);
        }
    }
}

static int cmd_promptdet(int argc, char **argv)
{
    if (argc == 2 && (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0)) {
        esp_err_t err = gnss_link_bench_prompt_observe(strcmp(argv[1], "on") == 0);
        say("BENCH promptdet %s: %s\n", argv[1],
            err == ESP_OK ? "report-only (a hit is counted, nothing is ever sent for it)" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        gnss_link_prompt_obs_t o;
        (void)gnss_link_bench_prompt_status(&o);
        say("BENCH promptdet %s: %u hit(s)", o.on ? "on" : "off", (unsigned)o.hits);
        if (o.last_hit_ms >= 0) {
            say(", last at uptime %lld ms, context \"", (long long)o.last_hit_ms);
            print_context(&o);
            say("\"");
        }
        say("\n");
        return 0;
    }
    say("usage: promptdet on|off|status\n");
    return 1;
}

static int cmd_promptwatch(int argc, char **argv)
{
    uint32_t baud = 0;
    uint32_t secs = 0;
    if (argc != 3 || !parse_baud(argv[1], &baud) || !parse_u32(argv[2], 5, 600, &secs)) {
        say("usage: promptwatch <baud> <seconds 5-600>   (listen only: nothing is transmitted)\n");
        return 1;
    }
    station_bench_req_t req = { .op = STATION_BENCH_PROMPT_WATCH, .baud = baud, .seconds = secs };
    station_bench_res_t res;
    esp_err_t err = station_bench_run(&req, &res, (secs + 60u) * 1000u);
    say("BENCH %s\n", res.line[0] != '\0' ? res.line : esp_err_to_name(err));
    return (err == ESP_OK && res.pass) ? 0 : 1;
}

static int cmd_uartclose(int argc, char **argv)
{
    if (argc != 2 || !confirmed(argc, argv, TOK_UART)) {
        if (argc != 2) {
            say("usage: uartclose " TOK_UART "\n");
        }
        return 1;
    }
    if (!link_free()) {
        return 1;
    }
    esp_err_t err = gnss_link_bench_uart_close();
    if (err != ESP_OK) {
        say("BENCH FAIL uartclose: %s\n", esp_err_to_name(err));
        return 1;
    }
    int64_t t0 = esp_timer_get_time();
    gnss_link_stats_t st;
    say("BENCH uartclose: UART driver deleted, link_open false; waiting for the link's own reopen ...\n");
    while (esp_timer_get_time() - t0 < 120LL * 1000000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gnss_link_get_stats(&st) == ESP_OK && st.link_open) {
            say("BENCH uartclose: link_open true again after %u ms (failed opens since boot: %u)\n",
                (unsigned)((esp_timer_get_time() - t0) / 1000), (unsigned)st.open_failures);
            print_heap("after the reopen");
            return 0;
        }
    }
    say("BENCH FAIL uartclose: the link is still closed after 120 s (it keeps trying every 60 s)\n");
    return 1;
}

#endif /* CONFIG_GNSS_LINK_BENCH_HOOKS */

/* Internal blocks held by heapfrag (until "heapfrag free"). */
#define FRAG_MAX 512u
static void *s_frag[FRAG_MAX];
static unsigned s_frag_n;

static void frag_free(void)
{
    for (unsigned i = 0; i < s_frag_n; i++) {
        heap_caps_free(s_frag[i]);
        s_frag[i] = NULL;
    }
    s_frag_n = 0;
}

static int cmd_heapfrag(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "free") == 0) {
        unsigned n = s_frag_n;
        frag_free();
        say("BENCH heapfrag free: %u block(s) released\n", n);
        print_heap("now");
        return 0;
    }
    uint32_t kb = 0;
    if (argc != 3 || !parse_u32(argv[1], 8, 64, &kb)) {
        say("usage: heapfrag <largest free internal block in KB, 8-64> " TOK_HEAP "   |   heapfrag free\n");
        return 1;
    }
    if (!confirmed(argc, argv, TOK_HEAP)) {
        return 1;
    }
    if (s_frag_n > 0) {
        say("BENCH REFUSED: blocks are held already (heapfrag free first)\n");
        return 1;
    }
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t target = (size_t)kb * 1024u;
    print_heap("before");
    /* Blocks of the target size fill every larger free region; every other
     * one is freed again, leaving holes of the target size between held
     * blocks; holes that merged are filled again until no free block is
     * larger than the target. */
    static void *s_tmp[FRAG_MAX];
    unsigned tmp_n = 0;
    while (heap_caps_get_largest_free_block(caps) > target && tmp_n < FRAG_MAX) {
        void *p = heap_caps_malloc(target, caps);
        if (p == NULL) {
            break;
        }
        s_tmp[tmp_n++] = p;
    }
    for (unsigned i = 0; i < tmp_n; i++) {
        if ((i & 1u) == 0 || s_frag_n >= FRAG_MAX) {
            heap_caps_free(s_tmp[i]);
        } else {
            s_frag[s_frag_n++] = s_tmp[i];
        }
        s_tmp[i] = NULL;
    }
    while (heap_caps_get_largest_free_block(caps) > target && s_frag_n < FRAG_MAX) {
        void *p = heap_caps_malloc(target, caps);
        if (p == NULL) {
            break;
        }
        s_frag[s_frag_n++] = p;
    }
    size_t largest = heap_caps_get_largest_free_block(caps);
    say("BENCH heapfrag %u KB: %u block(s) of %u B held; largest free internal block %u B%s\n", (unsigned)kb,
        s_frag_n, (unsigned)target, (unsigned)largest, largest <= target ? "" : " (NOT reached: table full)");
    print_heap("after");
    return largest <= target ? 0 : 1;
}

static int cmd_linkrestart(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        say("usage: linkrestart   (the ladder's link restart now; nothing is sent to the receiver)\n");
        return 1;
    }
    static station_status_t st;
    if (station_get_status(&st) != ESP_OK) {
        say("BENCH REFUSED: no station status\n");
        return 1;
    }
    print_heap("before");
    gnss_link_stats_t a;
    (void)gnss_link_get_stats(&a);
    if (st.state == STATION_STATE_STREAMING) {
        /* Through the station, as the health ladder asks for it. */
        esp_err_t err = station_request_recovery(STATION_RECOVER_LINK_RESTART);
        if (err != ESP_OK) {
            say("BENCH REFUSED linkrestart: %s\n", esp_err_to_name(err));
            return 1;
        }
        int64_t t0 = esp_timer_get_time();
        do {
            vTaskDelay(pdMS_TO_TICKS(100));
        } while (station_get_status(&st) == ESP_OK && st.state == STATION_STATE_RECOVERING &&
                 esp_timer_get_time() - t0 < 10LL * 1000000);
        say("BENCH linkrestart through the recovery ladder's step: station %s\n",
            st.state == STATION_STATE_STREAMING ? "streaming again" : "not streaming (see the log)");
    } else {
        /* No receiver streaming (e.g. a board without one): the same restart as a station job. */
        station_bench_req_t req = { .op = STATION_BENCH_LINK_RESTART };
        station_bench_res_t res;
        esp_err_t err = station_bench_run(&req, &res, 30000u);
        say("BENCH %s\n", res.line[0] != '\0' ? res.line : esp_err_to_name(err));
        if (err != ESP_OK || !res.pass) {
            return 1;
        }
    }
    gnss_link_stats_t b;
    (void)gnss_link_get_stats(&b);
    say("BENCH linkrestart: link_open %s, failed opens %u -> %u, flushed bytes +%llu\n", b.link_open ? "true" : "false",
        (unsigned)a.open_failures, (unsigned)b.open_failures, (unsigned long long)(b.flushed_bytes - a.flushed_bytes));
    print_heap("after");
    return b.link_open ? 0 : 1;
}

/* ---- Wi-Fi: a move to another access point -------------------------------------------- */

#define MOVE_START_MS 3000u    /* the move shows (the association ends) within this long, or it was refused */
#define MOVE_MAX_MS   40000u   /* the move is over (the address on the new link, or DHCP's answer) within this long */

/*
 * The station's own move now (network_move_now), watched until it is over:
 * PASS when the station stayed online throughout with the same address (the
 * address and the connections kept), FAIL when it went offline or the move
 * did not end. Nothing is sent to the receiver.
 */
static int cmd_wifimove(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        say("usage: wifimove   (the station's own move to the strongest access point of its network now)\n");
        return 1;
    }
    static network_status_t a;
    static network_status_t st;
    if (network_get_status(&a) != ESP_OK || !a.online || !a.associated) {
        say("BENCH REFUSED wifimove: not online\n");
        return 1;
    }
    esp_err_t err = network_move_now();
    if (err != ESP_OK) {
        say("BENCH REFUSED wifimove: %s\n", esp_err_to_name(err));
        return 1;
    }
    int64_t t0 = esp_timer_get_time();
    bool left = false;
    bool offline = false;
    bool changed = false;
    uint32_t back_ms = 0;
    uint32_t ms = 0;
    while (ms < MOVE_MAX_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
        ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        if (network_get_status(&st) != ESP_OK) {
            continue;
        }
        offline |= !st.online;
        changed |= st.online && strcmp(st.ip, a.ip) != 0;
        if (!st.associated) {
            left = true;
        } else if (left && back_ms == 0) {
            back_ms = ms;
        }
        if (!left && ms >= MOVE_START_MS) {
            say("BENCH REFUSED wifimove: the station did not move (see the log for why)\n");
            return 1;
        }
        if (left && st.associated && st.online && !st.moving) {
            break;
        }
    }
    if (ms >= MOVE_MAX_MS) {
        say("BENCH FAIL wifimove: not over after %u s (see the log)\n", (unsigned)(MOVE_MAX_MS / 1000));
        return 1;
    }
    bool kept = !offline && !changed;
    say("BENCH %s wifimove: channel %u -> %u, %d -> %d dBm, associated again after %u ms, address %s%s%s\n",
        kept ? "PASS" : "FAIL", a.channel, st.channel, a.rssi, st.rssi, (unsigned)back_ms, a.ip,
        kept      ? " kept (online throughout)"
        : changed ? " changed to "
                  : " lost for a while (offline)",
        changed ? st.ip : "");
    return kept ? 0 : 1;
}

/* ---- console check: log lines from another task ---------------------------------------- */

#define FLOOD_PRIO  3      /* above the command line's task: its lines come while a reply is printed */
#define FLOOD_STACK 3072   /* a log line is formatted on the logging task's stack */

static volatile bool s_flood_running;
static uint32_t s_flood_lines;
static uint32_t s_flood_ms;

static void flood_task(void *arg)
{
    (void)arg;
    uint32_t n = s_flood_lines;
    TickType_t last = xTaskGetTickCount();
    TickType_t period = pdMS_TO_TICKS(s_flood_ms) > 0 ? pdMS_TO_TICKS(s_flood_ms) : 1;
    for (uint32_t i = 1; i <= n; i++) {
        ESP_LOGI("logflood", "line %u of %u: a log line from another task while the command line answers",
                 (unsigned)i, (unsigned)n);
        vTaskDelayUntil(&last, period);
    }
    ESP_LOGI("logflood", "done: %u lines", (unsigned)n);
    s_flood_running = false;
    vTaskDelete(NULL);
}

static int cmd_logflood(int argc, char **argv)
{
    uint32_t secs = 0;
    uint32_t ms = 10;
    if ((argc != 2 && argc != 3) || !parse_u32(argv[1], 1, 120, &secs) ||
        (argc == 3 && !parse_u32(argv[2], 1, 1000, &ms))) {
        say("usage: logflood <seconds 1-120> [ms between lines 1-1000, normally 10]\n");
        return 1;
    }
    if (s_flood_running) {
        say("BENCH REFUSED: a log flood runs\n");
        return 1;
    }
    s_flood_lines = secs * 1000u / ms;
    s_flood_ms = ms;
    s_flood_running = true;
    if (xTaskCreatePinnedToCore(flood_task, "logflood", FLOOD_STACK, NULL, FLOOD_PRIO, NULL, tskNO_AFFINITY) !=
        pdPASS) {
        s_flood_running = false;
        say("BENCH FAIL logflood: no memory for its task\n");
        return 1;
    }
    say("BENCH logflood: %u log lines, one every %u ms, from another task in the background (commands keep "
        "working meanwhile; the last line says done)\n",
        (unsigned)s_flood_lines, (unsigned)ms);
    return 0;
}

/* The commands as registered, for `help` (esp_console's own help prints
 * through stdout, a byte at a time, where log lines could land inside its
 * lines). */
#define CMD_MAX 40
static struct {
    const char *name;
    const char *help;
} s_cmds[CMD_MAX];
static size_t s_ncmds;

static void reg(const char *name, const char *help, esp_console_cmd_func_t fn)
{
    const esp_console_cmd_t c = { .command = name, .help = help, .func = fn };
    esp_err_t err = s_ncmds < CMD_MAX ? esp_console_cmd_register(&c) : ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "command %s not registered: %s", name, esp_err_to_name(err));
        return;
    }
    s_cmds[s_ncmds].name = name;
    s_cmds[s_ncmds].help = help;
    s_ncmds++;
}

/* One line per command: its name, then what it does and how to type it. */
static int cmd_help(int argc, char **argv)
{
    if (argc > 2) {
        say("usage: help [command]\n");
        return 1;
    }
    int width = 0;
    for (size_t i = 0; i < s_ncmds; i++) {
        int n = (int)strlen(s_cmds[i].name);
        width = n > width ? n : width;
    }
    bool found = false;
    for (size_t i = 0; i < s_ncmds; i++) {
        if (argc == 2 && strcmp(argv[1], s_cmds[i].name) != 0) {
            continue;
        }
        say("%-*s  %s\n", width, s_cmds[i].name, s_cmds[i].help);
        found = true;
    }
    if (!found) {
        say("help: no command %s (help alone lists them all)\n", argv[1]);
        return 1;
    }
    return 0;
}

/* ---- public -------------------------------------------------------------------------- */

esp_err_t bench_boot_early(void)
{
    uint8_t armed = 0;
    if (!settings_is_available() ||
        settings_get_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_BENCH_RACE, &armed) != ESP_OK || armed == 0) {
        return ESP_OK;
    }
    /* One-shot: cleared before it runs, so a crash in it cannot repeat it. */
    esp_err_t err = settings_erase_state_key(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_BENCH_RACE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stop key from boot not run: the one-shot flag could not be cleared (%s)", esp_err_to_name(err));
        return err;
    }
    ESP_LOGW(TAG, "stop key from boot (armed for this boot), %u ms after the ESP32 reset",
             (unsigned)(esp_timer_get_time() / 1000));
    station_bench_res_t res;
    err = station_bench_boot_race(RACE_MAX_MS, &res);
    snprintf(s_race_line, sizeof(s_race_line), "%s", res.line[0] != '\0' ? res.line : esp_err_to_name(err));
    say("BENCH %s\n", s_race_line);
    say_end();
    return err;
}

/* ---- the command line ------------------------------------------------------------------ */

#define CONSOLE_UART   CONFIG_ESP_CONSOLE_UART_NUM
#define CONSOLE_RX_BUF 256    /* the driver needs more than the 128-byte FIFO */
#define REPL_PROMPT    "bench> "
#define REPL_STACK     6144   /* internal RAM: commands write settings and the journal (flash) */
#define REPL_PRIO      2
#define REPL_HISTORY   32

/* Input through the console UART's driver as it is. The log tap (diag)
 * installs that driver at boot, with the transmit buffer its console writer
 * needs, and writes every log line through it. IDF's REPL constructors
 * (esp_console_new_repl_uart, and esp_console_new_repl_stdio through
 * esp_stdio_install_io_driver) install a driver of their own; with one
 * already there that fails, and their error path then deletes the installed
 * driver: no command line, and every log line after it fails in the driver.
 * So nothing is installed here while a driver is there. */
static esp_err_t console_input(void)
{
    if (!uart_is_driver_installed(CONSOLE_UART)) {
        /* The log tap could not install it (it logged why): reads need a driver. */
        esp_err_t err = uart_driver_install(CONSOLE_UART, CONSOLE_RX_BUF, 0, 0, NULL, 0);
        if (err != ESP_OK) {
            return err;
        }
    }
    /* Terminals send CR for Enter and linenoise ends a line at LF. Output
     * keeps the console's configured line ending, which the log tap's own
     * writes follow too. */
    if (uart_vfs_dev_port_set_rx_line_endings(CONSOLE_UART, ESP_LINE_ENDINGS_CR) != 0) {
        return ESP_FAIL;
    }
    /* stdin (and stdout, as the log tap already set it) through the driver:
     * reads block on its receive buffer instead of polling the FIFO. */
    uart_vfs_dev_use_driver(CONSOLE_UART);
    return ESP_OK;
}

/* esp_console and linenoise as IDF's REPL sets them up. */
static esp_err_t repl_init(void)
{
    esp_console_config_t cc = ESP_CONSOLE_CONFIG_DEFAULT();
#if CONFIG_LOG_COLORS
    cc.hint_color = atoi(LOG_COLOR_CYAN);
#else
    cc.hint_color = -1;
#endif
    esp_err_t err = esp_console_init(&cc);
    if (err != ESP_OK) {
        return err;
    }
    if (linenoiseHistorySetMaxLen(REPL_HISTORY) != 1) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK && linenoiseSetMaxLineLen(cc.max_cmdline_length) != 0) {
        err = ESP_ERR_INVALID_ARG;
    }
    if (err != ESP_OK) {
        (void)esp_console_deinit();
        return err;
    }
    linenoiseSetMultiLine(1);
    linenoiseSetCompletionCallback(&esp_console_get_completion);
    linenoiseSetHintsCallback((linenoiseHintsCallback *)&esp_console_get_hint);
    /* A terminal that does not answer the status request (a capture, a
     * script on a socket) gets plain line input without escape sequences. */
    if (linenoiseProbe() != 0) {
        linenoiseSetDumbMode(1);
    }
    return ESP_OK;
}

static void repl_task(void *arg)
{
    (void)arg;
    /* Unbuffered: linenoise reads one byte at a time. */
    setvbuf(stdin, NULL, _IONBF, 0);
    say("\nBench console: type help for the commands.\n");
    for (;;) {
        const char *prompt = REPL_PROMPT;
        if (linenoiseIsDumbMode()) {
            /* The prompt goes out whole too (linenoise would write it a byte
             * at a time); a dumb terminal never has it redrawn. */
            say("%s", REPL_PROMPT);
            diag_console_flush(&s_out, false);
            prompt = "";
        }
        char *line = linenoise(prompt);
        if (line == NULL) {
            /* A read error: try again without spinning. */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (line[0] != '\0') {
            (void)linenoiseHistoryAdd(line);
        }
        int ret = 0;
        esp_err_t err = esp_console_run(line, &ret);
        if (err == ESP_ERR_NOT_FOUND) {
            say("Unrecognized command\n");
        } else if (err == ESP_OK && ret != 0) {
            say("Command returned non-zero error code: 0x%x (%s)\n", ret, esp_err_to_name(ret));
        } else if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {   /* INVALID_ARG: an empty line */
            say("Internal error: %s\n", esp_err_to_name(err));
        }
        say_end();
        linenoiseFree(line);
    }
}

esp_err_t bench_start(void)
{
    esp_err_t err = console_input();
    if (err == ESP_OK) {
        err = repl_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bench console not started: %s", esp_err_to_name(err));
        return err;
    }
    reg("help", "The commands, or one of them: help [command]", cmd_help);
    reg("listen", "Listen only on the receiver's COM1: listen <baud> <seconds>", cmd_listen);
    reg("version", "The version query: version <baud> " TOK_SEND, cmd_version);
    reg("listen_pc", "Listen only while a PC flashes the receiver over its other port: listen_pc <baud> <seconds>",
        cmd_listen_pc);
    reg("entry", "Loader entry probe, banner and menu captured, then \"6\": entry <460800|115200> " TOK_RESET,
        cmd_entry);
    reg("menu2", "Menu item 2, start bytes recorded, CAN CAN, \"6\" (never a data frame): menu2 20 " TOK_RESET,
        cmd_menu2);
    reg("idle", "The loader left alone at its prompt, then \"6\": idle 300 " TOK_RESET, cmd_idle);
    reg("race_arm", "Stop key from the earliest point of the NEXT boot only: race_arm " TOK_RACE, cmd_race_arm);
    reg("race_result", "The result of this boot's stop key from boot", cmd_race_result);
    reg("flash", "Full flash of the staged package: flash <sha256 first 8 hex> <expected firmware> " TOK_FLASH,
        cmd_flash);
    reg("abort", "Stop sending after N blocks, watch, then the retry: abort <N> <sha8> <expected fw> " TOK_FLASH,
        cmd_abort);
    reg("reset_at", "Flash and restart the ESP32 right after a journal state: reset_at <state> <sha8> <fw> "
                    TOK_FLASH,
        cmd_reset_at);
    reg("jstate", "Write a journal in a state, nothing sent: jstate <state> " TOK_JOURNAL, cmd_jstate);
    reg("jclear", "Clear the receiver update journal: jclear " TOK_CLEAR, cmd_jclear);
    reg("restart", "Software restart of the ESP32: restart " TOK_RESTART, cmd_restart);
    reg("safemode", "Restart into safe mode (a failed boot at the crash limit): safemode " TOK_SAFE, cmd_safemode);
    reg("status", "Station state, journal and the last session", cmd_status);
    reg("capture", "Print the byte capture (last KB): capture [KB]", cmd_capture);
#if CONFIG_GNSS_LINK_BENCH_HOOKS
    reg("stall", "RX task stall, no reads, no watchdog feed: stall <ms 1-9000> | stall <ms 10000-30000> " TOK_TWDT,
        cmd_stall);
    reg("rawstall", "Stall once inside the next RAW-lease read (within 60 s): rawstall <ms 1-9000>", cmd_rawstall);
    reg("ftimes", "Record frame and drain-pass times in the background: ftimes <seconds 1-600> | ftimes status | "
                  "ftimes stop | ftimes summary | ftimes dump",
        cmd_ftimes);
    reg("promptdet", "Loader prompt detector, report-only (nothing sent): promptdet on|off|status", cmd_promptdet);
    reg("promptwatch", "Listen for the loader prompt at a baud, nothing sent: promptwatch <baud> <seconds 5-600>",
        cmd_promptwatch);
    reg("uartclose", "Delete the receiver UART driver; the link reopens it: uartclose " TOK_UART, cmd_uartclose);
#endif
    reg("heapfrag", "Hold internal heap blocks so no free block is larger: heapfrag <KB 8-64> " TOK_HEAP
                    " | heapfrag free",
        cmd_heapfrag);
    reg("linkrestart", "The ladder's link restart now (nothing sent to the receiver): linkrestart", cmd_linkrestart);
    reg("wifimove", "The station's own move to the strongest access point of its network now, watched: wifimove",
        cmd_wifimove);
    reg("logflood", "Log lines from another task in the background, to check that replies stay whole: logflood "
                    "<seconds 1-120> [ms between lines 1-1000]",
        cmd_logflood);
    if (xTaskCreatePinnedToCore(repl_task, "console_repl", REPL_STACK, NULL, REPL_PRIO, NULL, tskNO_AFFINITY) !=
        pdPASS) {
        ESP_LOGE(TAG, "bench console not started: no memory for its task");
        (void)esp_console_deinit();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGW(TAG, "bench console on the UART%d console (this is a bench build, never a release); type help",
             CONSOLE_UART);
    return ESP_OK;
}

#else

esp_err_t bench_boot_early(void)
{
    return ESP_OK;
}

esp_err_t bench_start(void)
{
    return ESP_OK;
}

#endif
