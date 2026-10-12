/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "ntrip_server.h"
#include "unity.h"

static ntrip_caster_cfg_t slot(const char *host, ntrip_backoff_profile_t profile, bool protected_slot)
{
    ntrip_caster_cfg_t c;
    memset(&c, 0, sizeof(c));
    strcpy(c.host, host);
    c.backoff_profile = profile;
    c.protected_slot = protected_slot;
    return c;
}

TEST_CASE("caster host rules: protected exactly for Onocoy, fast forced there", "[ntrip]")
{
    const ntrip_backoff_profile_t polite = NTRIP_BACKOFF_POLITE;
    const ntrip_backoff_profile_t fast = NTRIP_BACKOFF_FAST;

    ntrip_caster_cfg_t c = slot("servers.onocoy.com", NTRIP_BACKOFF_STANDARD, false);
    TEST_ASSERT_TRUE(ntrip_server_apply_host_rules(&c, &polite));   /* a request is ignored there */
    TEST_ASSERT_TRUE(c.protected_slot);
    TEST_ASSERT_EQUAL(NTRIP_BACKOFF_FAST, c.backoff_profile);
    c = slot("ONOCOY.COM", NTRIP_BACKOFF_POLITE, false);
    TEST_ASSERT_TRUE(ntrip_server_apply_host_rules(&c, NULL));
    TEST_ASSERT_TRUE(c.protected_slot);
    TEST_ASSERT_EQUAL(NTRIP_BACKOFF_FAST, c.backoff_profile);

    /* A look-alike host is not Onocoy: never protected, never fast. */
    c = slot("onocoy.com.example.net", NTRIP_BACKOFF_FAST, true);
    TEST_ASSERT_TRUE(ntrip_server_apply_host_rules(&c, NULL));
    TEST_ASSERT_FALSE(c.protected_slot);
    TEST_ASSERT_EQUAL(NTRIP_BACKOFF_STANDARD, c.backoff_profile);   /* left from Onocoy */
    c = slot("notonocoy.com", NTRIP_BACKOFF_STANDARD, false);
    TEST_ASSERT_FALSE(ntrip_server_apply_host_rules(&c, &fast));   /* fast is for Onocoy only */
    TEST_ASSERT_EQUAL(NTRIP_BACKOFF_STANDARD, c.backoff_profile);   /* unchanged */
}

TEST_CASE("caster host rules: polite forced for RTK2go, requested elsewhere", "[ntrip]")
{
    const ntrip_backoff_profile_t standard = NTRIP_BACKOFF_STANDARD;
    const ntrip_backoff_profile_t polite = NTRIP_BACKOFF_POLITE;
    const ntrip_backoff_profile_t fast = NTRIP_BACKOFF_FAST;

    ntrip_caster_cfg_t c = slot("rtk2go.com", NTRIP_BACKOFF_FAST, true);
    TEST_ASSERT_TRUE(ntrip_server_apply_host_rules(&c, &fast));    /* forced, not refused */
    TEST_ASSERT_FALSE(c.protected_slot);
    TEST_ASSERT_EQUAL(NTRIP_BACKOFF_POLITE, c.backoff_profile);
    c = slot("RTK2go.com", NTRIP_BACKOFF_STANDARD, false);
    TEST_ASSERT_TRUE(ntrip_server_apply_host_rules(&c, &standard));
    TEST_ASSERT_EQUAL(NTRIP_BACKOFF_POLITE, c.backoff_profile);

    c = slot("caster.example.org", NTRIP_BACKOFF_STANDARD, false);
    TEST_ASSERT_TRUE(ntrip_server_apply_host_rules(&c, &polite));
    TEST_ASSERT_EQUAL(NTRIP_BACKOFF_POLITE, c.backoff_profile);
    TEST_ASSERT_TRUE(ntrip_server_apply_host_rules(&c, NULL));     /* kept */
    TEST_ASSERT_EQUAL(NTRIP_BACKOFF_POLITE, c.backoff_profile);
    TEST_ASSERT_TRUE(ntrip_server_apply_host_rules(&c, &standard));
    TEST_ASSERT_EQUAL(NTRIP_BACKOFF_STANDARD, c.backoff_profile);
    /* Open caster other than RTK2go: no forced profile. */
    c = slot("crtk.net", NTRIP_BACKOFF_STANDARD, false);
    TEST_ASSERT_TRUE(ntrip_server_apply_host_rules(&c, NULL));
    TEST_ASSERT_EQUAL(NTRIP_BACKOFF_STANDARD, c.backoff_profile);

    /* Applying the rules again changes nothing (set_config re-applies them). */
    ntrip_server_onocoy_defaults(&c);
    ntrip_caster_cfg_t again = c;
    TEST_ASSERT_TRUE(ntrip_server_apply_host_rules(&again, NULL));
    TEST_ASSERT_EQUAL_MEMORY(&c, &again, sizeof(c));
}

