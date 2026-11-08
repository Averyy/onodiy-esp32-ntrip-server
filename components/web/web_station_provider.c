/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 * Pure station serialization adapter, shared by REST and WebSocket status.
 */
#include <stdio.h>
#include <stdint.h>
#include "web_shell_provider.h"
#include "web_build.h"
static bool section(void *ctx, const void *snapshot, bool pub, size_t item, wj_t *w, char *key)
{
    (void)item; (void)key;
    web_build_section(w,snapshot,(web_section_t)(uintptr_t)ctx,pub);
    return wj_ok(w);
}
static bool caster(void *ctx, const void *snapshot, bool pub, size_t item, wj_t *w, char *key)
{
    (void)ctx;
    const web_status_snap_t *snap=snapshot;
    if (item >= snap->slots) return true;
    snprintf(key,WEB_SHELL_ITEM_KEY_MAX+1,"%u",(unsigned)item);
    web_build_caster_summary(w,snap,item,pub);
    return wj_ok(w);
}

#define SECTION(name,sec,pub) { .key=name,.visibility=pub,.max_bytes=1280,.max_items=1,.max_item_bytes=1279, \
                               .ctx=(void *)(uintptr_t)sec,.serialize=section }
const web_shell_section_t web_station_sections[]={
    SECTION("device",WEB_SEC_DEVICE,WEB_SHELL_PUBLIC),
    SECTION("safe_mode",WEB_SEC_SAFE_MODE,WEB_SHELL_PUBLIC),
    SECTION("network",WEB_SEC_NETWORK,WEB_SHELL_ADMIN_ONLY),
    SECTION("receiver",WEB_SEC_RECEIVER,WEB_SHELL_PUBLIC),
    {.key="casters",.visibility=WEB_SHELL_PUBLIC,.merge=WEB_SHELL_KEYED_ARRAY,
     .max_bytes=WEB_MAX_SLOTS*897+12,.max_items=WEB_MAX_SLOTS,.max_item_bytes=895,.serialize=caster},
    SECTION("local_caster",WEB_SEC_LOCAL_CASTER,WEB_SHELL_ADMIN_ONLY),
    SECTION("health",WEB_SEC_HEALTH,WEB_SHELL_PUBLIC),
    SECTION("alerts",WEB_SEC_ALERTS,WEB_SHELL_ADMIN_ONLY),
    SECTION("availability",WEB_SEC_AVAILABILITY,WEB_SHELL_ADMIN_ONLY),
    SECTION("update",WEB_SEC_UPDATE,WEB_SHELL_ADMIN_ONLY),
    SECTION("monitor",WEB_SEC_MONITOR,WEB_SHELL_ADMIN_ONLY),
};
const size_t web_station_section_count=sizeof(web_station_sections)/sizeof(web_station_sections[0]);
