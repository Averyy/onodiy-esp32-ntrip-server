/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "diag_journal_core.h"

#include <string.h>

#include "diag_crc.h"

#define CHUNK_SLOTS 16u                         /* slots read per flash call */
#define CHUNK_BYTES (CHUNK_SLOTS * DJ_SLOT_SIZE)

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool all_ff(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0xFFu) {
            return false;
        }
    }
    return true;
}

/* ---- codec ------------------------------------------------------------------------ */

void dj_encode_record(const dj_entry_t *e, uint8_t out[DJ_SLOT_SIZE])
{
    uint64_t unix_u = (uint64_t)e->unix_s;
    put32(out + 0, e->seq);
    put32(out + 4, e->uptime_s);
    put32(out + 8, (uint32_t)unix_u);
    put32(out + 12, (uint32_t)(unix_u >> 32));
    put16(out + 16, e->kind);
    put16(out + 18, 0);
    put32(out + 20, (uint32_t)e->a);
    put32(out + 24, (uint32_t)e->b);
    put32(out + 28, diag_crc32(out, 28));
}

int dj_decode_record(const uint8_t in[DJ_SLOT_SIZE], dj_entry_t *e)
{
    if (all_ff(in, DJ_SLOT_SIZE)) {
        return 0;
    }
    if (diag_crc32(in, 28) != get32(in + 28)) {
        return -1;
    }
    uint32_t seq = get32(in);
    if (seq == 0 || seq == UINT32_MAX) {
        return -1;
    }
    if (e != NULL) {
        e->seq = seq;
        e->uptime_s = get32(in + 4);
        e->unix_s = (int64_t)((uint64_t)get32(in + 8) | ((uint64_t)get32(in + 12) << 32));
        e->kind = get16(in + 16);
        e->a = (int32_t)get32(in + 20);
        e->b = (int32_t)get32(in + 24);
    }
    return 1;
}

void dj_encode_sector_hdr(uint32_t sector_seq, uint32_t first_seq, uint16_t rec_format, uint8_t out[DJ_SLOT_SIZE])
{
    memset(out, 0xFF, DJ_SLOT_SIZE);
    put32(out + 0, DJ_MAGIC);
    put16(out + 4, DJ_HDR_FORMAT);
    put16(out + 6, rec_format);
    put32(out + 8, sector_seq);
    put32(out + 12, first_seq);
    put16(out + 16, DJ_SLOT_SIZE);
    put32(out + 28, diag_crc32(out, 28));
}

int dj_decode_sector_hdr(const uint8_t in[DJ_SLOT_SIZE], dj_sector_t *s)
{
    dj_sector_t r = { 0 };
    int res;
    if (all_ff(in, DJ_SLOT_SIZE)) {
        res = 0;
    } else if (get32(in) != DJ_MAGIC || get16(in + 4) != DJ_HDR_FORMAT || diag_crc32(in, 28) != get32(in + 28) ||
               get32(in + 8) == 0) {
        res = -1;
    } else {
        r.valid = true;
        /* Another record size is another record layout: unreadable here. */
        r.rec_format = (get16(in + 16) == DJ_SLOT_SIZE) ? get16(in + 6) : 0xFFFFu;
        r.sector_seq = get32(in + 8);
        r.first_seq = get32(in + 12);
        res = 1;
    }
    if (s != NULL) {
        *s = r;
    }
    return res;
}

/* ---- flash helpers ------------------------------------------------------------------ */

static int rd(dj_t *j, uint32_t off, void *buf, size_t len)
{
    if (j->fl.read(j->fl.ctx, off, buf, len) != 0) {
        j->io_errors++;
        return DJ_ERR_IO;
    }
    return DJ_OK;
}

static uint32_t sector_off(uint32_t k)
{
    return k * DJ_SECTOR_SIZE;
}

/* ---- mount ------------------------------------------------------------------------ */

