/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */
#include "web_shell_registry.h"
#include "web_shell_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool key_valid(const char *s)
{
    if (s == NULL || *s == '\0' || strlen(s) > WEB_SHELL_ITEM_KEY_MAX) return false;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
                         (*s >= '0' && *s <= '9') || *s == '_')) return false;
    return true;
}

static bool reserved(const char *s)
{
    static const char *const names[] = { "/ws", "/api/status", "/api/session", "/api/setup/admin", "/api/admin-password" };
    for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); i++) if (!strcmp(s, names[i])) return true;
    return false;
}

static size_t prefix(const char *s)
{
    const char *p = strchr(s, '*');
    return p ? (size_t)(p - s) : strlen(s);
}

static bool overlap(const char *a, const char *b)
{
    size_t na = prefix(a), nb = prefix(b), n = na < nb ? na : nb;
    return !strncmp(a, b, n) && (na == nb || strchr(na < nb ? a : b, '*') != NULL);
}

static bool route_valid(const web_shell_route_rule_t *p)
{
    if (!p->uri || strncmp(p->uri, "/api/", 5) || strlen(p->uri) >= 64 || reserved(p->uri)) return false;
    for (const char *s = p->uri; *s; s++) {
        if ((unsigned char)*s <= 0x20 || (unsigned char)*s >= 0x7f || strchr("?#\\", *s)) return false;
        if (*s == '*' && (s[1] || s == p->uri || s[-1] != '/')) return false;
    }
    /* Prefix wildcards cannot hide reserved routes, even for another method. */
    static const char *const paths[] = { "/api/status", "/api/session", "/api/setup/admin", "/api/admin-password" };
    if (strchr(p->uri, '*')) for (size_t i = 0; i < sizeof(paths)/sizeof(paths[0]); i++)
        if (overlap(p->uri, paths[i])) return false;
    return true;
}

bool web_shell_registry_init(web_shell_registry_t *r, size_t routes, size_t sections, size_t status_bytes)
{
    if (!r) return false;
    memset(r, 0, sizeof(*r));
    if (!routes || routes > WEB_SHELL_ROUTES_MAX || !sections || sections > WEB_SHELL_SECTIONS_MAX ||
        status_bytes < 256 || status_bytes > WEB_SHELL_STATUS_MAX) return false;
    r->route_cap = routes; r->section_cap = sections; r->status_cap = status_bytes;
    return true;
}

bool web_shell_registry_routes(web_shell_registry_t *r, const web_shell_route_rule_t *routes, size_t count)
{
    if (!r || r->sealed) return false;
    if ((!routes && count) || count > r->route_cap - r->route_count) goto fail;
    for (size_t i = 0; i < count; i++) {
        const web_shell_route_rule_t *a = &routes[i];
        if (!route_valid(a)) goto fail;
        for (size_t j = 0; j < r->route_count + i; j++) {
            const web_shell_route_rule_t *b = j < r->route_count ? &r->routes[j] : &routes[j-r->route_count];
            if (!strcmp(a->uri, b->uri)) {
                if (a->method == b->method || a->method == INT_MAX || b->method == INT_MAX) goto fail;
            } else if (overlap(a->uri, b->uri) && (strchr(a->uri, '*') || strchr(b->uri, '*'))) {
                /* Reject ordering-dependent match sets, including cross-method wildcards. */
                goto fail;
            }
        }
    }
    if (count) memcpy(r->routes + r->route_count, routes, count * sizeof(*routes));
    r->route_count += count;
    return true;
fail:
    r->failed = true;
    return false;
}

