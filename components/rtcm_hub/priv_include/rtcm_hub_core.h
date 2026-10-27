/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * RTCM hub core: frame ring, epoch tagging, sticky station messages and
 * per-consumer cursors (pure logic, no IDF or RTOS includes, host-testable).
 * rtcm_hub.c wraps it with a spinlock, PSRAM buffers, a clock and task
 * notifications; the policy itself is described in rtcm_hub.h.
 *
 * Ring: records {u16 len, u16 msg_type, u32 seq, u32 rx_ms, u32 epoch} +
 * frame bytes, padded to 4. msg_type holds the 12-bit message number
 * (RTCM_CORE_REC_TYPE_MASK), RTCM_CORE_REC_UNSURE when the record belongs
 * to an unsure epoch (rtcm_epoch.h), and RTCM_CORE_REC_EXACT for a UART
 * interrupt arrival stamp; readers mask the number with
 * RTCM_CORE_REC_TYPE_MASK. A record never wraps: when it does not fit at the
 * end, a wrap marker (len 0xFFFF) fills the rest and the record starts at
 * offset 0. Offsets are monotonic 64-bit byte counts; ring position = offset
 * modulo capacity.
 *
 * Concurrency (one producer, consumers on other tasks or cores). All shared
 * fields change under the caller's lock (rtcm_core_lock_t), held only for a
 * few field updates, never for a bulk copy:
 *  - publish: under the lock assign the epoch and publish reserve_off =
 *    write_off + record length (+ wrap waste); copy the record unlocked; under
 *    the lock publish write_off, the counters and a new sticky copy.
 *  - read: a byte at offset x is intact while reserve_off - x <= capacity. A
 *    consumer checks that for its cursor before trusting a header and again
 *    after copying the frame; a failed check is a lap. Header fields are
 *    range-checked (len, record inside the ring, seq) before use, so torn
 *    bytes can never cause an out-of-bounds access.
 *  - sticky copies are double-buffered: the producer fills the inactive copy
 *    unlocked and flips it under the lock with a generation bump; a consumer
 *    copies the active one and keeps it only if the generation did not move.
 *  - wake-ups: publish hands back the consumers that registered a waiter
 *    (rtcm_core_prepare_wait) and marks a wake pass in progress until
 *    rtcm_core_wake_done(); detach reports whether a pass that may still hold
 *    the waiter is running, so the caller can wait for it to end before the
 *    waiting task may go away.
 *  - the hold (rtcm_core_hold): an offset in the ring, chosen under the lock
 *    together with the epoch state and reserve_off, that no consumer reads
 *    past until rtcm_core_release(). The release bumps a generation; a
 *    consumer whose position predates it starts over at its next read.
 *  - finishing (rtcm_core_finish): one consumer stops at the end of the
 *    epoch it is delivering, decided by its own task from the records it
 *    reads and, at the write head, from the epoch state under the lock.
 *
 * Each consumer is driven by one task (read, resync, prepare/cancel wait,
 * set_max_lag); stats and detach may be called from any task.
 *
 * Time: `now_ms` is a monotonic 64-bit millisecond clock. A frame is stored
 * with its arrival on that clock (the UART interrupt's time of its last
 * byte, else its read time): records keep its low 32 bits (rx_ms), and ages,
 * consumer lag, the newest frame's age and epoch_in_flight are all measured
 * from it as (uint32_t)(now - rx_ms). If the newest frame is older than
 * 2^31 ms every record counts as stale.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gnss_framing.h"