static void scan_current(dj_t *j)
{
    dj_sector_t *s = &j->sec[j->cur];
    if (s->rec_format != DJ_REC_FORMAT) {
        /* Written by a firmware with another record layout: leave it full. */
        j->cur_slot = DJ_SLOTS;
        j->next_seq = s->first_seq + DJ_RECS_PER_SECTOR;
        return;
    }
    uint8_t buf[CHUNK_BYTES];
    uint32_t last_used = 0;
    uint32_t max_seq = 0;
    for (uint32_t base = 0; base < DJ_SLOTS; base += CHUNK_SLOTS) {
        if (rd(j, sector_off((uint32_t)j->cur) + base * DJ_SLOT_SIZE, buf, sizeof(buf)) != DJ_OK) {
            /* Unreadable: never write into it, and number past anything it may hold. */
            j->cur_slot = DJ_SLOTS;
            j->next_seq = s->first_seq + DJ_RECS_PER_SECTOR;
            return;
        }
        for (uint32_t i = 0; i < CHUNK_SLOTS; i++) {
            uint32_t slot = base + i;
            if (slot == 0) {
                continue;
            }
            dj_entry_t e;
            int r = dj_decode_record(buf + i * DJ_SLOT_SIZE, &e);
            if (r == 0) {
                continue;
            }
            last_used = slot;
            if (r > 0) {
                if (e.seq > max_seq) {
                    max_seq = e.seq;
                }
            } else {
                j->bad_slots++;
            }
        }
    }
    j->cur_slot = last_used + 1u;
    j->next_seq = s->first_seq;
    if (max_seq >= j->next_seq) {
        j->next_seq = max_seq + 1u;
    }
    if (j->next_seq == 0) {
        j->next_seq = 1;
    }
}

int dj_mount(dj_t *j, const dj_flash_t *fl, uint32_t size)
{
    memset(j, 0, sizeof(*j));
    j->fl = *fl;
    j->cur = -1;
    if (size % DJ_SECTOR_SIZE != 0 || size / DJ_SECTOR_SIZE < 2u || size / DJ_SECTOR_SIZE > DJ_MAX_SECTORS) {
        return DJ_ERR_SIZE;
    }
    j->sectors = size / DJ_SECTOR_SIZE;

    for (uint32_t k = 0; k < j->sectors; k++) {
        uint8_t hdr[DJ_SLOT_SIZE];
        if (rd(j, sector_off(k), hdr, sizeof(hdr)) != DJ_OK) {
            continue;
        }
        if (dj_decode_sector_hdr(hdr, &j->sec[k]) > 0 && j->sec[k].sector_seq > j->top_sector_seq) {
            j->top_sector_seq = j->sec[k].sector_seq;
            j->cur = (int32_t)k;
        }
    }
    if (j->cur < 0) {
        j->cur_slot = DJ_SLOTS;
        j->next_seq = 1;
    } else {
        scan_current(j);
    }
    j->mounted = true;
    return DJ_OK;
}

/* ---- append ------------------------------------------------------------------------ */

/* Open the ring's next sector with sector_seq top + `step` (1; a clear adds the sector count). */
static int open_sector(dj_t *j, uint32_t step)
{
    uint32_t k = (j->cur < 0) ? 0u : ((uint32_t)j->cur + 1u) % j->sectors;
    j->stale &= ~(1ull << k);   /* erased here: nothing left for a clear to do */
    /* The oldest records are dropped here, whatever happens next. The
     * sector is always erased, also when it reads blank: a blank sector can
     * be one whose erase a power cut interrupted, and such cells may read
     * as erased but not hold what is written next (NOR flash wants an
     * interrupted erase repeated). On a fresh partition that costs one
     * erase per sector, once. */
    j->sec[k].valid = false;
    if (j->fl.erase(j->fl.ctx, sector_off(k), DJ_SECTOR_SIZE) != 0) {
        j->io_errors++;
        return DJ_ERR_IO;
    }
    j->erases++;
    uint32_t seq = j->top_sector_seq + step;
    uint8_t hdr[DJ_SLOT_SIZE];
    dj_encode_sector_hdr(seq, j->next_seq, DJ_REC_FORMAT, hdr);
    if (j->fl.write(j->fl.ctx, sector_off(k), hdr, sizeof(hdr)) != 0) {
        /* Not blank any more: the next attempt erases it again. */
        j->io_errors++;
        return DJ_ERR_IO;
    }
    j->sec[k].valid = true;
    j->sec[k].rec_format = DJ_REC_FORMAT;
    j->sec[k].sector_seq = seq;
    j->sec[k].first_seq = j->next_seq;
    j->top_sector_seq = seq;
    j->cur = (int32_t)k;
    j->cur_slot = 1;
    return DJ_OK;
}