bool web_shell_registry_sections(web_shell_registry_t *r, const web_shell_section_t *sections, size_t count)
{
    if (!r || r->sealed) return false;
    if ((!sections && count) || count > r->section_cap - r->section_count) goto fail;
    size_t items = 0;
    for (size_t i = 0; i < r->section_count; i++) items += r->sections[i].max_items;
    for (size_t i = 0; i < count; i++) {
        const web_shell_section_t *s = &sections[i];
        if (!key_valid(s->key) || !strcmp(s->key, "api") || !strcmp(s->key, "public") || !strcmp(s->key, "uptime_s") ||
            !s->serialize || !s->max_items || !s->max_item_bytes || s->max_bytes > r->status_cap ||
            s->max_item_bytes > s->max_bytes || s->max_bytes < strlen(s->key)+4 ||
            (unsigned)s->visibility > WEB_SHELL_PUBLIC || (unsigned)s->merge > WEB_SHELL_KEYED_ARRAY ||
            (s->merge == WEB_SHELL_REPLACE && s->max_items != 1)) goto fail;
        if (s->max_items > WEB_SHELL_ITEMS_MAX - items) goto fail;
        items += s->max_items;
        for (size_t j = 0; j < r->section_count + i; j++) {
            const web_shell_section_t *prev = j < r->section_count ? &r->sections[j] : &sections[j-r->section_count];
            if (!strcmp(s->key, prev->key)) goto fail;
        }
    }
    if (count) memcpy(r->sections + r->section_count, sections, count * sizeof(*sections));
    r->section_count += count;
    return true;
fail:
    r->failed = true;
    return false;
}

bool web_shell_registry_status(web_shell_registry_t *r, const web_shell_status_provider_t *p)
{
    if (!r || r->sealed) return false;
    if (!p || !p->capture || !p->release || r->status.capture) { r->failed = true; return false; }
    r->status = *p;
    return true;
}

bool web_shell_registry_seal(web_shell_registry_t *r)
{
    if (!r || r->failed || !r->status.capture) return false;
    r->sealed = true;
    return true;
}

bool web_shell_registry_path(const web_shell_registry_t *r, const char *path)
{
    if (!path) return false;
    if (reserved(path)) return true;
    for (size_t i = 0; i < r->route_count; i++) {
        const web_shell_route_rule_t *rt = &r->routes[i];
        if (!strcmp(rt->uri, path)) return true;
        if (strchr(rt->uri, '*') && !strncmp(rt->uri, path, prefix(rt->uri)) &&
            (!rt->path_known || rt->path_known(rt->ctx, path))) return true;
    }
    return false;
}

bool web_shell_access_valid(web_access_t access)
{
    return access >= WEB_ACC_PUB && access <= WEB_ACC_CLAIM;
}

void web_shell_frame_dispose(web_shell_frame_t *f)
{
    if (f) { free(f->text); memset(f, 0, sizeof(*f)); }
}

bool web_shell_frame_reserve(const web_shell_registry_t *r, web_shell_frame_t *f)
{
    if (f->text && f->cap != r->status_cap) web_shell_frame_dispose(f);   /* web_shell_init() retried with another size */
    if (!f->text) {
        f->text = malloc(r->status_cap);
        f->cap = f->text ? r->status_cap : 0;
    }
    return f->text != NULL;
}

bool web_shell_status_stream(const web_shell_registry_t *r, const void *snapshot,
                             uint32_t uptime_s, bool pub, wj_t *w)
{
    size_t start = w->total;
    wj_obj_open(w,NULL);
    wj_int(w,"api",WEB_SHELL_API_VERSION); wj_bool(w,"public",pub);
    wj_uint(w,"uptime_s",uptime_s);
    for (size_t sec = 0; sec < r->section_count; sec++) {
        const web_shell_section_t *s = &r->sections[sec];
        if (pub && s->visibility != WEB_SHELL_PUBLIC) continue;
        bool keyed = s->merge == WEB_SHELL_KEYED_ARRAY;
        size_t bytes = keyed ? strlen(s->key)+5 : 0;
        size_t entries = 0;
        if (keyed) wj_arr_open(w,s->key);
        for (size_t item = 0; item < s->max_items; item++) {
            char key[WEB_SHELL_ITEM_KEY_MAX + 1] = {0};
            uint8_t depth = w->depth;
            bool comma = w->need_comma[depth];
            size_t before = w->total;
            if (!s->serialize(s->ctx, snapshot, pub, item, w, key) || !wj_ok(w) || w->depth != depth) return false;
            /* The limits a frame holds a fragment to; the separator is not part of it. */
            size_t wrote = w->total - before;
            if (!wrote) {
                if (!keyed) return false;
                continue;
            }
            wrote -= comma ? 1 : 0;
            bytes += wrote + (entries++ ? 1 : 0);
            if (wrote > s->max_item_bytes || bytes > s->max_bytes) return false;
        }
        if (keyed) wj_arr_close(w);
    }
    wj_obj_close(w);
    return wj_ok(w) && w->total - start < r->status_cap;
}