#include "rtcm_epoch.h"
#include "rtcm_hub_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RTCM_CORE_MAX_CONSUMERS   24u
#define RTCM_CORE_NAME_LEN        16u
#define RTCM_CORE_REC_HDR         16u
#define RTCM_CORE_MIN_FRAME       (GNSS_RTCM3_HEADER_LEN + GNSS_RTCM3_CRC_LEN)
#define RTCM_CORE_MAX_REC         (RTCM_CORE_REC_HDR + ((GNSS_RTCM3_MAX_FRAME + 3u) & ~3u))
#define RTCM_CORE_MIN_RING        (4u * RTCM_CORE_MAX_REC)
#define RTCM_CORE_WRAP_MARKER     0xFFFFu
#define RTCM_CORE_REC_TYPE_MASK   0x0FFFu  /**< message number bits of a record's msg_type */
#define RTCM_CORE_REC_UNSURE      0x8000u  /**< record flag: its epoch is unsure (rtcm_epoch.h) */
#define RTCM_CORE_REC_EXACT       0x4000u  /**< record flag: UART arrival stamp, not reader time */
#define RTCM_CORE_STICKY_SLOTS    3u       /**< 1005/1006, 1033, 1230, replayed in this order */
#define RTCM_CORE_STICKY_MAX_AGE_MS 30000u /**< an older sticky copy is never replayed */
#define RTCM_CORE_LAG_MIN_MS      100u
#define RTCM_CORE_LAG_MAX_MS      60000u

/** Lock callbacks (a spinlock on the target, no-ops or a test hook on the host). */
typedef struct {
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void *ctx;
} rtcm_core_lock_t;

typedef struct {
    uint32_t gap_ms;          /**< epoch gap rule */
    uint32_t default_lag_ms;  /**< lag limit of a new consumer */
    unsigned max_consumers;   /**< 1..RTCM_CORE_MAX_CONSUMERS */
    uint32_t first_epoch;     /**< sequence number of the first epoch (1; tests use others) */
} rtcm_core_cfg_t;

typedef struct {
    uint64_t frames_in;
    uint64_t bytes_in;
    uint64_t epochs_in;
    uint64_t epochs_by_mmb;
    uint64_t epochs_by_time;
    uint64_t epochs_by_gap;
    uint64_t publish_failures;
    uint32_t consumers;
    size_t capacity;
    uint32_t last_frame_age_ms;   /**< UINT32_MAX = never */
} rtcm_core_stats_t;

typedef struct {
    uint64_t frames_out;          /**< incl. sticky replays */
    uint64_t bytes_out;
    uint64_t frames_dropped;      /**< lost while streaming: lapped or over the lag limit */
    uint64_t bytes_dropped;
    uint64_t frames_skipped_idle; /**< skipped while not streaming (attach, resync, unsure epochs) */
    uint64_t epochs_dropped;
    uint64_t epochs_out;          /**< epochs delivered from their first frame to their last */
    uint64_t sticky_out;          /**< sticky copies replayed (part of frames_out) */
    uint32_t lag_drops;           /**< lag-limit events (one event may drop several epochs) */
    uint32_t laps;                /**< times overwritten while streaming */
} rtcm_core_counters_t;

typedef struct {
    rtcm_core_counters_t c;
    uint32_t max_lag_ms;
    uint32_t lag_ms;              /**< age of the oldest unread frame now (0 = none unread) */
} rtcm_core_consumer_stats_t;

/** One consumer. Fields are internal; see the file comment for ownership. */
typedef struct {
    /* changed under the lock */
    bool in_use;
    bool waiting;
    void *waiter;
    char name[RTCM_CORE_NAME_LEN];
    uint32_t max_lag_ms;
    rtcm_core_counters_t pub;     /**< counters as of the owner's last operation */
    uint64_t pub_rd;
    uint32_t pub_last_rx_ms;
    /* owner task only */
    uint64_t rd;                  /**< offset of the next unread record */
    uint64_t frames_pos;          /**< frames published before rd (seq of the next record) */
    uint64_t bytes_pos;           /**< frame bytes published before rd */
    uint32_t last_rx_ms;          /**< rx_ms of the last record passed */
    bool streaming;               /**< false: waiting for an epoch start */
    bool wait_any;                /**< waiting, and no epoch existed: take the first record */
    bool wait_loss;               /**< waiting after a lap: skipped frames are losses */
    uint32_t wait_after;          /**< waiting: deliver from the first epoch after this one */
    uint32_t last_epoch;          /**< streaming: epoch of the last delivered frame */
    bool epoch_open;              /**< last_epoch delivered from its start, end not seen yet */
    bool dropping;                /**< lag limit: skipping the rest of drop_epoch */
    uint32_t drop_epoch;
    bool drop_run;                /**< in a lag-limit event (no frame delivered since it began) */
    uint8_t sticky_next;          /**< next sticky slot to replay (RTCM_CORE_STICKY_SLOTS = done) */
    uint32_t release_gen;         /**< the hub's release_gen when this position was taken */
    bool restarted;               /**< the last read started over after a release (rtcm_core_take_restart) */
    bool finishing;               /**< deliver only the rest of the epoch being delivered (rtcm_core_finish) */
    bool finished;                /**< ... and that epoch is over: nothing more until rtcm_core_resync() */
    rtcm_core_counters_t cnt;
} rtcm_core_consumer_t;

