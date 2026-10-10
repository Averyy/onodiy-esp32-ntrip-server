/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "fl_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fl_crc.h"
#include "fl_rx.h"

#define US(ms) ((uint64_t)(ms) * 1000u)
#define T1_LATE_MS (180000u + 20000u)

static uint32_t s_garbage_i;
static uint32_t s_tx_baud[SIM_OUT_MAX];

uint32_t sim_crc32(const char *s, size_t n)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint8_t)s[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
    }
    return crc;
}

uint32_t sim_now_ms(sim_t *s)
{
    return s->base_ms + (uint32_t)(s->now_us / 1000u);
}

static uint64_t wire_us(size_t n, uint32_t baud)
{
    return ((uint64_t)n * 10u * 1000000u + baud - 1u) / baud;
}

/* Receiver's transmit baud in its current state. */
static uint32_t tx_baud(const sim_t *s)
{
    switch (s->st) {
    case SIM_MENU:
    case SIM_SUBMENU:
    case SIM_XRX:
    case SIM_COMMIT:
        return s->loader_baud;
    case SIM_GONE:
        return 115200u;
    default:
        return s->com1_baud;
    }
}

static void emit_at(sim_t *s, const void *data, size_t len, uint64_t at_us, uint32_t baud)
{
    const uint8_t *p = data;
    uint64_t t = at_us > s->out_tail_us ? at_us : s->out_tail_us;
    for (size_t i = 0; i < len; i++) {
        if (s->out_len >= SIM_OUT_MAX) {
            return;
        }
        t += wire_us(1, baud);
        size_t k = (s->out_head + s->out_len) % SIM_OUT_MAX;
        s->out_due[k] = t;
        s->out_b[k] = p[i];
        s_tx_baud[k] = baud;
        s->out_len++;
    }
    s->out_tail_us = t;
}

static void emit_delay(sim_t *s, const void *data, size_t len, uint32_t delay_ms)
{
    emit_at(s, data, len, s->now_us + US(delay_ms), tx_baud(s));
}

static void emit(sim_t *s, const char *text, uint32_t delay_ms)
{
    emit_delay(s, text, strlen(text), delay_ms);
}

static void emit_byte(sim_t *s, uint8_t b, uint32_t delay_ms)
{
    emit_delay(s, &b, 1, delay_ms);
}

static void nmea(char *out, size_t cap, const char *body)
{
    uint8_t x = 0;
    for (const char *p = body; *p; p++) {
        x ^= (uint8_t)*p;
    }
    snprintf(out, cap, "$%s*%02X\r\n", body, (unsigned)x);
}

static void emit_versiona(sim_t *s)
{
    char content[300];
    snprintf(content, sizeof(content),
             "VERSIONA,COM1,0,55.0,FINESTEERING,2190,1234.000,0,0,0;\"%s\",\"R4.10Build%u\",\"SYNTH-AUTH\","
             "\"1234567890123-SYNTH00000001\",\"0123456789abcdef\",\"2025/04/18\"",
             s->model, (unsigned)(s->flashed ? s->build_after : s->build_before));
    char line[340];
    snprintf(line, sizeof(line), "#%s*%08x\r\n", content, (unsigned)sim_crc32(content, strlen(content)));
    emit(s, line, 20);
}

static void emit_gga(sim_t *s, uint32_t delay_ms)
{
    char line[96];
    nmea(line, sizeof(line), "GNGGA,000000.00,,,,,0,00,,,,,,,");
    emit(s, line, delay_ms);
}

static void emit_prompt(sim_t *s, uint32_t delay_ms)
{
    if (s->split_prompt) {
        emit(s, "\r\nbo", delay_ms);
        emit(s, "ot> ", 30);
    } else {
        emit(s, "\r\nboot> ", delay_ms);
    }
}

static void set_state(sim_t *s, sim_state_t st)
{
    s->st = st;
    s->t_state_us = s->now_us;
}

