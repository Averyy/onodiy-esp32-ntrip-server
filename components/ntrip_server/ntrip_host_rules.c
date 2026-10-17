/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The Onocoy preset and the per-slot host rules (protected slot, backoff
 * profile, which slots count as Onocoy, the RTK2go mountpoint rules at save
 * time). Pure logic on the public config type (no RTOS calls), so the web
 * API's request parser can use it in the host tests too; which host is which
 * caster stays in ntrip_proto.c.
 */
#include <string.h>

#include "ntrip_proto.h"
#include "ntrip_server.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_LINUX
/* The host tests switch the build option (ntrip_test_onocoy_set_allowed). */
static bool s_test_onocoy_allowed;

void ntrip_test_onocoy_set_allowed(bool allowed)
{
    s_test_onocoy_allowed = allowed;
}

bool ntrip_test_onocoy_allowed(void)
{
    return s_test_onocoy_allowed;
}
#else
bool ntrip_test_onocoy_allowed(void)
{
#if CONFIG_NTRIP_TEST_ONOCOY_SLOT
    return true;   /* bench or QEMU build (the option depends on one of them) */
#else
    return false;
#endif
}
#endif

bool ntrip_slot_is_onocoy(const ntrip_caster_cfg_t *cfg)
{
    return cfg != NULL && (ntrip_host_is_onocoy(cfg->host) || (cfg->test_onocoy && ntrip_test_onocoy_allowed()));
}

bool ntrip_slot_is_open_caster(const ntrip_caster_cfg_t *cfg)
{
    return cfg != NULL && !ntrip_slot_is_onocoy(cfg) && ntrip_host_is_open_caster(cfg->host);
}

bool ntrip_slot_is_rtk2go(const ntrip_caster_cfg_t *cfg)
{
    return cfg != NULL && ntrip_host_is_rtk2go(cfg->host);
}

uint32_t ntrip_server_open_caster_pair(const ntrip_caster_cfg_t *cfgs, size_t n, uint32_t *open_mask)
{
    uint32_t onocoy = 0;
    uint32_t open = 0;
    for (size_t i = 0; cfgs != NULL && i < n && i < 32; i++) {
        if (!cfgs[i].enabled) {
            continue;
        }
        if (ntrip_slot_is_onocoy(&cfgs[i])) {
            onocoy |= 1u << i;
        } else if (ntrip_slot_is_open_caster(&cfgs[i])) {
            open |= 1u << i;
        }
    }
    if (onocoy == 0 || open == 0) {
        onocoy = 0;
        open = 0;
    }
    if (open_mask != NULL) {
        *open_mask = open;
    }
    return onocoy;
}

void ntrip_server_onocoy_defaults(ntrip_caster_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    out->enabled = false;  /* enabled once the user enters credentials */
    out->protected_slot = true;
    strlcpy(out->name, "Onocoy", sizeof(out->name));
    strlcpy(out->host, "servers.onocoy.com", sizeof(out->host));
    out->port = 2101;
    out->proto = NTRIP_PROTO_V2;   /* Onocoy's guide: POST /<mountpoint>, Basic <username>:<password> */
    out->tls = false;
    out->trust_mode = NTRIP_TRUST_BUNDLE;
    out->backoff_profile = NTRIP_BACKOFF_FAST;
}

bool ntrip_server_onocoy_ready(const ntrip_caster_cfg_t *cfg)
{
    return cfg != NULL && ntrip_slot_is_onocoy(cfg) && cfg->mountpoint[0] != '\0' && cfg->username[0] != '\0' &&
           (cfg->password_set || cfg->password[0] != '\0');
}

bool ntrip_server_onocoy_blank_proto(ntrip_caster_cfg_t *cfg)
{
    if (cfg == NULL || !ntrip_slot_is_onocoy(cfg) || cfg->mountpoint[0] != '\0' || cfg->username[0] != '\0' ||
        cfg->password[0] != '\0' || cfg->password_set) {
        return false;   /* not Onocoy, or holds a credential: set up, or streaming in the older layout */
    }
    ntrip_caster_cfg_t preset;
    ntrip_server_onocoy_defaults(&preset);
    if (cfg->proto == preset.proto) {
        return false;
    }
    cfg->proto = preset.proto;
    return true;
}

bool ntrip_server_apply_host_rules(ntrip_caster_cfg_t *cfg, const ntrip_backoff_profile_t *requested)
{
    bool onocoy = ntrip_slot_is_onocoy(cfg);
    ntrip_backoff_profile_t profile = cfg->backoff_profile;
    if (onocoy) {
        profile = NTRIP_BACKOFF_FAST;          /* the daily budget is tight */
    } else if (ntrip_host_is_rtk2go(cfg->host)) {
        profile = NTRIP_BACKOFF_POLITE;        /* bans sources that hammer it */
    } else if (requested != NULL) {
        if (*requested == NTRIP_BACKOFF_FAST) {
            return false;                      /* fast is for Onocoy only */
        }
        profile = *requested;
    } else if (profile == NTRIP_BACKOFF_FAST) {
        profile = NTRIP_BACKOFF_STANDARD;      /* the slot no longer counts as Onocoy */
    }
    cfg->protected_slot = onocoy;
    cfg->backoff_profile = profile;
    if (!ntrip_test_onocoy_allowed()) {
        cfg->test_onocoy = false;              /* a mark this build ignores is not kept */
    }
    return true;
}

ntrip_rtk2go_mount_t ntrip_server_rtk2go_check(const char *old_host, const char *old_mount, const char *new_host,
                                               const char *new_mount)
{
    if (new_mount == NULL || new_mount[0] == '\0' ||
        !ntrip_rtk2go_rule_applies(old_host, old_mount, new_host, new_mount)) {
        return NTRIP_RTK2GO_OK;
    }
    return ntrip_rtk2go_mount_check(new_mount);
}
