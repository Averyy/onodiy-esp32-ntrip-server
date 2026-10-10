/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "cert_time.h"

#include <time.h>

/* <0, 0, >0 as a is before, at or after b, field by field like Mbed TLS. */
static int time_cmp(const mbedtls_x509_time *a, const mbedtls_x509_time *b)
{
    const int fa[] = { a->year, a->mon, a->day, a->hour, a->min, a->sec };
    const int fb[] = { b->year, b->mon, b->day, b->hour, b->min, b->sec };
    for (size_t i = 0; i < sizeof(fa) / sizeof(fa[0]); i++) {
        if (fa[i] != fb[i]) {
            return fa[i] < fb[i] ? -1 : 1;
        }
    }
    return 0;
}

static bool is_bundle_stand_in(const mbedtls_x509_crt *crt)
{
    return crt->raw.p == NULL && crt->valid_from.year == 0 && crt->valid_to.year == 0;
}

uint32_t fw_cert_time_flags(const mbedtls_x509_crt *crt, bool clock_trusted, int64_t now_unix)
{
    if (crt != NULL && is_bundle_stand_in(crt)) {
        return 0;
    }
    struct tm tm;
    time_t t = (time_t)now_unix;
    if (crt == NULL || !clock_trusted || (int64_t)t != now_unix || gmtime_r(&t, &tm) == NULL) {
        return MBEDTLS_X509_BADCERT_OTHER;
    }
    const mbedtls_x509_time now = {
        .year = tm.tm_year + 1900, .mon = tm.tm_mon + 1, .day = tm.tm_mday,
        .hour = tm.tm_hour, .min = tm.tm_min, .sec = tm.tm_sec,
    };
    uint32_t flags = 0;
    if (time_cmp(&crt->valid_from, &now) > 0) {
        flags |= MBEDTLS_X509_BADCERT_FUTURE;
    }
    if (time_cmp(&crt->valid_to, &now) < 0) {
        flags |= MBEDTLS_X509_BADCERT_EXPIRED;
    }
    return flags;
}