/** Where rtcm_core_hold() put the hold. */
typedef enum {
    RTCM_CORE_HOLD_NONE = 0,  /**< not placed: an epoch is in flight (try again, or force) */
    RTCM_CORE_HOLD_BOUNDARY,  /**< between two epochs */
    RTCM_CORE_HOLD_QUIET,     /**< forced: after an open epoch whose line went quiet (its last MSM never came) */
    RTCM_CORE_HOLD_CUT,       /**< forced while an epoch was in flight: at that epoch's start */
} rtcm_core_hold_t;

/** Latest copy of one station message type, double-buffered. */
typedef struct {
    uint8_t buf[2][GNSS_RTCM3_MAX_FRAME];
    uint16_t len[2];
    uint64_t rx_ms[2];
    bool exact[2];
    uint8_t active;
    bool valid;
    uint32_t gen;
} rtcm_core_sticky_t;

/** Hub core state. Allocate it statically or on the heap (about 13 KB). */
typedef struct {
    rtcm_core_lock_t lock;
    uint8_t *ring;
    size_t cap;
    uint32_t default_lag_ms;
    unsigned max_consumers;
    /* written by the producer under the lock */
    uint64_t write_off;
    uint64_t reserve_off;
    uint64_t frames_in;
    uint64_t bytes_in;
    uint64_t publish_failures;
    uint64_t last_rx_ms;          /**< 64-bit arrival of the newest frame */
    bool have_frame;
    uint32_t committed_epoch;     /**< epoch of the newest committed record */
    bool have_epoch;
    rtcm_epoch_t ep;              /**< changed under the lock (publish, clear_sticky) */
    uint64_t epoch_start_off;     /**< under the lock: offset of the newest epoch's first record (or its wrap marker) */
    bool held;                    /**< under the lock: a hold is set (rtcm_core_hold) */
    uint64_t hold_off;            /**< ... no consumer reads a record at or after this offset */
    rtcm_core_hold_t hold_kind;
    uint32_t release_gen;         /**< under the lock: holds released so far */
    rtcm_core_sticky_t sticky[RTCM_CORE_STICKY_SLOTS];
    bool waking;                  /**< a wake pass is running */
    uint32_t wake_gen;            /**< completed wake passes */
    uint32_t attached;
    uint64_t corrupt;             /**< intact records that failed the sanity checks (expected 0) */
    rtcm_core_consumer_t cons[RTCM_CORE_MAX_CONSUMERS];
} rtcm_core_t;

/**
 * Initialise over `ring` (`ring_len` bytes, rounded down to a multiple of 4).
 * @return false if the ring is smaller than RTCM_CORE_MIN_RING or an argument
 *         is out of range.
 */
bool rtcm_core_init(rtcm_core_t *c, uint8_t *ring, size_t ring_len, const rtcm_core_cfg_t *cfg,
                    const rtcm_core_lock_t *lock);

/**
 * Store one whole frame (producer) that arrived at `arrival_ms` (`timed`:
 * the exact arrival, which the epoch gap rule needs). Never blocks.
 *
 * @param wake receives the waiters to notify (room for RTCM_CORE_MAX_CONSUMERS).
 * @param n_wake number of waiters handed back; if > 0, call
 *        rtcm_core_wake_done() after notifying them.
 * @return false if the frame was rejected (NULL, shorter than an empty frame
 *         or longer than a maximum frame); counted in publish_failures.
 */