static int open_next(dj_t *j)
{
    return open_sector(j, 1u);
}

bool dj_needs_open(const dj_t *j)
{
    return j->cur < 0 || j->cur_slot >= DJ_SLOTS;
}

int dj_append(dj_t *j, dj_entry_t *e)
{
    if (!j->mounted) {
        return DJ_ERR_STATE;
    }
    if (j->cur < 0 || j->cur_slot >= DJ_SLOTS) {
        int r = open_next(j);
        if (r != DJ_OK) {
            return r;
        }
    }
    e->seq = j->next_seq;
    uint8_t rec[DJ_SLOT_SIZE];
    dj_encode_record(e, rec);
    uint32_t off = sector_off((uint32_t)j->cur) + j->cur_slot * DJ_SLOT_SIZE;
    /* A slot that failed may hold part of a record: never write it again. */
    j->cur_slot++;
    j->next_seq++;
    if (j->next_seq == 0) {
        j->next_seq = 1;
    }
    if (j->fl.write(j->fl.ctx, off, rec, sizeof(rec)) != 0) {
        j->io_errors++;
        return DJ_ERR_IO;
    }
    j->written++;
    return DJ_OK;
}

/* ---- clear ------------------------------------------------------------------------ */

int dj_clear_start(dj_t *j)
{
    if (!j->mounted) {
        return DJ_ERR_STATE;
    }
    if (j->cur < 0) {
        return DJ_OK;   /* no sector was ever opened: nothing to clear */
    }
    uint64_t others = 0;
    for (uint32_t k = 0; k < j->sectors; k++) {
        if (j->sec[k].valid) {
            others |= 1ull << k;
        }
    }
    /* The new header first: from then on a mount finds it as the current sector. Every sector is erased once
     * (this one now, the others by dj_clear_step), so its number goes up by the sector count: the newest
     * number over the sector count stays the erase cycles each sector has seen. */
    int r = open_sector(j, j->sectors);
    if (r != DJ_OK) {
        return r;
    }
    others &= ~(1ull << (uint32_t)j->cur);
    for (uint32_t k = 0; k < j->sectors; k++) {
        if ((others >> k) & 1u) {
            j->sec[k].valid = false;
        }
    }
    j->stale |= others;
    return DJ_OK;
}

int dj_clear_step(dj_t *j)
{
    for (uint32_t k = 0; k < j->sectors; k++) {
        if (((j->stale >> k) & 1u) == 0) {
            continue;
        }
        j->stale &= ~(1ull << k);
        if (j->fl.erase(j->fl.ctx, sector_off(k), DJ_SECTOR_SIZE) != 0) {
            j->io_errors++;
            return DJ_ERR_IO;
        }
        j->erases++;
        return j->stale != 0 ? 1 : 0;
    }
    return 0;
}

/* ---- read ------------------------------------------------------------------------- */

/* Valid sectors in ring order (ascending sector_seq). */
static uint32_t ring_order(const dj_t *j, uint8_t *idx)
{
    uint32_t n = 0;
    for (uint32_t k = 0; k < j->sectors; k++) {
        if (!j->sec[k].valid) {
            continue;
        }
        uint32_t i = n++;
        while (i > 0 && j->sec[idx[i - 1]].sector_seq > j->sec[k].sector_seq) {
            idx[i] = idx[i - 1];
            i--;
        }
        idx[i] = (uint8_t)k;
    }
    return n;
}

