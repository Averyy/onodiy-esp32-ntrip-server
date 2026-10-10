/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Bench console (see bench.h). The commands follow the receiver flasher's
 * bench plan one step at a time; each prints one PASS/FAIL line.
 */
#include "bench.h"

#include "sdkconfig.h"

#if CONFIG_BENCH_CONSOLE

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lifecycle.h"
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

#define RUN_TIMEOUT_MS (20u * 60u * 1000u)
#define RACE_MAX_MS    10000u

static char s_race_line[200];

/* ---- helpers ------------------------------------------------------------------------ */

static bool confirmed(int argc, char **argv, const char *token)
{
    if (argc >= 2 && strcmp(argv[argc - 1], token) == 0) {
        return true;
    }
    printf("BENCH REFUSED: this writes to the receiver or the journal. Repeat it with the word %s at the end.\n",
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
        printf("BENCH baud: 115200, 230400, 460800 or 921600\n");
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
    printf("BENCH %s\n", res.line[0] != '\0' ? res.line : esp_err_to_name(err));
    return (err == ESP_OK && res.pass) ? 0 : 1;
}

/* The staged package, confirmed by the first 8 hex digits of its SHA-256 and the build it should produce. */
static bool flash_request(const char *sha8, const char *expected_fw, station_flash_request_t *req)
{
    pkg_store_info_t info;
    if (pkg_store_get_info(&info) != ESP_OK || !info.valid) {
        printf("BENCH REFUSED: no package staged (upload it on the web UI's receiver update page first)\n");
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
        printf("BENCH REFUSED: the staged package's SHA-256 starts with %s, not %s\n", hex, sha8);
        return false;
    }
    const char *b = strstr(expected_fw, "Build");
    uint32_t build = 0;
    if (b == NULL || strlen(expected_fw) >= sizeof(req->expected_fw) || !parse_u32(b + 5, 1, 0x7FFFFFFF, &build)) {
        printf("BENCH REFUSED: the expected firmware must look like R4.10Build18975\n");
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
    printf("BENCH state: prepared, baud-changing, reset-sent, menu-2-sent, transferring, eot-sent, verifying, "
           "failed, needs-user\n");
    return 0;
}

/* ---- commands ------------------------------------------------------------------------ */

static int cmd_listen(int argc, char **argv)
{
    uint32_t baud;
    uint32_t secs;
    if (argc != 3 || !parse_baud(argv[1], &baud) || !parse_u32(argv[2], 1, 600, &secs)) {
        printf("usage: listen <baud> <seconds 1-600>\n");
        return 1;
    }
    return run(STATION_BENCH_LISTEN, baud, secs, false);
}

static int cmd_listen_pc(int argc, char **argv)
{
    uint32_t baud;
    uint32_t secs;
    if (argc != 3 || !parse_baud(argv[1], &baud) || !parse_u32(argv[2], 1, 3600, &secs)) {
        printf("usage: listen_pc <baud> <seconds 1-3600>   (while a PC flashes the receiver over its other port)\n");
        return 1;
    }
    return run(STATION_BENCH_LISTEN, baud, secs, true);
}

static int cmd_version(int argc, char **argv)
{
    uint32_t baud;
    if (argc != 3 || !parse_baud(argv[1], &baud)) {
        printf("usage: version <baud> " TOK_SEND "\n");
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
        printf("usage: entry <460800|115200> " TOK_RESET "\n");
        return 1;
    }
#if !CONFIG_UM98X_FLASH_FALLBACK_115200
    if (baud == 115200) {
        printf("BENCH REFUSED: the 115200 entry is built in only with UM98X_FLASH_FALLBACK_115200\n");
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
        printf("usage: menu2 <seconds of start bytes 5-120, normally 20> " TOK_RESET "\n");
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
        printf("usage: idle <seconds 10-1800, normally 300> " TOK_RESET "\n");
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
            printf("usage: race_arm " TOK_RACE "\n");
        }
        return 1;
    }
    esp_err_t err = settings_set_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_BENCH_RACE, 1);
    printf("BENCH %s\n", err == ESP_OK ? "armed for the next boot only: power-cycle the board now (the result prints "
                                         "at boot and with race_result)"
                                       : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_race_result(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("BENCH %s\n", s_race_line[0] != '\0' ? s_race_line : "no stop-key-from-boot run this boot");
    return 0;
}

static int start_flash(const char *sha8, const char *fw, uint32_t stop_after, uint8_t reset_at)
{
    station_flash_request_t req;
    if (!flash_request(sha8, fw, &req)) {
        return 1;
    }
    esp_err_t err = station_bench_flash(&req, stop_after, reset_at);
    if (err != ESP_OK) {
        printf("BENCH REFUSED: %s (the station must stream, no journal, no app update or trial, no maintenance)\n",
               esp_err_to_name(err));
        return 1;
    }
    printf("BENCH started: every byte is logged; `status` shows the outcome when it ends\n");
    return 0;
}

static int cmd_flash(int argc, char **argv)
{
    if (argc != 4) {
        printf("usage: flash <first 8 hex of the staged SHA-256> <expected firmware, e.g. R4.10Build18975> " TOK_FLASH
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
        printf("usage: abort <blocks before stopping> <first 8 hex of the SHA-256> <expected firmware> " TOK_FLASH "\n");
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
        printf("usage: reset_at <journal state> <first 8 hex of the SHA-256> <expected firmware> " TOK_FLASH "\n");
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
    if (argc != 3) {
        printf("usage: jstate <journal state> " TOK_JOURNAL "\n");
        return 1;
    }
    uint8_t st = parse_state(argv[1]);
    if (st == 0 || !confirmed(argc, argv, TOK_JOURNAL)) {
        return 1;
    }
    esp_err_t err = station_bench_journal_set(st);
    printf("BENCH %s\n", err == ESP_OK ? "journal written (nothing sent to the receiver): restart the ESP32 "
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
    printf("BENCH %s\n", err == ESP_OK ? "journal cleared" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_restart(int argc, char **argv)
{
    if (argc != 2 || !confirmed(argc, argv, TOK_RESTART)) {
        return 1;
    }
    esp_err_t err = lifecycle_request_reboot(LIFECYCLE_REBOOT_USER);
    printf("BENCH %s\n", err == ESP_OK ? "restart requested (it waits for a held reboot inhibit)" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static station_status_t st;
    if (station_get_status(&st) != ESP_OK) {
        printf("BENCH no status\n");
        return 1;
    }
    printf("BENCH state %d, receiver %s %s, journal %s (%s, phase %s, %u transfer(s)), flashing %s\n", (int)st.state,
           st.receiver.model, st.receiver.firmware,
           st.flash_journal_settings_unavailable ? "unknown" : st.flash_journal_open ? "open" : "none",
           um98x_flash_journal_state_name(st.flash_journal.state), um98x_flash_journal_state_name(st.flash_journal.phase),
           (unsigned)st.flash_journal.transfers, st.flash_available ? "built in" : "not built in");
    printf("BENCH progress: phase %d, %u/%u bytes, %u/%u frames, %u retries; last: outcome %u, rule %u, %s\n",
           (int)st.flash.phase, (unsigned)st.flash.bytes_sent, (unsigned)st.flash.bytes_total,
           (unsigned)st.flash.frames_acked, (unsigned)st.flash.frames_total, (unsigned)st.flash.retries,
           (unsigned)st.flash_last.outcome, (unsigned)st.flash_last.failed_rule, st.flash_last.reason);
    return 0;
}

static int cmd_capture(int argc, char **argv)
{
    uint32_t kb = 4;
    if (argc == 2 && !parse_u32(argv[1], 1, 256, &kb)) {
        printf("usage: capture [last KB, 1-256]\n");
        return 1;
    }
    if (!um98x_flash_capture_available()) {
        printf("BENCH no capture in this build\n");
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
        fputs(buf, stdout);
    }
    printf("\nBENCH capture end (download it all from the web UI: Diagnostics)\n");
    return 0;
}

static void reg(const char *name, const char *help, esp_console_cmd_func_t fn)
{
    const esp_console_cmd_t c = { .command = name, .help = help, .func = fn };
    esp_err_t err = esp_console_cmd_register(&c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "command %s not registered: %s", name, esp_err_to_name(err));
    }
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
    esp_err_t err = settings_erase_key(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_BENCH_RACE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stop key from boot not run: the one-shot flag could not be cleared (%s)", esp_err_to_name(err));
        return err;
    }
    ESP_LOGW(TAG, "stop key from boot (armed for this boot), %u ms after the ESP32 reset",
             (unsigned)(esp_timer_get_time() / 1000));
    station_bench_res_t res;
    err = station_bench_boot_race(RACE_MAX_MS, &res);
    snprintf(s_race_line, sizeof(s_race_line), "%s", res.line[0] != '\0' ? res.line : esp_err_to_name(err));
    printf("BENCH %s\n", s_race_line);
    return err;
}

esp_err_t bench_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "bench> ";
    rc.task_stack_size = 6144;
    esp_console_dev_uart_config_t uc = {
        .channel = CONFIG_ESP_CONSOLE_UART_NUM,
        .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .tx_gpio_num = -1,
        .rx_gpio_num = -1,
    };
    esp_err_t err = esp_console_new_repl_uart(&uc, &rc, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bench console not started: %s", esp_err_to_name(err));
        return err;
    }
    (void)esp_console_register_help_command();
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
    reg("status", "Station state, journal and the last session", cmd_status);
    reg("capture", "Print the byte capture (last KB): capture [KB]", cmd_capture);
    err = esp_console_start_repl(repl);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "bench console on the UART0 console (this is a bench build, never a release); type help");
    }
    return err;
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