static void loader_banner(sim_t *s)
{
    char b[128];
    snprintf(b, sizeof(b), "\r\nuart baudrate: %u\r\nTimeout: 2s\r\nDefault action: 0\r\n", (unsigned)s->loader_baud);
    emit(s, b, 2);
    emit(s,
         "0 Load OS\r\n1 Load FW only\r\n2 Download from uart to flash\r\n3 Download from uart and run\r\n"
         "4 Set boot config\r\n5 Menu cmd\r\n6 Soft reset cpu\r\n7 Dump efuse\r\n8 Update factory image\r\n"
         "9 Download from spi to flash\r\n10 Program efuse from uart\r\n",
         0);
    emit_prompt(s, 0);
}

static void app_start(sim_t *s)
{
    if (s->app_dead_after && s->flashed) {
        /* The new application does not start: the loader stays. */
        set_state(s, SIM_MENU);
        loader_banner(s);
        return;
    }
    set_state(s, SIM_APP);
    s->com1_baud = s->app_baud;
    s->quiet = false;
    s->next_periodic_us = s->now_us + US(s->app_period_ms);
    if (s->banner) {
        char line[64];
        nmea(line, sizeof(line), "devicename,COM1");
        emit(s, line, 1);
    }
}

static void start_xrx(sim_t *s)
{
    set_state(s, SIM_XRX);
    s->started = false;
    s->ready_us = s->out_tail_us > s->now_us ? s->out_tail_us : s->now_us;
    s->next_start_us = s->ready_us + US(s->start_delay_ms);
    s->expect = 1;
    s->flen = 0;
    s->img_len = 0;
    s->nak_done = 0;
    s->c_done = false;
    s->dup_done = false;
    s->silent = false;
    s->eot_count = 0;
}

static void menu2(sim_t *s)
{
    s->m2_count++;
    sim_menu2_t m = s->menu2;
    if (m == SIM_M2_INVALID_ONCE && s->invalid_seen == 0) {
        s->invalid_seen++;
        emit(s, "\r\nInvalid input\r\n", 5);
        emit_prompt(s, 0);
        return;
    }
    switch (m) {
    case SIM_M2_SUBMENU:
        emit(s, "\r\nChoose firmware for update\r\n1 bootloader\r\n2 whole package\r\n", 5);
        set_state(s, SIM_SUBMENU);
        return;
    case SIM_M2_INVALID_ALWAYS:
        emit(s, "\r\nInvalid input\r\n", 5);
        emit_prompt(s, 0);
        return;
    case SIM_M2_INPUT_TIMEOUT:
        emit(s, "\r\nGet input timeout\r\n", 5);
        emit_prompt(s, 0);
        return;
    case SIM_M2_ABORTED:
        emit(s, "\r\n## Binary download aborted\r\n", 5);
        emit_prompt(s, 0);
        return;
    case SIM_M2_PROMPT_ONLY:
        emit_prompt(s, 5);
        return;
    case SIM_M2_SILENT:
        return;
    case SIM_M2_SILENT_FIRST:
        if (s->m2_count == 1) {
            return;
        }
        break;
    default:
        break;
    }
    if (s->begin_text) {
        emit(s, "\r\nbegin to update flash\r\n", 5);
    }
    /* The address holds 'C's: a start byte must never be taken from it. */
    emit(s, "## Ready for binary (xmodem) download to 0x00C0C000 at 460800 bps...\r\n", 5);
    if (s->c_text_before_start) {
        emit(s, "Checking flash\r\n", 50);
    }
    start_xrx(s);
}

static void app_line(sim_t *s, const char *line)
{
    if (s->mute_after_config && s->rx_config_com1 > 0 && strncmp(line, "config com1 ", 12) != 0) {
        return;
    }
    if (strcmp(line, "VERSIONA") == 0) {
        s->rx_versiona++;
        emit_versiona(s);
    } else if (strcmp(line, "unlog") == 0) {
        s->quiet = true;
    } else if (strncmp(line, "config com1 ", 12) == 0) {
        s->rx_config_com1++;
        if (s->mute_after_config && s->rx_config_com1 > 1) {
            return;
        }
        uint32_t b = (uint32_t)strtoul(line + 12, NULL, 10);
        char reply[96];
        char body[80];
        snprintf(body, sizeof(body), "command,config com1 %u,response: OK", (unsigned)b);
        nmea(reply, sizeof(reply), body);
        emit(s, reply, 5);
        if (!s->ignore_baud_change && b != 0) {
            s->com1_baud = b;
        }
    } else if (strcmp(line, "gpgga 1") == 0) {
        emit_gga(s, 200);
    } else if (strcmp(line, "reset") == 0) {
        s->rx_resets++;
        set_state(s, SIM_RESETTING);
        s->line_len = 0;
    }
}