bool web_shell_frame_build(const web_shell_registry_t *r, const void *snapshot,
                           uint32_t uptime_s, bool pub, web_shell_frame_t *f)
{
    if (!web_shell_frame_reserve(r, f)) return false;
    f->built = false; f->len = 0; f->count = 0;
    f->uptime_s = uptime_s; f->public_form = pub;
    for (size_t sec = 0; sec < r->section_count; sec++) {
        const web_shell_section_t *s = &r->sections[sec];
        if (pub && s->visibility != WEB_SHELL_PUBLIC) continue;
        size_t bytes = s->merge == WEB_SHELL_KEYED_ARRAY ? strlen(s->key)+5 : 0;
        size_t entries = 0;
        for (size_t item = 0; item < s->max_items; item++) {
            if (f->len >= r->status_cap || f->count >= WEB_SHELL_ITEMS_MAX) goto fail;
            size_t avail = r->status_cap - f->len;
            if (avail > s->max_item_bytes + 1) avail = s->max_item_bytes + 1;
            wj_t w;
            if (s->merge == WEB_SHELL_REPLACE) wj_init_members(&w, f->text+f->len, avail);
            else wj_init_elements(&w, f->text+f->len, avail);
            char key[WEB_SHELL_ITEM_KEY_MAX + 1] = {0};
            if (!s->serialize(s->ctx, snapshot, pub, item, &w, key) || !wj_ok(&w) || w.depth != 1) goto fail;
            if (!w.len) {
                if (s->merge == WEB_SHELL_REPLACE) goto fail;
                continue;
            }
            if (s->merge == WEB_SHELL_REPLACE) {
                char head[WEB_SHELL_ITEM_KEY_MAX+5];
                snprintf(head, sizeof(head), "\"%s\":", s->key);
                if (strncmp(f->text+f->len, head, strlen(head))) goto fail;
            } else {
                if (!memchr(key, 0, sizeof(key)) || !key[0]) goto fail;
                for (size_t j = 0; j < f->count; j++)
                    if (f->cells[j].section == sec && !strcmp(f->cells[j].key, key)) goto fail;
            }
            bytes += w.len + (entries++ ? 1 : 0);
            if (bytes > s->max_bytes) goto fail;
            web_shell_cell_t *c = &f->cells[f->count++];
            c->section = (unsigned)sec;
            memcpy(c->key, key, sizeof(key));
            c->offset = f->len; c->len = w.len;
            c->crc = web_crc32(0, f->text+f->len, w.len);
            f->len += w.len + 1;
        }
    }
    f->built = true;
    return true;
fail:
    f->len = 0; f->count = 0;
    return false;
}

static const web_shell_cell_t *find_cell(const web_shell_cell_t *c, size_t n, unsigned sec, const char *key)
{
    for (size_t i = 0; i < n; i++) if (c[i].section == sec && !strcmp(c[i].key, key)) return &c[i];
    return NULL;
}

bool web_shell_frame_removed(const web_shell_frame_t *f, const web_shell_baseline_t *b)
{
    if (b && b->valid) for (size_t i = 0; i < b->count; i++)
        if (!find_cell(f->cells, f->count, b->cells[i].section, b->cells[i].key)) return true;
    return false;
}

static bool changed(const web_shell_cell_t *c, const web_shell_baseline_t *b)
{
    const web_shell_cell_t *old = b && b->valid ? find_cell(b->cells,b->count,c->section,c->key) : NULL;
    return !old || old->crc != c->crc || old->len != c->len;
}

