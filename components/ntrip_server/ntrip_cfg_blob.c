/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "ntrip_cfg_blob.h"

#include <string.h>

#include "ntrip_proto.h"
#include "settings_codec.h"

#define CFG_LAYOUT_OLD 1u   /* no layout byte: an Onocoy slot's credential username in `mountpoint` */

size_t ntrip_cfg_encode(const ntrip_caster_cfg_t *c, uint8_t *buf, size_t cap)
{
    blob_writer_t w;
    blob_writer_init(&w, buf, cap);
    blob_put_u8(&w, NTRIP_CFG_BLOB_VERSION);
    blob_put_bool(&w, c->enabled);
    blob_put_bool(&w, c->protected_slot);
    blob_put_u8(&w, (uint8_t)c->proto);
    blob_put_bool(&w, c->tls);
    blob_put_u16(&w, c->port);
    blob_put_str(&w, c->name);
    blob_put_str(&w, c->host);
    blob_put_str(&w, c->mountpoint);
    blob_put_str(&w, c->username);
    blob_put_str(&w, c->password);
    blob_put_u8(&w, (uint8_t)c->trust_mode);
    blob_put_u8(&w, (uint8_t)c->backoff_profile);
    blob_put_str(&w, c->label);
    blob_put_bool(&w, c->v2_raw_body);
    blob_put_bool(&w, c->dup_stream_ack);
    blob_put_u8(&w, NTRIP_CFG_LAYOUT);   /* appended later: older records end before it */
    return blob_writer_ok(&w) ? w.len : 0;
}

bool ntrip_cfg_enums_valid(const ntrip_caster_cfg_t *c)
{
    return (c->proto == NTRIP_PROTO_V1 || c->proto == NTRIP_PROTO_V2) && c->trust_mode <= NTRIP_TRUST_INSECURE &&
           c->backoff_profile <= NTRIP_BACKOFF_POLITE;
}

bool ntrip_cfg_decode(const uint8_t *buf, size_t len, ntrip_caster_cfg_t *out)
{
    blob_reader_t r;
    blob_reader_init(&r, buf, len);
    if (blob_get_u8(&r) != NTRIP_CFG_BLOB_VERSION) {
        return false;
    }
    ntrip_caster_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.enabled = blob_get_bool(&r);
    c.protected_slot = blob_get_bool(&r);
    c.proto = (ntrip_proto_t)blob_get_u8(&r);
    c.tls = blob_get_bool(&r);
    c.port = blob_get_u16(&r);
    blob_get_str(&r, c.name, sizeof(c.name));
    blob_get_str(&r, c.host, sizeof(c.host));
    blob_get_str(&r, c.mountpoint, sizeof(c.mountpoint));
    blob_get_str(&r, c.username, sizeof(c.username));
    blob_get_str(&r, c.password, sizeof(c.password));
    c.trust_mode = (ntrip_trust_t)blob_get_u8(&r);
    c.backoff_profile = (ntrip_backoff_profile_t)blob_get_u8(&r);
    blob_get_str(&r, c.label, sizeof(c.label));
    c.v2_raw_body = blob_get_bool(&r);
    c.dup_stream_ack = blob_get_bool(&r);
    uint8_t layout = CFG_LAYOUT_OLD;
    if (blob_reader_ok(&r) && blob_reader_remaining(&r) > 0) {
        layout = blob_get_u8(&r);
    }
    if (!blob_reader_ok(&r) || !ntrip_cfg_enums_valid(&c)) {
        ntrip_secure_zero(&c, sizeof(c));
        return false;
    }
    if (layout < NTRIP_CFG_LAYOUT && ntrip_host_is_onocoy(c.host)) {
        /* The older layout: the credential username is in `mountpoint`; a
         * username stored with it is what a caster this slot was before
         * left behind. */
        memset(c.username, 0, sizeof(c.username));
    }
    c.password_set = c.password[0] != '\0';
    *out = c;
    ntrip_secure_zero(&c, sizeof(c));   /* the password */
    return true;
}