static void menu_line(sim_t *s, const char *line)
{
    if (line[0] == '\0') {
        emit_prompt(s, 2);
        return;
    }
    char *end = NULL;
    long n = strtol(line, &end, 10);
    if (end == line || *end != '\0' || n < 0 || n > 15) {
        emit(s, "\r\nInvalid input\r\n", 2);
        emit_prompt(s, 0);
        return;
    }
    s->rx_menu[n]++;
    if (n == 2) {
        menu2(s);
    } else if (n == 6) {
        s->rx_six++;
        if (s->transfer_failed && !s->flashed) {
            s->rx_six_after_ponr++;
        }
        if (s->exit_needs_second_6 && s->six_count++ == 0) {
            emit_prompt(s, 2);
            return;
        }
        if (!s->no_resetting_text) {
            emit(s, "\r\nresetting the cpu...\r\n", 2);
        }
        set_state(s, SIM_APP_BOOTING);
    } else {
        emit(s, "\r\nInvalid input\r\n", 2);
        emit_prompt(s, 0);
    }
}

static size_t frame_len(const sim_t *s)
{
    return 3u + 1024u + (s->mode == 'C' ? 2u : 1u);
}

static void post_report(sim_t *s)
{
    sim_post_t p = s->post;
    if (p == SIM_POST_ERROR_THEN_OK) {
        p = s->transfers <= 1 ? SIM_POST_ERROR : SIM_POST_SUCCESS;
    }
    set_state(s, SIM_COMMIT);
    switch (p) {
    case SIM_POST_SUCCESS:
        emit(s, "\r\nupdating...\r\n", 100);
        if (s->post_recover_warn) {
            emit(s, "recover warinnig: backup area\r\n", 50);
        }
        emit(s, "backup succeed\r\n", s->post_success_ms);
        emit_prompt(s, 50);
        s->flashed = true;
        s->st = SIM_MENU;
        break;
    case SIM_POST_LATE:
        emit(s, "backup succeed\r\n", T1_LATE_MS);
        emit_prompt(s, 50);
        s->flashed = true;
        s->st = SIM_MENU;
        break;
    case SIM_POST_ERROR:
        emit(s, "\r\n", 100);
        emit(s, s->post_error != NULL ? s->post_error : "wrong pkg header", 0);
        emit(s, "\r\n", 0);
        emit_prompt(s, 20);
        s->transfer_failed = true;
        s->st = SIM_MENU;
        break;
    case SIM_POST_PROMPT_ONLY:
        emit_prompt(s, 2000);
        s->st = SIM_MENU;
        break;
    case SIM_POST_TEXT_ONLY:
        emit(s, "\r\nsomething else\r\n", 2000);
        break;
    default:
        /* Silent; the application may start on its own later. */
        if (s->post_app_after_ms > 0) {
            s->app_boot_ms = s->post_app_after_ms;
            s->flashed = true;
            set_state(s, SIM_APP_BOOTING);
        }
        break;
    }
}

static void xrx_eot(sim_t *s)
{
    s->rx_eots++;
    s->eot_count++;
    if (s->eot_silent) {
        post_report(s);
        return;
    }
    if (s->eot_count <= s->eot_naks) {
        emit_byte(s, FL_NAK, 2);
        return;
    }
    if (s->eot_text) {
        emit(s, "writing\r\n", 2);
    } else {
        emit_byte(s, FL_ACK, 2);
    }
    post_report(s);
}