size_t web_shell_frame_render(const web_shell_registry_t *r, const web_shell_frame_t *f,
                             const web_shell_baseline_t *b, bool ws, bool full,
                             uint32_t seq, char *out, size_t cap)
{
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!f->built) return 0;
    full |= !b || !b->valid || web_shell_frame_removed(f, b);
    wj_t w;
    wj_init(&w,out,cap,NULL,NULL);
    wj_obj_open(&w,NULL);
    if (ws) {
        wj_str(&w,"type","status"); wj_uint(&w,"seq",seq); wj_bool(&w,"full",full);
        wj_uint(&w,"uptime_s",f->uptime_s); wj_obj_open(&w,"data");
    } else {
        wj_int(&w,"api",WEB_SHELL_API_VERSION); wj_bool(&w,"public",f->public_form);
        wj_uint(&w,"uptime_s",f->uptime_s);
    }
    for (size_t sec = 0; sec < r->section_count; sec++) {
        const web_shell_section_t *s = &r->sections[sec];
        if (f->public_form && s->visibility != WEB_SHELL_PUBLIC) continue;
        bool any = full;
        for (size_t j = 0; j < f->count; j++) if (f->cells[j].section == sec && changed(&f->cells[j],b)) any = true;
        if (!any) continue;
        if (s->merge == WEB_SHELL_KEYED_ARRAY) wj_arr_open(&w,s->key);
        for (size_t j = 0; j < f->count; j++) {
            const web_shell_cell_t *c = &f->cells[j];
            if (c->section != sec || (!full && !changed(c,b))) continue;
            if (s->merge == WEB_SHELL_REPLACE) {
                /* Fragment already contains its key. Feed the value through
                 * the writer so separators and nesting remain shared. */
                size_t keylen = strlen(s->key)+3;
                wj_raw(&w,s->key,f->text+c->offset+keylen,c->len-keylen);
            } else wj_raw(&w,NULL,f->text+c->offset,c->len);
        }
        if (s->merge == WEB_SHELL_KEYED_ARRAY) wj_arr_close(&w);
    }
    if (ws) wj_obj_close(&w);
    wj_obj_close(&w);
    if (!wj_ok(&w)) { out[0] = 0; return 0; }
    return w.len;
}

void web_shell_frame_commit(const web_shell_frame_t *f, web_shell_baseline_t *b)
{
    if (!f->built) return;
    b->count = f->count; b->valid = true;
    memcpy(b->cells,f->cells,f->count*sizeof(f->cells[0]));
}

bool web_shell_owner_submit(const web_shell_owner_ops_t *o, void *request)
{
    void *copy = o->acquire(o->ctx,request);
    if (!copy) return false;
    o->deadline(o->ctx,copy,true);
    if (o->enqueue(o->ctx,copy)) return true;
    o->deadline(o->ctx,copy,false);
    o->complete(o->ctx,copy);
    return false;
}

void web_shell_owner_finish(const web_shell_owner_ops_t *o, void **owned, bool close)
{
    if (!owned || !*owned) return;
    void *copy = *owned; *owned = NULL;
    if (close) o->close(o->ctx,copy);
    o->deadline(o->ctx,copy,false);
    o->complete(o->ctx,copy);
}

bool web_shell_origin_valid(const char *s)
{
    if (!s || strncmp(s,"https://",8) || !s[8] || strlen(s) > 120) return false;
    s += 8;
    bool label = false;
    char prev = 0;
    for (; *s; prev = *s++) {
        if (*s == '.') { if (!label || prev == '-') return false; label = false; }
        else if ((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || (*s == '-' && label)) label = true;
        else return false;
    }
    return label && prev != '-';
}

bool web_shell_csp(char *out, size_t cap, const char *host,
                   const char *const *img, size_t ni, const char *const *connect, size_t nc)
{
    if (!out || !cap) return false;
    out[0] = 0;
    if (!host || ni > 4 || nc > 4 || (ni && !img) || (nc && !connect)) return false;
    /* The Host check accepts blanks around the name; they are not part of it. */
    while (*host == ' ' || *host == '\t') host++;
    size_t hlen = strlen(host);
    while (hlen && (host[hlen-1] == ' ' || host[hlen-1] == '\t')) hlen--;
    if (!hlen) return false;
    for (size_t i=0;i<hlen;i++) if (strchr(" ;'\"\\\r\n\t",host[i])) return false;
    for (size_t i=0;i<ni;i++) if (!web_shell_origin_valid(img[i])) return false;
    for (size_t i=0;i<nc;i++) if (!web_shell_origin_valid(connect[i])) return false;
    char images[4*122+1] = "", connections[4*122+1] = "";
    for (size_t i=0;i<ni;i++) { strcat(images," "); strcat(images,img[i]); }
    for (size_t i=0;i<nc;i++) { strcat(connections," "); strcat(connections,connect[i]); }
    int n=snprintf(out,cap,"default-src 'self'; script-src 'self'; worker-src 'self'; style-src 'self'; img-src 'self' data: blob:%s; "
        "font-src 'self'; connect-src 'self'%s ws://%.*s; object-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'",
        images,connections,(int)hlen,host);
    if (n < 0 || (size_t)n >= cap) { out[0]=0; return false; }
    return true;
}