bool rtcm_core_publish(rtcm_core_t *c, const uint8_t *frame, size_t len, uint16_t msg_type, uint64_t arrival_ms,
                       bool timed, void **wake, size_t *n_wake);

/** End of the wake pass started by a publish that returned waiters. */
void rtcm_core_wake_done(rtcm_core_t *c);

/** Forget the sticky copies; the next frame starts a new epoch. */
void rtcm_core_clear_sticky(rtcm_core_t *c);

/**
 * Attach a consumer: sticky copies first, then live data from the next sure
 * epoch start. @return NULL when max_consumers are attached.
 *
 * A waiting consumer (after attach, resync or a lap) never starts on an
 * unsure epoch: it skips it whole (idle skips, or losses after a lap). A
 * streaming consumer that meets a new unsure epoch ends the epoch it was
 * sending (counted in epochs_out if it was sent from its start), goes back to
 * waiting and skips the unsure frames as idle; it starts again at the next
 * sure epoch. So a caster never sends the tail of an epoch whose head was
 * lost at a framer reset.
 */
rtcm_core_consumer_t *rtcm_core_attach(rtcm_core_t *c, const char *name);

/**
 * Detach a consumer (its slot becomes free).
 * @return true if a wake pass that may still hold its waiter is running; the
 *         caller then waits until rtcm_core_wake_passed(c, *gen) is true.
 */
bool rtcm_core_detach(rtcm_core_t *c, rtcm_core_consumer_t *k, uint32_t *gen);

/** True once the wake pass that was running at detach has ended. */
bool rtcm_core_wake_passed(rtcm_core_t *c, uint32_t gen);

/**
 * Copy pending sticky copies and as many whole frames as fit (owner task).
 * @param max_len at least GNSS_RTCM3_MAX_FRAME (0 is returned otherwise).
 * @return bytes copied, 0 if nothing is available.
 */
size_t rtcm_core_read(rtcm_core_t *c, rtcm_core_consumer_t *k, uint8_t *buf, size_t max_len, uint64_t now_ms);

/** Identical read, plus arrival metadata for delivered bytes; meta may be NULL. */
size_t rtcm_core_read_with_meta(rtcm_core_t *c, rtcm_core_consumer_t *k, uint8_t *buf, size_t max_len,
                                uint64_t now_ms, rtcm_hub_read_meta_t *meta);

/** Re-position as on attach; everything unread counts as idle skips; ends finishing (owner task). */
void rtcm_core_resync(rtcm_core_t *c, rtcm_core_consumer_t *k);

/** Set the lag limit. @return false outside RTCM_CORE_LAG_MIN_MS..RTCM_CORE_LAG_MAX_MS. */
bool rtcm_core_set_max_lag(rtcm_core_t *c, rtcm_core_consumer_t *k, uint32_t ms);

/**
 * Register `waiter` to be handed back by the next publish (owner task, after
 * a read that returned 0). @return false if data arrived meanwhile: read again.
 * At a hold point (or with only sticky copies pending while held) there is
 * nothing to read: the waiter is registered.
 */
bool rtcm_core_prepare_wait(rtcm_core_t *c, rtcm_core_consumer_t *k, void *waiter);

/** Withdraw a registration (after the wait ended either way). */
void rtcm_core_cancel_wait(rtcm_core_t *c, rtcm_core_consumer_t *k);

/** Hub-wide counters. */
void rtcm_core_get_stats(rtcm_core_t *c, uint64_t now_ms, rtcm_core_stats_t *out);

/**
 * The quiet-line view of the newest epoch (any task): true while it holds an
 * MSM that announced more (multiple-message bit set) and the newest frame is
 * younger than `quiet_ms`, so a line quiet that long ends an open epoch here.
 * rtcm_core_hold() does not decide by it: there an open epoch stays in
 * flight however quiet the line is, until the caller forces the hold (a
 * late last MSM after a quiet line is the normal case under load, and this
 * view cut such an epoch). Nothing in the firmware uses it to stop a stream.
 * @param age_ms optional: the newest frame's age, UINT32_MAX before the first.
 */