static void ack(sim_t *s, uint32_t delay_ms)
{
    if (s->junk_before_ack) {
        static const uint8_t j[2] = { 0x00, 0xFF };
        emit_delay(s, j, 2, delay_ms);
        delay_ms = 0;
    }
    if (s->lone_can_before_ack) {
        emit_byte(s, FL_CAN, delay_ms);
        delay_ms = 0;
    }
    emit_byte(s, FL_ACK, delay_ms);
    s->last_ack_us = s->out_tail_us;
}

static void xrx_frame(sim_t *s)
{
    s->rx_frames++;
    if (!s->started) {
        s->started = true;
        s->transfers++;
    }
    const uint8_t *f = s->fbuf;
    uint8_t blk = f[1];
    bool hdr_ok = (uint8_t)(blk + f[2]) == 0xFF;
    bool sum_ok;
    if (s->mode == 'C') {
        uint16_t crc = fl_crc16_xmodem(f + 3, 1024);
        sum_ok = f[1027] == (crc >> 8) && f[1028] == (crc & 0xFF);
    } else {
        sum_ok = f[1027] == fl_sum8(f + 3, 1024);
    }
    int b = (int)(s->img_len / 1024u) + 1;   /* 1-based block being answered */
    bool first_transfer = s->transfers <= 1;
    if (s->silent || (s->silent_block == b && (first_transfer || s->silent_always))) {
        if (!s->silent) {
            s->silent = true;
            s->silent_since_us = s->now_us;
            s->junk_next_s = 1;
        }
        return;
    }
    if (s->can_block == b && first_transfer) {
        static const uint8_t c2[2] = { FL_CAN, FL_CAN };
        emit_delay(s, c2, 2, 5);
        emit(s, "\r\n## Binary download aborted\r\n", 100);
        emit_prompt(s, 0);
        s->transfer_failed = true;
        set_state(s, SIM_MENU);
        return;
    }
    if (s->mismatch_block == b && first_transfer) {
        /* The loader gave up and started the application at 115200. */
        s->transfer_failed = true;
        set_state(s, SIM_APP);
        s->com1_baud = 115200;
        s->quiet = false;
        s->next_periodic_us = s->now_us;
        return;
    }
    if (s->text_block == b && s->text != NULL && first_transfer && !s->text_done) {
        s->text_done = true;
        emit(s, s->text, 5);
        emit(s, "\r\n", 0);
        if (s->text_then_nak) {
            emit_byte(s, FL_NAK, 5);
        } else {
            s->transfer_failed = true;
            set_state(s, SIM_MENU);
            emit_prompt(s, 0);
        }
        return;
    }
    if (!hdr_ok || !sum_ok) {
        emit_byte(s, FL_NAK, 2);
        return;
    }
    if (blk == (uint8_t)(s->expect - 1u) && s->img_len > 0) {
        ack(s, 2);   /* duplicate of the last block */
        return;
    }
    if (blk != s->expect) {
        emit_byte(s, FL_NAK, 2);
        return;
    }
    if (s->nak_block == b && s->nak_done < s->nak_times) {
        s->nak_done++;
        emit_byte(s, FL_NAK, 2);
        return;
    }
    if (s->nak_every_done < s->nak_every) {
        s->nak_every_done++;
        emit_byte(s, FL_NAK, 2);
        return;
    }
    s->nak_every_done = 0;
    if (s->c_block == b && !s->c_done) {
        s->c_done = true;
        emit_byte(s, 'C', 2);
        return;
    }
    if (s->img_len + 1024u <= SIM_IMG_MAX) {
        memcpy(s->img + s->img_len, f + 3, 1024);
        s->img_len += 1024u;
    }
    s->expect++;
    uint32_t d = (s->pause_block == b) ? s->pause_ms : 2u;
    ack(s, d);
    if (s->dup_ack_block == b && !s->dup_done) {
        s->dup_done = true;
        emit_byte(s, FL_ACK, 0);
    }
}

