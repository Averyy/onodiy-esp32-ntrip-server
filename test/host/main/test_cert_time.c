/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Certificate validity dates on update transfers (fw_update cert_time.c).
 */
#include <string.h>

#include "cert_time.h"
#include "unity.h"

/* 2026-10-10 12:00:00 UTC */
#define NOW 1791633600LL

static const unsigned char s_der[1];

static mbedtls_x509_crt crt(int fy, int fmo, int fd, int fh, int fmi, int fs, int ty, int tmo, int td, int th, int tmi,
                            int ts)
{
    mbedtls_x509_crt c;
    memset(&c, 0, sizeof(c));
    c.raw.p = (unsigned char *)s_der;
    c.raw.len = sizeof(s_der);
    c.valid_from = (mbedtls_x509_time){ .year = fy, .mon = fmo, .day = fd, .hour = fh, .min = fmi, .sec = fs };
    c.valid_to = (mbedtls_x509_time){ .year = ty, .mon = tmo, .day = td, .hour = th, .min = tmi, .sec = ts };
    return c;
}

TEST_CASE("cert dates: valid, not yet valid, expired", "[cert_time]")
{
    mbedtls_x509_crt c = crt(2026, 1, 1, 0, 0, 0, 2027, 1, 1, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(0, fw_cert_time_flags(&c, true, NOW));

    /* Both ends count as valid, to the second. */
    c = crt(2026, 10, 10, 12, 0, 0, 2026, 10, 10, 12, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(0, fw_cert_time_flags(&c, true, NOW));
    TEST_ASSERT_EQUAL_UINT32(MBEDTLS_X509_BADCERT_FUTURE, fw_cert_time_flags(&c, true, NOW - 1));
    TEST_ASSERT_EQUAL_UINT32(MBEDTLS_X509_BADCERT_EXPIRED, fw_cert_time_flags(&c, true, NOW + 1));

    /* Each field decides when the larger ones are equal. */
    c = crt(2026, 10, 10, 12, 0, 1, 2030, 1, 1, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(MBEDTLS_X509_BADCERT_FUTURE, fw_cert_time_flags(&c, true, NOW));
    c = crt(2026, 10, 11, 0, 0, 0, 2030, 1, 1, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(MBEDTLS_X509_BADCERT_FUTURE, fw_cert_time_flags(&c, true, NOW));
    c = crt(2026, 11, 1, 0, 0, 0, 2030, 1, 1, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(MBEDTLS_X509_BADCERT_FUTURE, fw_cert_time_flags(&c, true, NOW));
    c = crt(2020, 1, 1, 0, 0, 0, 2026, 10, 10, 11, 59, 59);
    TEST_ASSERT_EQUAL_UINT32(MBEDTLS_X509_BADCERT_EXPIRED, fw_cert_time_flags(&c, true, NOW));
    c = crt(2020, 1, 1, 0, 0, 0, 2026, 9, 30, 23, 59, 59);
    TEST_ASSERT_EQUAL_UINT32(MBEDTLS_X509_BADCERT_EXPIRED, fw_cert_time_flags(&c, true, NOW));
    c = crt(2020, 1, 1, 0, 0, 0, 2025, 12, 31, 23, 59, 59);
    TEST_ASSERT_EQUAL_UINT32(MBEDTLS_X509_BADCERT_EXPIRED, fw_cert_time_flags(&c, true, NOW));

    /* Valid-from after valid-to: both. */
    c = crt(2027, 1, 1, 0, 0, 0, 2025, 1, 1, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(MBEDTLS_X509_BADCERT_FUTURE | MBEDTLS_X509_BADCERT_EXPIRED,
                             fw_cert_time_flags(&c, true, NOW));

    /* Past 2038 (64-bit time). */
    c = crt(2040, 1, 1, 0, 0, 0, 2049, 12, 31, 23, 59, 59);
    TEST_ASSERT_EQUAL_UINT32(0, fw_cert_time_flags(&c, true, 2208988800LL));   /* 2040-01-01 */
    TEST_ASSERT_EQUAL_UINT32(MBEDTLS_X509_BADCERT_FUTURE, fw_cert_time_flags(&c, true, NOW));
}

TEST_CASE("cert dates: no trusted clock fails every real certificate", "[cert_time]")
{
    mbedtls_x509_crt c = crt(2000, 1, 1, 0, 0, 0, 2099, 12, 31, 23, 59, 59);
    TEST_ASSERT_EQUAL_UINT32(MBEDTLS_X509_BADCERT_OTHER, fw_cert_time_flags(&c, false, NOW));
    TEST_ASSERT_EQUAL_UINT32(MBEDTLS_X509_BADCERT_OTHER, fw_cert_time_flags(&c, false, 0));
    TEST_ASSERT_EQUAL_UINT32(MBEDTLS_X509_BADCERT_OTHER, fw_cert_time_flags(NULL, true, NOW));
}

TEST_CASE("cert dates: the bundle's stand-in root has none to check", "[cert_time]")
{
    /* What the CA bundle hands Mbed TLS for a bundled root: name and key, no DER, no dates. */
    mbedtls_x509_crt root;
    memset(&root, 0, sizeof(root));
    TEST_ASSERT_EQUAL_UINT32(0, fw_cert_time_flags(&root, true, NOW));
    TEST_ASSERT_EQUAL_UINT32(0, fw_cert_time_flags(&root, false, NOW));

    /* A real certificate without dates is not the stand-in. */
    mbedtls_x509_crt c = crt(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, fw_cert_time_flags(&c, true, NOW));
    /* Dates without DER are checked like any other. */
    root.valid_from = (mbedtls_x509_time){ .year = 2027, .mon = 1, .day = 1 };
    root.valid_to = (mbedtls_x509_time){ .year = 2030, .mon = 1, .day = 1 };
    TEST_ASSERT_EQUAL_UINT32(MBEDTLS_X509_BADCERT_FUTURE, fw_cert_time_flags(&root, true, NOW));
}