/* A slot pointed at a local test caster (the Mac as QEMU sees it). */
static ntrip_caster_cfg_t test_slot(void)
{
    ntrip_caster_cfg_t c = slot("10.0.2.2", NTRIP_BACKOFF_STANDARD, false);
    c.enabled = true;
    c.port = 2101;
    c.proto = NTRIP_PROTO_V1;
    strcpy(c.mountpoint, "TESTMOUNT");
    strcpy(c.username, "testuser01");
    strcpy(c.password, "fakepass123");
    c.test_onocoy = true;
    return c;
}

TEST_CASE("caster host rules: a test Onocoy slot counts as Onocoy only where the build allows it", "[ntrip]")
{
    const ntrip_backoff_profile_t fast = NTRIP_BACKOFF_FAST;

    ntrip_test_onocoy_set_allowed(true);
    ntrip_caster_cfg_t c = test_slot();
    TEST_ASSERT_TRUE(ntrip_slot_is_onocoy(&c));
    TEST_ASSERT_TRUE(ntrip_server_apply_host_rules(&c, NULL));
    TEST_ASSERT_TRUE(c.protected_slot);
    TEST_ASSERT_EQUAL(NTRIP_BACKOFF_FAST, c.backoff_profile);
    TEST_ASSERT_TRUE(c.test_onocoy);
    TEST_ASSERT_TRUE(ntrip_server_onocoy_ready(&c));
    /* The same host without the mark: fast is still refused there. */
    ntrip_caster_cfg_t plain = test_slot();
    plain.test_onocoy = false;
    TEST_ASSERT_FALSE(ntrip_slot_is_onocoy(&plain));
    TEST_ASSERT_FALSE(ntrip_server_apply_host_rules(&plain, &fast));
    TEST_ASSERT_FALSE(ntrip_server_onocoy_ready(&plain));

    /* A build without the option: the mark is ignored and dropped, the host decides. */
    ntrip_test_onocoy_set_allowed(false);
    c = test_slot();
    c.protected_slot = true;
    c.backoff_profile = NTRIP_BACKOFF_FAST;   /* as a bench build stored it */
    TEST_ASSERT_FALSE(ntrip_slot_is_onocoy(&c));
    TEST_ASSERT_FALSE(ntrip_server_onocoy_ready(&c));
    TEST_ASSERT_TRUE(ntrip_server_apply_host_rules(&c, NULL));
    TEST_ASSERT_FALSE(c.protected_slot);
    TEST_ASSERT_EQUAL(NTRIP_BACKOFF_STANDARD, c.backoff_profile);
    TEST_ASSERT_FALSE(c.test_onocoy);
    /* A real Onocoy host does not need the mark. */
    ntrip_server_onocoy_defaults(&c);
    TEST_ASSERT_TRUE(ntrip_slot_is_onocoy(&c));
}

TEST_CASE("caster host rules: RTK2go names checked when saved with a new host or mountpoint", "[ntrip]")
{
    TEST_ASSERT_EQUAL(NTRIP_RTK2GO_BAD_CHARS, ntrip_server_rtk2go_check("", "", "rtk2go.com", "My.Base"));
    TEST_ASSERT_EQUAL(NTRIP_RTK2GO_FIRST_A,
                      ntrip_server_rtk2go_check("caster.example.org", "Abase", "rtk2go.com", "Abase"));
    /* Stored before the rules: kept as long as host and name stay. */
    TEST_ASSERT_EQUAL(NTRIP_RTK2GO_OK, ntrip_server_rtk2go_check("rtk2go.com", "A.base", "rtk2go.com", "A.base"));
    TEST_ASSERT_EQUAL(NTRIP_RTK2GO_FIRST_A, ntrip_server_rtk2go_check("rtk2go.com", "A.base", "rtk2go.com", "Abase"));
    TEST_ASSERT_EQUAL(NTRIP_RTK2GO_OK, ntrip_server_rtk2go_check("rtk2go.com", "A.base", "rtk2go.com", "Bench01"));
    /* Other casters, and an empty name (the field rules ask for it): no RTK2go rule. */
    TEST_ASSERT_EQUAL(NTRIP_RTK2GO_OK, ntrip_server_rtk2go_check("", "", "caster.example.org", "A.b"));
    TEST_ASSERT_EQUAL(NTRIP_RTK2GO_OK, ntrip_server_rtk2go_check("", "", "rtk2go.com", ""));
}
