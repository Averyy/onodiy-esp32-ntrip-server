/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "diag_logring.h"

#include <string.h>

#include "diag_crc.h"

static uint32_t hdr_crc(const logring_hdr_t *h)
{
    return diag_crc32(h, offsetof(logring_hdr_t, crc));
}

static void sync_range(const logring_t *r, const void *addr, size_t len)
{
    if (r->sync != NULL && len > 0) {
        r->sync(r->sync_ctx, addr, len);
    }
}

static void seal(logring_t *r)
{
    r->hdr->crc = hdr_crc(r->hdr);
    sync_range(r, r->hdr, sizeof(*r->hdr));
}

static uint32_t advance(const logring_t *r, uint32_t off, uint32_t n)
{
    /* off < data_size and n <= data_size, so the sum fits easily. */
    uint32_t v = off + n;
    return (v >= r->data_size) ? v - r->data_size : v;
}

static void put(logring_t *r, uint32_t off, const void *src, uint32_t n)
{
    uint32_t first = r->data_size - off;
    if (first > n) {
        first = n;
    }
    memcpy(r->data + off, src, first);
    if (n > first) {
        memcpy(r->data, (const uint8_t *)src + first, n - first);
    }
}

static void get(const logring_t *r, uint32_t off, void *dst, uint32_t n)
{
    uint32_t first = r->data_size - off;
    if (first > n) {
        first = n;
    }
    memcpy(dst, r->data + off, first);
    if (n > first) {
        memcpy((uint8_t *)dst + first, r->data, n - first);
    }
}

