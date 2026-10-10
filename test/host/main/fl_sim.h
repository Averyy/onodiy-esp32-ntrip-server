/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Byte-level simulation of the receiver for the flasher's host tests: its
 * application on COM1 (version log, baud change, sentence request, quiesce,
 * reset), the resident loader reached with the stop key after a soft reset
 * (banner, menu, prompt), its XMODEM-1K receive side and its report after
 * the transfer. Behaviours are scripted per test (start byte, delays,
 * pauses, garbage, abort text, cancel, silence, another baud, ...).
 *
 * Time is virtual: writes take their wire time, reads wait until the next
 * byte is due or their timeout passes. The clock starts close to the 32-bit
 * wrap so every core is exercised across it. Bytes sent at another baud than
 * the listener's arrive as the mismatch pattern (and nothing reaches the
 * other side's parser).
 *
 * The behaviours are synthetic: they follow what the flasher expects of the
 * receiver (PROVISIONAL until the bench confirms it), not captures of a real
 * receiver.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fl_io.h"

#define SIM_OUT_MAX   65536
#define SIM_IMG_MAX   (4u * 1024u * 1024u)

typedef enum {
    SIM_APP = 0,
    SIM_RESETTING,      /* soft reset: the loader looks for the stop key */
    SIM_APP_BOOTING,    /* the application starts (banner after app_boot_ms) */
    SIM_MENU,
    SIM_SUBMENU,
    SIM_XRX,            /* XMODEM receive */
    SIM_COMMIT,         /* writing after EOT */
    SIM_GONE,           /* left the loader at another baud (mismatch test) */
} sim_state_t;

typedef enum {
    SIM_M2_READY = 0,
    SIM_M2_SUBMENU,
    SIM_M2_INVALID_ONCE,
    SIM_M2_INVALID_ALWAYS,
    SIM_M2_INPUT_TIMEOUT,
    SIM_M2_ABORTED,
    SIM_M2_PROMPT_ONLY,
    SIM_M2_SILENT,
    SIM_M2_SILENT_FIRST,   /* no answer to the first item, ready on the second */
} sim_menu2_t;

typedef enum {
    SIM_POST_SUCCESS = 0,
    SIM_POST_ERROR,
    SIM_POST_PROMPT_ONLY,
    SIM_POST_SILENT,
    SIM_POST_LATE,
    SIM_POST_TEXT_ONLY,
    SIM_POST_ERROR_THEN_OK,   /* first transfer reports post_error, later ones succeed */
} sim_post_t;