static void xrx_byte(sim_t *s, uint8_t c)
{
    if (c != FL_CAN || s->flen != 0) {
        s->can_pending = 0;
    }
    if (s->flen == 0) {
        if (c == FL_STX) {
            s->fbuf[0] = c;
            s->flen = 1;
            if (!s->started) {
                s->mode = (s->nak_after_ms > 0 && s->now_us - s->ready_us >= US(s->nak_after_ms)) ? FL_NAK : s->start;
            }
        } else if (c == FL_EOT && s->started) {
            xrx_eot(s);
        } else if (c == FL_CAN) {
            s->rx_cans++;
            if (++s->can_pending >= 2) {
                s->can_pending = 0;
                if (s->started) {
                    s->transfer_failed = true;
                }
                emit(s, "\r\n## Binary download aborted\r\n", 50);
                emit_prompt(s, 0);
                set_state(s, SIM_MENU);
            }
            return;
        } else if (c == FL_ACK) {
            s->rx_acks++;
        }
        return;
    }
    s->fbuf[s->flen++] = c;
    if (s->flen == frame_len(s)) {
        xrx_frame(s);
        s->flen = 0;
    }
}

static bool stop_key_baud_ok(const sim_t *s)
{
    if (s->loader_only_baud != 0) {
        return s->host_baud == s->loader_only_baud;
    }
    if (s->host_baud == 460800u) {
        return true;
    }
    return s->loader_any_baud && (s->host_baud == 115200u || s->host_baud == 921600u);
}

/* Bytes from the host, at the end of their write. */
static void rx_bytes(sim_t *s, const uint8_t *d, size_t n)
{
    if (s->st == SIM_RESETTING) {
        uint32_t keys = 0;
        for (size_t i = 0; i < n; i++) {
            keys += (d[i] == 'T' || d[i] == '@');
        }
        if (keys > 0) {
            s->rx_stop_key_bytes += keys;
            s->rx_bursts++;
            s->rx_last_burst = keys;
            if (keys > s->rx_max_burst) {
                s->rx_max_burst = keys;
            }
            /* This write started when the previous burst had just left: no idle gap. */
            if (s->rx_bursts > 1 && s->now_us - wire_us(n, s->host_baud) - s->last_burst_end_us < 2000u) {
                s->rx_bursts_merged++;
            }
            s->last_burst_end_us = s->now_us;
        }
        if (keys > 0 && s->loader_reachable && stop_key_baud_ok(s) &&
            s->now_us - s->t_state_us <= US(s->boot_window_ms)) {
            s->loader_baud = s->host_baud;
            set_state(s, SIM_MENU);
            s->line_len = 0;
            loader_banner(s);
        }
        return;
    }
    if (s->st == SIM_MENU || s->st == SIM_SUBMENU || s->st == SIM_XRX || s->st == SIM_COMMIT) {
        if (s->host_baud != s->loader_baud) {
            return;   /* garbage to the loader */
        }
    } else if (s->st == SIM_APP) {
        if (s->host_baud != s->com1_baud) {
            return;
        }
    } else {
        return;   /* booting or gone: nothing is parsed */
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t c = d[i];
        switch (s->st) {
        case SIM_XRX:
            xrx_byte(s, c);
            break;
        case SIM_APP:
        case SIM_MENU:
        case SIM_SUBMENU:
            if (s->st == SIM_MENU && (c == 'T' || c == '@')) {
                s->rx_stop_key_bytes++;
                break;   /* stop key left over */
            }
            if (c == FL_CAN || c == FL_ACK || c == 0x04) {
                if (c == FL_CAN) {
                    s->rx_cans++;
                } else if (c == FL_ACK) {
                    s->rx_acks++;
                }
                break;
            }
            if (c == '\r' || c == '\n') {
                s->line[s->line_len] = '\0';
                /* An empty line reprints the prompt, unless the loader skips a
                 * leading CR LF that more input follows in the same write. */
                bool empty_prompt = c == '\r' && s->st == SIM_MENU && (!s->skip_leading_crlf || i + 2 >= n);
                if (s->line_len > 0 || empty_prompt) {
                    char line[256];
                    memcpy(line, s->line, s->line_len + 1);
                    s->line_len = 0;
                    if (s->st == SIM_APP) {
                        app_line(s, line);
                    } else if (s->st == SIM_SUBMENU) {
                        if (strcmp(line, "q") == 0) {
                            s->rx_q++;
                            set_state(s, SIM_MENU);
                            emit_prompt(s, 2);
                        }
                    } else {
                        menu_line(s, line);
                    }
                }
                if (s->st != SIM_APP && s->st != SIM_MENU && s->st != SIM_SUBMENU) {
                    /* The rest of this write reaches the new state. */
                    rx_bytes(s, d + i + 1, n - i - 1);
                    return;
                }
                break;
            }
            if (s->line_len < sizeof(s->line) - 1) {
                s->line[s->line_len++] = (char)c;
            } else {
                s->line_len = 0;
            }
            break;
        default:
            break;
        }
    }
}