static void sync_data(const logring_t *r, uint32_t off, uint32_t n)
{
    uint32_t first = r->data_size - off;
    if (first > n) {
        first = n;
    }
    sync_range(r, r->data + off, first);
    if (n > first) {
        sync_range(r, r->data, n - first);
    }
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rec_len_at(const logring_t *r, uint32_t off)
{
    uint8_t h[LOGRING_REC_HDR];
    get(r, off, h, sizeof(h));
    return le16(h + 4);
}

/* CRC-16 over the record header's first six bytes and its text. */
static uint16_t rec_check(const logring_t *r, const uint8_t *hdr6, uint32_t text_off, uint32_t len)
{
    uint16_t crc = diag_crc16_update(DIAG_CRC16_INIT, hdr6, 6);
    uint32_t first = r->data_size - text_off;
    if (first > len) {
        first = len;
    }
    crc = diag_crc16_update(crc, r->data + text_off, first);
    if (len > first) {
        crc = diag_crc16_update(crc, r->data, len - first);
    }
    return crc;
}

void logring_attach(logring_t *r, void *mem, size_t mem_size, logring_sync_fn sync, void *sync_ctx)
{
    r->hdr = (logring_hdr_t *)mem;
    r->data = (uint8_t *)mem + sizeof(logring_hdr_t);
    r->data_size = (uint32_t)(mem_size - sizeof(logring_hdr_t));
    r->sync = sync;
    r->sync_ctx = sync_ctx;
}

bool logring_header_ok(const logring_t *r)
{
    const logring_hdr_t *h = r->hdr;
    if (h->magic != LOGRING_MAGIC || h->layout != LOGRING_LAYOUT || h->hdr_size != sizeof(logring_hdr_t) ||
        h->data_size != r->data_size || h->crc != hdr_crc(h)) {
        return false;
    }
    if (h->tail >= r->data_size || h->used > r->data_size || h->first_seq == 0 || h->next_seq < h->first_seq) {
        return false;
    }
    /* Every record takes at least a header and one byte of text. */
    uint32_t count = h->next_seq - h->first_seq;
    return count <= h->used / (LOGRING_REC_HDR + 1u) && (count != 0 || h->used == 0);
}

bool logring_recover(logring_t *r)
{
    if (!logring_header_ok(r)) {
        return false;
    }
    logring_hdr_t *h = r->hdr;
    uint32_t off = h->tail;
    uint32_t seq = h->first_seq;
    uint32_t remaining = h->used;
    while (remaining > 0) {
        if (remaining < LOGRING_REC_HDR) {
            break;
        }
        uint8_t rh[LOGRING_REC_HDR];
        get(r, off, rh, sizeof(rh));
        uint32_t len = le16(rh + 4);
        if (le32(rh) != seq || len == 0 || len > LOGRING_MAX_TEXT || LOGRING_REC_HDR + len > remaining) {
            break;
        }
        if (rec_check(r, rh, advance(r, off, LOGRING_REC_HDR), len) != le16(rh + 6)) {
            break;
        }
        off = advance(r, off, LOGRING_REC_HDR + len);
        remaining -= LOGRING_REC_HDR + len;
        seq++;
    }
    if (remaining != 0 || seq != h->next_seq) {
        h->used -= remaining;
        h->next_seq = seq;
        seal(r);
    }
    return true;
}

void logring_format(logring_t *r, uint32_t boot_id, uint32_t first_seq)
{
    logring_hdr_t *h = r->hdr;
    memset(h, 0, sizeof(*h));
    h->magic = LOGRING_MAGIC;
    h->layout = LOGRING_LAYOUT;
    h->hdr_size = sizeof(logring_hdr_t);
    h->data_size = r->data_size;
    h->boot_id = boot_id;
    h->first_seq = (first_seq == 0) ? 1u : first_seq;
    h->next_seq = h->first_seq;
    seal(r);
}

uint32_t logring_append(logring_t *r, const char *text, size_t len)
{
    if (len == 0) {
        return 0;
    }
    if (len > LOGRING_MAX_TEXT) {
        len = LOGRING_MAX_TEXT;
    }
    if (len > r->data_size - LOGRING_REC_HDR) {
        len = r->data_size - LOGRING_REC_HDR;
    }
    logring_hdr_t *h = r->hdr;
    uint32_t need = LOGRING_REC_HDR + (uint32_t)len;

    if (r->data_size - h->used < need) {
        /* Drop the oldest records, and make that durable before their bytes
         * are overwritten. */
        while (r->data_size - h->used < need) {
            uint32_t size = (h->used >= LOGRING_REC_HDR) ? LOGRING_REC_HDR + rec_len_at(r, h->tail) : 0;
            if (size == 0 || size > h->used || h->first_seq == h->next_seq) {
                /* Inconsistent: start over empty, keeping the numbering. */
                h->tail = 0;
                h->used = 0;
                h->first_seq = h->next_seq;
                break;
            }
            h->tail = advance(r, h->tail, size);
            h->used -= size;
            h->first_seq++;
            h->lost++;
        }
        seal(r);
    }

    uint32_t head = advance(r, h->tail, h->used);
    uint32_t seq = h->next_seq;
    uint8_t rh[LOGRING_REC_HDR];
    rh[0] = (uint8_t)seq;
    rh[1] = (uint8_t)(seq >> 8);
    rh[2] = (uint8_t)(seq >> 16);
    rh[3] = (uint8_t)(seq >> 24);
    rh[4] = (uint8_t)len;
    rh[5] = (uint8_t)(len >> 8);
    uint16_t check = diag_crc16_update(DIAG_CRC16_INIT, rh, 6);
    check = diag_crc16_update(check, text, len);
    rh[6] = (uint8_t)check;
    rh[7] = (uint8_t)(check >> 8);
    put(r, head, rh, sizeof(rh));
    put(r, advance(r, head, LOGRING_REC_HDR), text, (uint32_t)len);
    sync_data(r, head, need);

    h->used += need;
    h->next_seq = seq + 1u;
    seal(r);
    return seq;
}

uint32_t logring_count(const logring_t *r)
{
    return r->hdr->next_seq - r->hdr->first_seq;
}

size_t logring_read(const logring_t *r, uint32_t *cursor, char *out, size_t cap, logring_read_t *info)
{
    logring_read_t res = { 0 };
    size_t pos = 0;
    if (cap == 0 || out == NULL || cursor == NULL) {
        if (info != NULL) {
            *info = res;
        }
        return 0;
    }
    const logring_hdr_t *h = r->hdr;
    uint32_t first = h->first_seq;
    uint32_t next = h->next_seq;
    uint32_t want = *cursor + 1u;
    if (*cursor == 0) {
        want = first;                       /* a new reader: everything held, no gap */
    } else if (want < first) {
        res.gap = true;                     /* overwritten before it was read */
        want = first;
    } else if (want > next) {
        res.gap = true;                     /* a cursor from another boot */
        want = first;
    }

    uint32_t off = h->tail;
    uint32_t seq = first;
    while (seq < want) {
        off = advance(r, off, LOGRING_REC_HDR + rec_len_at(r, off));
        seq++;
    }
    while (seq < next) {
        uint32_t len = rec_len_at(r, off);
        uint32_t copy = len;
        if (pos + len > cap - 1) {
            if (pos != 0) {
                break;
            }
            copy = (uint32_t)(cap - 1);     /* cut, so the reader still advances */
        }
        get(r, advance(r, off, LOGRING_REC_HDR), out + pos, copy);
        pos += copy;
        *cursor = seq;
        res.records++;
        off = advance(r, off, LOGRING_REC_HDR + len);
        seq++;
        if (copy != len) {
            break;
        }
    }
    res.more = seq < next;
    out[pos] = '\0';
    if (info != NULL) {
        *info = res;
    }
    return pos;
}