typedef struct {
    /* ---- configuration ---- */
    uint32_t app_baud;          /* saved COM1 baud */
    bool loader_reachable;      /* the soft reset reaches the stop-key window */
    bool loader_any_baud;       /* follows the stop key at 115200/460800/921600 (else 460800 only) */
    uint32_t boot_window_ms;    /* stop key accepted this long after the reset */
    uint32_t app_boot_ms;       /* application banner after a restart */
    bool banner;                /* "$devicename" at application start */
    uint32_t app_period_ms;     /* periodic sentence while not quiesced (0 = none) */
    bool ignore_baud_change;    /* "config com1" answers but does not change */
    bool mute_after_config;     /* after the first "config com1" the application answers nothing */
    uint32_t loader_only_baud;  /* the stop key works only at this baud (0 = the usual rule) */
    bool split_prompt;          /* the prompt arrives in two reads */
    bool late_text_after_reset; /* garbage text 9 s after the reset, no loader */
    bool reset_silent;          /* nothing at all after the reset (no loader, no app) */
    char model[16];
    uint32_t build_before;
    uint32_t build_after;
    /* menu */
    sim_menu2_t menu2;
    bool skip_leading_crlf;     /* "\r\n" before an item prints no prompt of its own */
    bool begin_text;
    /* XMODEM */
    uint8_t start;              /* 'C' or 0x15 */
    uint32_t start_delay_ms;
    uint32_t start_repeat_ms;
    uint32_t nak_after_ms;      /* switch to NAK start bytes after this (0 = never) */
    bool start_junk;            /* junk and a "Choose" line around the start bytes */
    bool c_text_before_start;   /* a line starting with 'C' before the start byte */
    int nak_block;              /* NAK this block nak_times times */
    int nak_times;
    int nak_every;              /* NAK every block this many times */
    int nak_every_done;
    int c_block;                /* answer 'C' to this block once */
    int pause_block;            /* acknowledge this block after pause_ms */
    uint32_t pause_ms;
    int text_block;             /* print text before answering this block */
    const char *text;
    bool text_then_nak;         /* ... then NAK (else stop answering) */
    int can_block;              /* CAN CAN at this block (first transfer only) */
    int silent_block;           /* stop answering at this block (first transfer only) */
    bool silent_always;         /* ... in every transfer */
    int mismatch_block;         /* leave the loader at this block (115200 output) */
    bool junk_before_ack;
    bool lone_can_before_ack;
    int dup_ack_block;          /* acknowledge this block twice */
    int junk_every_s;           /* with silent_block: junk every second (no ACK) */
    /* EOT */
    int eot_naks;
    bool eot_silent;
    bool eot_text;
    /* after EOT */
    sim_post_t post;
    uint32_t post_success_ms;
    const char *post_error;
    bool post_recover_warn;
    uint32_t post_app_after_ms; /* SIM_POST_SILENT: the application starts this long after EOT (0 = never) */
    /* exit */
    bool no_resetting_text;
    bool app_dead_after;        /* the application does not start after the flash */
    bool exit_needs_second_6;   /* the first "6" after the transfer only reprints the prompt */

    /* ---- state ---- */
    sim_state_t st;
    uint64_t now_us;
    uint32_t base_ms;
    uint32_t host_baud;
    uint32_t com1_baud;         /* application's COM1 now (unsaved changes included) */
    uint32_t loader_baud;
    bool quiet;                 /* quiesced */
    uint64_t t_state_us;
    uint64_t next_periodic_us;
    uint64_t next_start_us;
    bool started;               /* the handshake's first frame arrived */
    uint8_t mode;               /* 'C' or NAK of the transfer */
    uint64_t ready_us;
    uint8_t expect;             /* next block number */
    uint8_t fbuf[1100];
    size_t flen;
    uint8_t *img;
    size_t img_len;
    uint32_t transfers;         /* transfers begun (first frame received) */
    bool flashed;               /* a transfer ended in success */
    int invalid_seen;
    int m2_count;
    char line[256];
    size_t line_len;
    int nak_done;
    bool c_done;
    bool text_done;
    bool dup_done;
    bool silent;
    uint64_t silent_since_us;
    uint32_t junk_next_s;
    int eot_count;
    int can_pending;
    uint64_t last_ack_us;
    int six_count;

    /* output queue */
    uint64_t out_due[SIM_OUT_MAX];
    uint8_t out_b[SIM_OUT_MAX];
    uint8_t out_baud_ok[SIM_OUT_MAX];   /* sent at the host's baud */
    size_t out_head;
    size_t out_len;
    uint64_t out_tail_us;               /* when the last queued byte leaves */

    /* ---- what the receiver got (for assertions) ---- */
    uint32_t rx_frames;         /* complete frames */
    uint32_t rx_frames_after_text;
    uint32_t rx_eots;
    uint32_t rx_cans;
    uint32_t rx_acks;
    uint32_t rx_versiona;
    uint32_t rx_resets;
    uint32_t rx_stop_key_bytes;
    uint32_t rx_bursts;
    uint32_t rx_max_burst;
    uint32_t rx_last_burst;
    uint32_t rx_bursts_merged;  /* stop-key bytes that arrived without a gap after the previous burst */
    uint64_t last_burst_end_us;
    uint32_t rx_menu[16];       /* menu items received at the prompt */
    uint32_t rx_six;            /* "6" at the prompt */
    uint32_t rx_six_after_ponr; /* "6" after a transfer failed */
    uint32_t rx_q;
    uint32_t rx_config_com1;
    uint32_t rx_frames_after_last_ack_eot; /* frames received after EOT started */
    uint32_t rx_can_after_eot;
    bool transfer_failed;       /* a transfer ended without success */
    uint32_t host_line_errors;
    uint32_t reads;
} sim_t;

void sim_init(sim_t *s);
void sim_free(sim_t *s);
/** The io wired to this simulation. */
void sim_io(sim_t *s, fl_io_t *io);
uint32_t sim_now_ms(sim_t *s);
/** Put the receiver at its loader prompt at the loader baud (host baud too). */
void sim_at_prompt(sim_t *s, uint32_t baud);
/** Put the receiver in XMODEM receive (ready line already printed). */
void sim_at_xrx(sim_t *s, uint32_t baud);
/** CRC32 of the receiver's '#' logs (reference implementation for the vectors). */
uint32_t sim_crc32(const char *s, size_t n);
