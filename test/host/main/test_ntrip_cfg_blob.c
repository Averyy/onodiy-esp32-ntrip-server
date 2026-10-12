/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "ntrip_cfg_blob.h"
#include "ntrip_proto.h"
#include "ntrip_server.h"
#include "unity.h"

static ntrip_caster_cfg_t onocoy_slot(void)
{
    ntrip_caster_cfg_t c;
    ntrip_server_onocoy_defaults(&c);
    c.enabled = true;
    strcpy(c.mountpoint, "MP01");
    strcpy(c.username, "console-user");
    strcpy(c.password, "pw1");
    strcpy(c.label, "station");
    return c;
}

/* A record written before the layout byte existed: the same fields, without it (and the flags byte after it). */
static size_t as_old(const ntrip_caster_cfg_t *c, uint8_t *buf, size_t cap)
{
    size_t len = ntrip_cfg_encode(c, buf, cap);
    TEST_ASSERT_TRUE(len > 2);
    TEST_ASSERT_EQUAL_UINT8(NTRIP_CFG_LAYOUT, buf[len - 2]);
    return len - 2u;
}

TEST_CASE("caster record: round trip with the current layout", "[ntrip]")
{
    ntrip_caster_cfg_t c = onocoy_slot();
    c.tls = true;
    c.port = 2121;
    c.trust_mode = NTRIP_TRUST_CUSTOM_CA;
    c.v2_raw_body = true;
    c.dup_stream_ack = true;
    uint8_t buf[NTRIP_CFG_BLOB_MAX];
    size_t len = ntrip_cfg_encode(&c, buf, sizeof(buf));
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_UINT8(NTRIP_CFG_BLOB_VERSION, buf[0]);
    ntrip_caster_cfg_t d;
    TEST_ASSERT_TRUE(ntrip_cfg_decode(buf, len, &d));
    TEST_ASSERT_TRUE(d.enabled);
    TEST_ASSERT_TRUE(d.protected_slot);
    TEST_ASSERT_TRUE(d.tls);
    TEST_ASSERT_EQUAL_UINT16(2121, d.port);
    TEST_ASSERT_EQUAL_STRING("servers.onocoy.com", d.host);
    TEST_ASSERT_EQUAL_STRING("MP01", d.mountpoint);
    TEST_ASSERT_EQUAL_STRING("console-user", d.username);
    TEST_ASSERT_EQUAL_STRING("pw1", d.password);
    TEST_ASSERT_TRUE(d.password_set);
    TEST_ASSERT_EQUAL_STRING("station", d.label);
    TEST_ASSERT_EQUAL(NTRIP_TRUST_CUSTOM_CA, d.trust_mode);
    TEST_ASSERT_EQUAL(NTRIP_BACKOFF_FAST, d.backoff_profile);
    TEST_ASSERT_TRUE(d.v2_raw_body);
    TEST_ASSERT_TRUE(d.dup_stream_ack);
    TEST_ASSERT_TRUE(ntrip_server_onocoy_ready(&d));

    /* The largest record fits. */
    ntrip_caster_cfg_t big;
    memset(&big, 'x', sizeof(big));
    big.name[sizeof(big.name) - 1] = '\0';
    big.host[sizeof(big.host) - 1] = '\0';
    big.mountpoint[sizeof(big.mountpoint) - 1] = '\0';
    big.username[sizeof(big.username) - 1] = '\0';
    big.password[sizeof(big.password) - 1] = '\0';
    big.label[sizeof(big.label) - 1] = '\0';
    big.proto = NTRIP_PROTO_V2;
    big.trust_mode = NTRIP_TRUST_INSECURE;
    big.backoff_profile = NTRIP_BACKOFF_POLITE;
    TEST_ASSERT_TRUE(ntrip_cfg_encode(&big, buf, sizeof(buf)) > 0);
}