bool rtcm_core_epoch_in_flight(rtcm_core_t *c, uint64_t now_ms, uint32_t quiet_ms, uint32_t *age_ms);

/**
 * Hold the stream at an epoch boundary (any task): no consumer gets a record
 * at or after the hold point (nor a sticky replay) until rtcm_core_release().
 * The point is chosen under the lock with the epoch state, so a frame being
 * stored at that moment is on the right side of it:
 *  - nothing stored yet, the newest epoch ended by its last MSM, or the next
 *    frame starts an epoch anyway (rtcm_core_clear_sticky): the write head,
 *    including a record still being copied;
 *  - the newest epoch holds no MSM yet (its leading station messages): that
 *    epoch's first record. A consumer that already sent some of them stops
 *    where it is: they are station messages, never part of an MSM run;
 *  - the newest epoch holds an MSM that announced more: it is in flight,
 *    however quiet the line is (the rest may come late: a slow reader, a
 *    stalled line), so RTCM_CORE_HOLD_NONE and no hold. With `force` (the
 *    caller waited long enough): its newest frame `quiet_ms` or more old and
 *    nothing `arriving` means its last MSM never came (or the receiver never
 *    clears the bit), so the write head (RTCM_CORE_HOLD_QUIET); else that
 *    epoch's first record (RTCM_CORE_HOLD_CUT), so no consumer starts it and
 *    one already inside it is cut there.
 * `arriving`: the producer has bytes it received and not yet published (the
 * caller knows, the core cannot): the line is not quiet, its reader is
 * behind, so an open epoch is still arriving however old its newest frame
 * is. It changes only the forced decision; the others are positions that
 * hold whatever comes next.
 * A hold already set is kept (its kind is returned).
 */
rtcm_core_hold_t rtcm_core_hold(rtcm_core_t *c, uint64_t now_ms, uint32_t quiet_ms, bool force, bool arriving);

/**
 * End the hold (any task; nothing if none is set). Every consumer starts over
 * as on rtcm_core_resync() at its next read (what was published from the
 * hold point on belongs to the pause): sticky copies, then the next sure
 * epoch. The frames it skips count as idle skips.
 */
void rtcm_core_release(rtcm_core_t *c);

/** A hold is set (any task). */
bool rtcm_core_held(rtcm_core_t *c);

/**
 * True when this consumer gets nothing more for now (owner task): a hold is
 * set and it has passed every record before the hold point (until the
 * release), or it is finishing (rtcm_core_finish) and the epoch it was
 * delivering is over (until rtcm_core_resync()).
 */
bool rtcm_core_at_hold(rtcm_core_t *c, rtcm_core_consumer_t *k);

/**
 * Finish this consumer's epoch (owner task), before its reader stops for
 * good (a caster session that is about to close): from now on it gets the
 * rest of the epoch it is delivering, and nothing of another one, nor a
 * sticky copy. The epoch is over at the first record of another epoch, or,
 * with every record read and none being stored, when the newest epoch is
 * that one and needs nothing more: its last MSM is in, it holds only its
 * leading station messages so far, or the next frame starts an epoch anyway.
 * Never by a quiet line: an epoch whose last MSM was lost ends when the next
 * one starts (the caller bounds the wait). A consumer that is not inside an
 * epoch (waiting for an epoch start, or dropping one by the lag limit) is
 * done at once. rtcm_core_resync() ends finishing.
 */
void rtcm_core_finish(rtcm_core_t *c, rtcm_core_consumer_t *k);

/**
 * True once after a read that started this consumer over because a hold was
 * released since its position was taken (owner task): the frames from that
 * read on begin with sticky copies and a whole epoch.
 */
bool rtcm_core_take_restart(rtcm_core_consumer_t *k);

/** Counters of one consumer (any task). */
void rtcm_core_get_consumer_stats(rtcm_core_t *c, rtcm_core_consumer_t *k, uint64_t now_ms,
                                  rtcm_core_consumer_stats_t *out);

/** Index of the sticky slot for a message type, or -1. */
int rtcm_core_sticky_slot(uint16_t msg_type);

#ifdef __cplusplus
}
#endif