/* When the next timed behaviour is due (UINT64_MAX: none). */
static uint64_t next_timed(sim_t *s)
{
    uint64_t next = UINT64_MAX;
    if ((s->st == SIM_APP || s->st == SIM_GONE) && !s->quiet && s->app_period_ms > 0) {
        next = s->next_periodic_us;
    }
    if (s->st == SIM_RESETTING) {
        uint64_t w = s->t_state_us + US(s->late_text_after_reset ? 9000u : s->boot_window_ms);
        next = next < w ? next : w;
    }
    if (s->st == SIM_APP_BOOTING) {
        uint64_t w = s->t_state_us + US(s->app_boot_ms);
        next = next < w ? next : w;
    }
    if (s->st == SIM_XRX && !s->started) {
        /* The test may set the delay after the simulation was put in XRX. */
        uint64_t first = s->ready_us + US(s->start_delay_ms);
        if (s->next_start_us < first) {
            s->next_start_us = first;
        }
        next = next < s->next_start_us ? next : s->next_start_us;
    }
    if (s->st == SIM_XRX && s->silent && s->junk_every_s > 0) {
        uint64_t w = s->silent_since_us + US(1000u * s->junk_next_s);
        next = next < w ? next : w;
    }
    return next;
}

/* Timed behaviour up to `until`. */
static void advance(sim_t *s, uint64_t until)
{
    for (;;) {
        uint64_t next = next_timed(s);
        if (next > until) {
            s->now_us = until;
            return;
        }
        s->now_us = next > s->now_us ? next : s->now_us;
        switch (s->st) {
        case SIM_APP:
        case SIM_GONE:
            emit_gga(s, 0);
            s->next_periodic_us += US(s->app_period_ms);
            break;
        case SIM_RESETTING:
            if (s->late_text_after_reset && s->now_us - s->t_state_us < US(9001u)) {
                emit(s, "\r\nloading something\r\n", 0);
                s->late_text_after_reset = false;   /* once; then the application */
                s->t_state_us = s->now_us - US(s->boot_window_ms);
                break;
            }
            if (s->reset_silent) {
                s->st = SIM_GONE;
                s->quiet = true;
                break;
            }
            set_state(s, SIM_APP_BOOTING);
            break;
        case SIM_APP_BOOTING:
            app_start(s);
            break;
        case SIM_XRX:
            if (!s->started) {
                uint8_t b = s->start;
                if (s->nak_after_ms > 0 && s->now_us - s->ready_us >= US(s->nak_after_ms)) {
                    b = FL_NAK;
                }
                if (s->start_junk) {
                    static const uint8_t j[3] = { 0x00, 0x91, 0xFF };
                    emit_delay(s, j, 3, 0);
                }
                emit_byte(s, b, 0);
                s->next_start_us += US(s->start_repeat_ms > 0 ? s->start_repeat_ms : 1000u);
            } else {
                emit_byte(s, 0x91, 0);   /* junk outside the mismatch pattern */
                s->junk_next_s++;
            }
            break;
        default:
            break;
        }
    }
}

/* ---- io ---------------------------------------------------------------------------- */

