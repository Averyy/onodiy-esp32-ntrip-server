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