size_t dj_read(dj_t *j, uint32_t *cursor, dj_entry_t *out, size_t cap, bool *more)
{
    if (more != NULL) {
        *more = false;
    }
    if (!j->mounted || cursor == NULL || out == NULL || cap == 0) {
        return 0;
    }
    if (*cursor >= j->next_seq) {
        *cursor = 0;   /* from before the journal was started over */
    }
    uint8_t order[DJ_MAX_SECTORS];
    uint32_t n_sec = ring_order(j, order);
    size_t n = 0;
    uint8_t buf[CHUNK_BYTES];
    for (uint32_t i = 0; i < n_sec; i++) {
        uint32_t k = order[i];
        const dj_sector_t *s = &j->sec[k];
        if (i + 1u < n_sec && j->sec[order[i + 1u]].first_seq <= *cursor + 1u) {
            continue;   /* every record here is older than the cursor */
        }
        if (s->rec_format != DJ_REC_FORMAT) {
            continue;
        }
        uint32_t end = ((int32_t)k == j->cur) ? j->cur_slot : DJ_SLOTS;
        for (uint32_t base = 0; base < end; base += CHUNK_SLOTS) {
            if (rd(j, sector_off(k) + base * DJ_SLOT_SIZE, buf, sizeof(buf)) != DJ_OK) {
                break;
            }
            for (uint32_t t = 0; t < CHUNK_SLOTS && base + t < end; t++) {
                if (base + t == 0) {
                    continue;
                }
                dj_entry_t e;
                if (dj_decode_record(buf + t * DJ_SLOT_SIZE, &e) <= 0 || e.seq <= *cursor) {
                    continue;
                }
                if (n == cap) {
                    if (more != NULL) {
                        *more = true;
                    }
                    return n;
                }
                out[n++] = e;
                *cursor = e.seq;
            }
        }
    }
    return n;
}

uint32_t dj_oldest_seq(const dj_t *j)
{
    uint32_t best_sector = 0;
    uint32_t seq = 0;
    for (uint32_t k = 0; k < j->sectors; k++) {
        if (j->sec[k].valid && (best_sector == 0 || j->sec[k].sector_seq < best_sector)) {
            best_sector = j->sec[k].sector_seq;
            seq = j->sec[k].first_seq;
        }
    }
    return (seq < j->next_seq) ? seq : 0;
}

uint32_t dj_capacity(const dj_t *j)
{
    return j->sectors * DJ_RECS_PER_SECTOR;
}

/* ---- rate limits -------------------------------------------------------------------- */

void dj_rate_init(dj_rate_t *r, uint32_t min_interval_ms, uint32_t per_hour, uint32_t exempt_mask)
{
    memset(r, 0, sizeof(*r));
    r->min_interval_ms = min_interval_ms;
    r->per_hour = per_hour;
    r->exempt_mask = exempt_mask;
    uint64_t burst = per_hour / 6u;   /* ten minutes' worth */
    if (burst < 10u) {
        burst = 10u;
    }
    r->burst_milli = burst * 1000u;
    r->tokens_milli = r->burst_milli;
}

dj_rate_result_t dj_rate_check(dj_rate_t *r, uint16_t kind, uint32_t now_ms)
{
    if (!r->started) {
        r->started = true;
        r->refill_ms = now_ms;
    }
    if (r->per_hour != 0) {
        /* tokens per ms = per_hour / 3 600 000, i.e. per_hour / 3600 thousandths */
        uint64_t acc = (uint64_t)(uint32_t)(now_ms - r->refill_ms) * r->per_hour + r->refill_rem;
        r->refill_ms = now_ms;
        r->refill_rem = (uint32_t)(acc % 3600u);
        r->tokens_milli += acc / 3600u;
        if (r->tokens_milli > r->burst_milli) {
            r->tokens_milli = r->burst_milli;
            r->refill_rem = 0;
        }
    }
    uint32_t idx = (kind < DJ_RATE_KINDS) ? kind : DJ_RATE_KINDS - 1u;
    bool exempt = kind < DJ_RATE_KINDS && ((r->exempt_mask >> kind) & 1u) != 0;
    if (!exempt) {
        if (((r->seen_mask >> idx) & 1u) != 0 && (uint32_t)(now_ms - r->last_ms[idx]) < r->min_interval_ms) {
            r->refused_kind++;
            return DJ_RATE_KIND;
        }
        if (r->per_hour != 0 && r->tokens_milli < 1000u) {
            r->refused_budget++;
            return DJ_RATE_BUDGET;
        }
    }
    if (r->per_hour != 0 && r->tokens_milli >= 1000u) {
        r->tokens_milli -= 1000u;
    }
    r->last_ms[idx] = now_ms;
    r->seen_mask |= 1u << idx;
    return DJ_RATE_OK;
}