static int io_write(void *ctx, const uint8_t *data, size_t len)
{
    sim_t *s = ctx;
    advance(s, s->now_us);
    advance(s, s->now_us + wire_us(len, s->host_baud));
    rx_bytes(s, data, len);
    return 0;
}

static uint8_t deliver(sim_t *s, size_t k)
{
    if (s_tx_baud[k] == s->host_baud) {
        return s->out_b[k];
    }
    static const uint8_t g[4] = { 0x00, 0xF8, 0x80, 0x78 };
    s->host_line_errors += (s_garbage_i & 1u);
    return g[s_garbage_i++ % 4u];
}

static size_t io_read(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    sim_t *s = ctx;
    s->reads++;
    uint64_t target = s->now_us + US(timeout_ms);
    for (;;) {
        advance(s, s->now_us);
        size_t n = 0;
        while (n < cap && s->out_len > 0 && s->out_due[s->out_head] <= s->now_us) {
            buf[n++] = deliver(s, s->out_head);
            s->out_head = (s->out_head + 1u) % SIM_OUT_MAX;
            s->out_len--;
        }
        if (n > 0) {
            return n;
        }
        uint64_t next = s->out_len > 0 ? s->out_due[s->out_head] : UINT64_MAX;
        uint64_t ev = next_timed(s);
        if (ev < next) {
            next = ev;
        }
        if (next > target) {
            advance(s, target);
            return 0;
        }
        advance(s, next);
    }
}

static void io_flush(void *ctx)
{
    sim_t *s = ctx;
    advance(s, s->now_us);
    while (s->out_len > 0 && s->out_due[s->out_head] <= s->now_us) {
        s->out_head = (s->out_head + 1u) % SIM_OUT_MAX;
        s->out_len--;
    }
}

static int io_set_baud(void *ctx, uint32_t baud)
{
    sim_t *s = ctx;
    s->host_baud = baud;
    /* Like the link: the first 20 ms after a switch are discarded. */
    advance(s, s->now_us + US(20));
    io_flush(ctx);
    return 0;
}

static uint32_t io_now(void *ctx)
{
    return sim_now_ms((sim_t *)ctx);
}

static void io_sleep(void *ctx, uint32_t ms)
{
    sim_t *s = ctx;
    advance(s, s->now_us + US(ms));
}

static uint32_t io_line_errors(void *ctx)
{
    return ((sim_t *)ctx)->host_line_errors;
}

void sim_io(sim_t *s, fl_io_t *io)
{
    memset(io, 0, sizeof(*io));
    io->ctx = s;
    io->write = io_write;
    io->read = io_read;
    io->flush = io_flush;
    io->set_baud = io_set_baud;
    io->now_ms = io_now;
    io->sleep_ms = io_sleep;
    io->line_errors = io_line_errors;
}

void sim_init(sim_t *s)
{
    uint8_t *img = s->img;
    memset(s, 0, sizeof(*s));
    s->img = img != NULL ? img : malloc(SIM_IMG_MAX);
    s->base_ms = UINT32_MAX - 30000u;   /* the clock wraps 30 s into every test */
    s->app_baud = 115200;
    s->com1_baud = 115200;
    s->host_baud = 115200;
    s->loader_baud = 460800;
    s->loader_reachable = true;
    s->boot_window_ms = 400;
    s->app_boot_ms = 1500;
    s->banner = true;
    s->app_period_ms = 1000;
    strcpy(s->model, "UM982");
    s->build_before = 18975;
    s->build_after = 19152;
    s->start = 'C';
    s->start_delay_ms = 50;
    s->start_repeat_ms = 1000;
    s->post_success_ms = 15000;
    s->next_periodic_us = US(500);
    s_garbage_i = 0;
}

void sim_free(sim_t *s)
{
    free(s->img);
    s->img = NULL;
}

void sim_at_prompt(sim_t *s, uint32_t baud)
{
    s->loader_baud = baud;
    s->host_baud = baud;
    s->st = SIM_MENU;
    s->quiet = true;
}

void sim_at_xrx(sim_t *s, uint32_t baud)
{
    sim_at_prompt(s, baud);
    start_xrx(s);
}
