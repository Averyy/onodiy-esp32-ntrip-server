/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Health monitor helpers (pure logic, no IDF or RTOS includes, injected
 * millisecond clock, host-tested). health.c samples the components once a
 * second and asks these what to do:
 *
 *  - Loss journal: the loss counters are monotonic and live in RAM in the
 *    components that increment them; health journals their increases, at
 *    most one record per tick, for the counter whose unjournalled increase
 *    is oldest, and at most one record per counter every
 *    HM_LOSS_MIN_INTERVAL_MS: a counter that rises for days (a garbled
 *    line) must not take the journal's shared hourly budget from every
 *    other kind of record, nor overwrite the journal's history in hours. A
 *    record that waits, or that the journal refuses (its rate limits), stays
 *    pending and keeps accumulating, so increases are merged, never lost.
 *  - Link-loss alert: raised when, within the last 10-11 minutes, the UART
 *    hardware FIFO overflowed at least once, or RTCM CRC errors exceeded
 *    1 % of the valid frames with at least 100 frames. Ten completed
 *    one-minute buckets plus the current one, RAM only. Increases while the
 *    GNSS ladder is suspended (configuring, flashing, raw sessions) are not
 *    counted; the first tick after a suspension takes a new baseline. The
 *    alert clears after 30 min without either condition.
 *  - Caster budgets: caster tasks are not on the task watchdog; a slot that
 *    stays CONNECTING longer than its budget, makes no send progress while
 *    streaming for longer than the send timeout plus 5 s, or stamps no
 *    progress at all for longer than the connect budget is restarted (that
 *    slot only), at most once per connect budget.
 *  - Debounce for alerts that must persist before they show and clear.
 *  - BOOT button: armed only after it was seen released (a button held or
 *    stuck at boot does nothing); the action is chosen by how long it was
 *    held and taken on release, so holding on past the first threshold
 *    never runs the first action as well.
 *  - Overall level: the worst of the GNSS ladder's and "degraded" for any
 *    other alert or a caster that is down.
 *
 * Thresholds are PROVISIONAL, to be confirmed on hardware.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HM_NEVER INT64_MIN

/* ---- loss journal ---------------------------------------------------------- */

/** Loss counters journalled, ids 1..HM_LOSS_IDS (same values as diag_loss_id_t). */
#define HM_LOSS_IDS 8

/** Shortest time between two records of the same counter. */
#define HM_LOSS_MIN_INTERVAL_MS 60000

typedef struct {
    bool have_base;
    uint64_t last[HM_LOSS_IDS];      /**< last sampled value (index = id - 1) */
    uint64_t pending[HM_LOSS_IDS];   /**< increase not journalled yet */
    int64_t since_ms[HM_LOSS_IDS];   /**< when the oldest unjournalled increase was seen */
    int64_t written_ms[HM_LOSS_IDS]; /**< when the counter's last record was journalled (HM_NEVER = not yet) */
} hm_loss_t;

void hm_loss_init(hm_loss_t *l);

/**
 * Sample the counters (vals[i] = counter id i + 1). The first call only takes
 * the baseline. A counter that went backwards is rebaselined.
 * @param inc optional: this tick's increase per counter.
 */
void hm_loss_sample(hm_loss_t *l, const uint64_t vals[HM_LOSS_IDS], int64_t now_ms, uint64_t inc[HM_LOSS_IDS]);

/**
 * The record to try at now_ms: among the counters whose last record is at
 * least HM_LOSS_MIN_INTERVAL_MS old, the one whose pending increase is
 * oldest (clamped to INT32_MAX).
 */
bool hm_loss_next(const hm_loss_t *l, int64_t now_ms, uint8_t *id, int32_t *delta);

/** The journal took that record at now_ms: clear what it carried (a clamped remainder stays pending). */
void hm_loss_written(hm_loss_t *l, uint8_t id, int32_t delta, int64_t now_ms);

/* ---- link-loss alert ----------------------------------------------------------- */

#define HM_LINK_BUCKETS      10
#define HM_LINK_BUCKET_MS    60000
#define HM_LINK_CLEAR_MS     1800000
#define HM_LINK_MIN_FRAMES   100u
#define HM_LINK_CRC_PCT      1u

typedef struct {
    bool have_base;
    uint64_t base_fifo, base_crc, base_frames;
    uint32_t fifo[HM_LINK_BUCKETS], crc[HM_LINK_BUCKETS], frames[HM_LINK_BUCKETS];
    uint8_t count;                   /**< completed buckets held */
    uint8_t head;                    /**< slot the next completed bucket goes to */
    uint32_t cur_fifo, cur_crc, cur_frames;
    int64_t bucket_start;
    bool alert;
    int64_t last_cond_ms;            /**< last tick either condition held */
} hm_link_t;

typedef struct {
    uint64_t fifo, crc, frames;      /**< window sums */
} hm_link_window_t;

void hm_link_init(hm_link_t *w, int64_t now_ms);