TEST_CASE("caster record: an Onocoy slot without the layout byte is the older layout whatever its username", "[ntrip]")
{
    /* Before the console's three values: the slot was an NTRIP 2.0 caster
     * with username "bob", then switched to Onocoy in place; the editor of
     * that time sent no username for Onocoy, so "bob" stayed stored, and the
     * Onocoy credential username went to `mountpoint`. */
    ntrip_caster_cfg_t c = onocoy_slot();
    strcpy(c.mountpoint, "credential-user");
    strcpy(c.username, "bob");
    uint8_t buf[NTRIP_CFG_BLOB_MAX];
    size_t len = as_old(&c, buf, sizeof(buf));
    ntrip_caster_cfg_t d;
    TEST_ASSERT_TRUE(ntrip_cfg_decode(buf, len, &d));
    TEST_ASSERT_EQUAL_STRING("", d.username);
    TEST_ASSERT_EQUAL_STRING("credential-user", d.mountpoint);
    TEST_ASSERT_EQUAL_STRING("pw1", d.password);
    /* Not set up: the UI asks for the console's values. */
    TEST_ASSERT_FALSE(ntrip_server_onocoy_ready(&d));
    /* It keeps streaming as before the update. */
    ntrip_wire_names_t w;
    ntrip_wire_names(ntrip_slot_is_onocoy(&d), false, d.mountpoint, d.username, d.label, &w);
    TEST_ASSERT_EQUAL_STRING("credential-user", w.mount);           /* 1.0: SOURCE <pw> /credential-user */
    ntrip_wire_names(ntrip_slot_is_onocoy(&d), true, d.mountpoint, d.username, d.label, &w);
    TEST_ASSERT_EQUAL_STRING("station", w.mount);                   /* 2.0: POST /<label> */
    TEST_ASSERT_EQUAL_STRING("credential-user", w.user);

    /* Saved again (the user entered the console's values): the current layout. */
    strcpy(d.mountpoint, "MP01");
    strcpy(d.username, "console-user");
    len = ntrip_cfg_encode(&d, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT8(NTRIP_CFG_LAYOUT, buf[len - 2]);
    TEST_ASSERT_TRUE(ntrip_cfg_decode(buf, len, &d));
    TEST_ASSERT_EQUAL_STRING("console-user", d.username);
    TEST_ASSERT_TRUE(ntrip_server_onocoy_ready(&d));
}

TEST_CASE("caster record: the older layout keeps the username of other casters", "[ntrip]")
{
    ntrip_caster_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.enabled = true;
    strcpy(c.host, "caster.example.com");
    c.port = 2101;
    c.proto = NTRIP_PROTO_V2;
    strcpy(c.mountpoint, "MOUNT");
    strcpy(c.username, "bob");
    strcpy(c.password, "pw");
    c.backoff_profile = NTRIP_BACKOFF_STANDARD;
    uint8_t buf[NTRIP_CFG_BLOB_MAX];
    size_t len = as_old(&c, buf, sizeof(buf));
    ntrip_caster_cfg_t d;
    TEST_ASSERT_TRUE(ntrip_cfg_decode(buf, len, &d));
    TEST_ASSERT_EQUAL_STRING("bob", d.username);
    TEST_ASSERT_EQUAL_STRING("MOUNT", d.mountpoint);
}

TEST_CASE("caster record: unknown versions and damage are refused", "[ntrip]")
{
    ntrip_caster_cfg_t c = onocoy_slot();
    uint8_t buf[NTRIP_CFG_BLOB_MAX];
    size_t len = ntrip_cfg_encode(&c, buf, sizeof(buf));
    ntrip_caster_cfg_t d;
    TEST_ASSERT_FALSE(ntrip_cfg_decode(buf, len - 3u, &d));   /* cut inside a field (before the layout and flags) */
    buf[len] = 0x5A;                                          /* a newer writer's extra field */
    TEST_ASSERT_TRUE(ntrip_cfg_decode(buf, len + 1u, &d));
    buf[0] = 0;
    TEST_ASSERT_FALSE(ntrip_cfg_decode(buf, len, &d));
    buf[0] = NTRIP_CFG_BLOB_VERSION + 1u;                     /* an incompatible record */
    TEST_ASSERT_FALSE(ntrip_cfg_decode(buf, len, &d));
    buf[0] = NTRIP_CFG_BLOB_VERSION;
    buf[3] = 7;                                               /* proto out of range */
    TEST_ASSERT_FALSE(ntrip_cfg_decode(buf, len, &d));
    TEST_ASSERT_EQUAL_size_t(0, ntrip_cfg_encode(&c, buf, 10));
}

TEST_CASE("caster record: the test Onocoy flag in the flags byte; older records read without it", "[ntrip]")
{
    ntrip_caster_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.enabled = true;
    strcpy(c.host, "10.0.2.2");
    c.port = 2101;
    c.proto = NTRIP_PROTO_V1;
    strcpy(c.mountpoint, "TESTMOUNT");
    strcpy(c.username, "testuser01");
    strcpy(c.password, "fakepass123");
    c.backoff_profile = NTRIP_BACKOFF_FAST;
    c.protected_slot = true;
    c.test_onocoy = true;
    uint8_t buf[NTRIP_CFG_BLOB_MAX];
    size_t len = ntrip_cfg_encode(&c, buf, sizeof(buf));
    TEST_ASSERT_TRUE(len > 2);
    TEST_ASSERT_EQUAL_UINT8(NTRIP_CFG_LAYOUT, buf[len - 2]);
    TEST_ASSERT_EQUAL_UINT8(NTRIP_CFG_FLAG_TEST_ONOCOY, buf[len - 1]);
    ntrip_caster_cfg_t d;
    TEST_ASSERT_TRUE(ntrip_cfg_decode(buf, len, &d));
    TEST_ASSERT_TRUE(d.test_onocoy);
    TEST_ASSERT_EQUAL_STRING("testuser01", d.username);

    /* A record from before the flags byte (the layout byte last): no flag. */
    TEST_ASSERT_TRUE(ntrip_cfg_decode(buf, len - 1u, &d));
    TEST_ASSERT_FALSE(d.test_onocoy);
    TEST_ASSERT_EQUAL_STRING("testuser01", d.username);   /* not an Onocoy host: the layout rule leaves it */

    /* Unknown flag bits are ignored. */
    buf[len - 1] = 0xFE;
    TEST_ASSERT_TRUE(ntrip_cfg_decode(buf, len, &d));
    TEST_ASSERT_FALSE(d.test_onocoy);

    /* A plain slot writes the flags byte as 0. */
    c.test_onocoy = false;
    len = ntrip_cfg_encode(&c, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT8(0, buf[len - 1]);

    /* The largest record still fits. */
    ntrip_caster_cfg_t big;
    memset(&big, 'x', sizeof(big));
    big.name[sizeof(big.name) - 1] = '\0';
    big.host[sizeof(big.host) - 1] = '\0';
    big.mountpoint[sizeof(big.mountpoint) - 1] = '\0';
    big.username[sizeof(big.username) - 1] = '\0';
    big.password[sizeof(big.password) - 1] = '\0';
    big.label[sizeof(big.label) - 1] = '\0';
    big.proto = NTRIP_PROTO_V2;
    big.trust_mode = NTRIP_TRUST_INSECURE;
    big.backoff_profile = NTRIP_BACKOFF_POLITE;
    big.test_onocoy = true;
    len = ntrip_cfg_encode(&big, buf, sizeof(buf));
    TEST_ASSERT_TRUE(len > 0 && len <= NTRIP_CFG_BLOB_MAX);
    TEST_ASSERT_TRUE(ntrip_cfg_decode(buf, len, &d));
    TEST_ASSERT_TRUE(d.test_onocoy);
}
