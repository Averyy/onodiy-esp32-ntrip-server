/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The Onocoy preset and the per-slot host rules (protected slot, backoff
 * profile). Pure logic on the public config type (no RTOS calls), so the web
 * API's request parser can use it in the host tests too; which host is which
 * caster stays in ntrip_proto.c.
 */
#include <string.h>

#include "ntrip_proto.h"
#include "ntrip_server.h"

void ntrip_server_onocoy_defaults(ntrip_caster_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    out->enabled = false;  /* enabled once the user enters credentials */
    out->protected_slot = true;
    strlcpy(out->name, "Onocoy", sizeof(out->name));
    strlcpy(out->host, "servers.onocoy.com", sizeof(out->host));
    out->port = 2101;
    out->proto = NTRIP_PROTO_V1;
    out->tls = false;
    out->trust_mode = NTRIP_TRUST_BUNDLE;
    out->backoff_profile = NTRIP_BACKOFF_FAST;
}

bool ntrip_server_onocoy_ready(const ntrip_caster_cfg_t *cfg)
{
    return cfg != NULL && ntrip_host_is_onocoy(cfg->host) && cfg->mountpoint[0] != '\0' && cfg->username[0] != '\0' &&
           (cfg->password_set || cfg->password[0] != '\0');
}

bool ntrip_server_apply_host_rules(ntrip_caster_cfg_t *cfg, const ntrip_backoff_profile_t *requested)
{
    bool onocoy = ntrip_host_is_onocoy(cfg->host);
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
        profile = NTRIP_BACKOFF_STANDARD;      /* the host is no longer Onocoy */
    }
    cfg->protected_slot = onocoy;
    cfg->backoff_profile = profile;
    return true;
}