/**
 * One tick with the monotonic counters (FIFO overflows, RTCM CRC errors,
 * valid RTCM frames). While `suspended` nothing is added and the next
 * unsuspended tick rebaselines. @return the alert state.
 */
bool hm_link_tick(hm_link_t *w, uint64_t fifo, uint64_t crc, uint64_t frames, bool suspended, int64_t now_ms);

/** Current window sums (completed buckets plus the current one). */
void hm_link_window(const hm_link_t *w, hm_link_window_t *out);

/* ---- caster budgets --------------------------------------------------------------- */

/** Same values as ntrip_state_t. */
typedef enum {
    HM_CST_DISABLED = 0,
    HM_CST_WAIT_NETWORK,
    HM_CST_WAIT_DATA,
    HM_CST_CONNECTING,
    HM_CST_STREAMING,
    HM_CST_BACKOFF,
    HM_CST_AUTH_FAILED,
    HM_CST_PAUSED,
    HM_CST_CONFIG_ERROR,
    HM_CST_BANNED,
} hm_cst_state_t;

#define HM_SEND_SLACK_MS 5000u

typedef struct {
    int64_t restart_ms;              /**< last restart (HM_NEVER = none) */
} hm_slot_t;

typedef struct {
    bool enabled;
    uint8_t state;                   /**< hm_cst_state_t */
    uint32_t state_age_s;
    uint32_t progress_age_ms;
    uint32_t connect_budget_s;
    uint32_t send_timeout_ms;
} hm_slot_in_t;

void hm_slot_init(hm_slot_t *s);

/** True: restart this slot now (and the restart is recorded). */
bool hm_slot_overrun(hm_slot_t *s, const hm_slot_in_t *in, int64_t now_ms);

/** A slot that is enabled and failing (waiting for the network, backing off, rejected or banned). */
bool hm_slot_down(const hm_slot_in_t *in);

/* ---- low-heap reboot ------------------------------------------------------------ */

/*
 * The internal heap's low-water mark below a limit reboots the ESP32 once.
 * Two cases where a reboot cannot help are left out, so they never loop:
 *  - a dip already in the mark at the first sample: it happened while the
 *    firmware started (the monitor starts last) and would happen again on
 *    every boot; only a mark that falls further later counts;
 *  - the previous boot ended in a low-heap reboot within
 *    HM_HEAP_REBOOT_SPACING_S of its start: this boot only warns.
 * A low-heap reboot is a planned one, so the boot-loop guard never sees it;
 * these rules are what bounds it.
 */
#define HM_HEAP_REBOOT_SPACING_S 3600u

typedef struct {
    bool have_base;
    uint32_t base_min;     /**< low-water mark at the first sample */
    bool held;             /**< no low-heap reboot this boot (the previous one was a quick one) */
    bool done;             /**< requested already */
} hm_heap_t;

/**
 * prev_low_heap: the previous boot ended in a low-heap reboot;
 * prev_uptime_s: how long it ran (UINT32_MAX = unknown).
 */
void hm_heap_init(hm_heap_t *h, bool prev_low_heap, uint32_t prev_uptime_s);

/** One sample of the low-water mark. @return true once: request the reboot now. */
bool hm_heap_reboot_due(hm_heap_t *h, uint32_t low_water, uint32_t limit);

/* ---- debounce ----------------------------------------------------------------- */

typedef struct {
    bool raw;
    bool out;
    int64_t changed_ms;
} hm_deb_t;

void hm_deb_init(hm_deb_t *d);

/** Output turns on after `raw` held for on_ms, off after it was clear for off_ms. */
bool hm_deb(hm_deb_t *d, bool raw, int64_t now_ms, uint32_t on_ms, uint32_t off_ms);

/* ---- BOOT button ------------------------------------------------------------- */

typedef enum {
    HM_BTN_NONE = 0,
    HM_BTN_ADMIN_REACHED,            /**< still held: releasing now clears the admin password */
    HM_BTN_FACTORY_REACHED,          /**< still held: releasing now runs a factory reset */
    HM_BTN_ADMIN,                    /**< released after the admin hold */
    HM_BTN_FACTORY,                  /**< released after the factory hold */
} hm_btn_event_t;

typedef struct {
    bool armed;                      /**< seen released at least once */
    bool down;
    int64_t down_ms;
    uint8_t reached;                 /**< 0, 1 admin, 2 factory */
} hm_button_t;

void hm_button_init(hm_button_t *b);

/** One sample of the button (pressed = active). */
hm_btn_event_t hm_button(hm_button_t *b, bool pressed, int64_t now_ms, uint32_t admin_ms, uint32_t factory_ms);

/* ---- overall level ------------------------------------------------------------ */

/** Worst of the ladder's level and DEGRADED when `degraded` (same values as health_level_t). */
uint8_t hm_level(uint8_t ladder_level, bool degraded);

#ifdef __cplusplus
}
#endif
