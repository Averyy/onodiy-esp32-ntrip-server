/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "web_shell_registry.h"
#include "web_shell_service.h"
#include "web_shell_assets.h"
#include "web_build.h"

static bool capture(void *ctx, const void **snapshot, uint32_t *uptime)
{
    *snapshot=ctx; *uptime=42; return true;
}
static void release(void *ctx, const void *snapshot) { (void)ctx; (void)snapshot; }
static void init(web_shell_registry_t *r, size_t routes, size_t sections)
{
    TEST_ASSERT_TRUE(web_shell_registry_init(r,routes,sections,WEB_SHELL_STATUS_MAX));
    web_shell_status_provider_t p={.capture=capture,.release=release};
    TEST_ASSERT_TRUE(web_shell_registry_status(r,&p));
}
TEST_CASE("web provider: a route that does not state its access level is refused", "[web_provider]")
{
    TEST_ASSERT_FALSE(web_shell_access_valid((web_access_t)0));
    TEST_ASSERT_FALSE(web_shell_access_valid(WEB_ACC_UNSET));
    TEST_ASSERT_FALSE(web_shell_access_valid((web_access_t)(WEB_ACC_CLAIM + 1)));
    TEST_ASSERT_FALSE(web_shell_access_valid((web_access_t)-1));
    TEST_ASSERT_TRUE(web_shell_access_valid(WEB_ACC_PUB));
    TEST_ASSERT_TRUE(web_shell_access_valid(WEB_ACC_PUBSTAR));
    TEST_ASSERT_TRUE(web_shell_access_valid(WEB_ACC_ADMIN));
    TEST_ASSERT_TRUE(web_shell_access_valid(WEB_ACC_CLAIM));
}

static bool concrete(void *ctx, const char *path) { (void)ctx; return !strcmp(path,"/api/items/1"); }

TEST_CASE("web provider: bounded registry rejects duplicate reserved and ambiguous routes atomically", "[web_provider]")
{
    web_shell_registry_t r;
    TEST_ASSERT_FALSE(web_shell_registry_init(&r,WEB_SHELL_ROUTES_MAX+1,1,1024));
    TEST_ASSERT_FALSE(web_shell_registry_init(&r,1,WEB_SHELL_SECTIONS_MAX+1,1024));
    init(&r,2,1);
    const web_shell_route_rule_t routes[]={ {.uri="/api/items/*",.method=0,.path_known=concrete},
                                         {.uri="/api/items/*",.method=1,.path_known=concrete} };
    TEST_ASSERT_TRUE(web_shell_registry_routes(&r,routes,2));
    TEST_ASSERT_TRUE(web_shell_registry_path(&r,"/api/items/1"));
    TEST_ASSERT_FALSE(web_shell_registry_path(&r,"/api/items/nope"));
    TEST_ASSERT_TRUE(web_shell_registry_path(&r,"/api/status"));
    TEST_ASSERT_FALSE(web_shell_registry_routes(&r,routes,1));
    TEST_ASSERT_EQUAL_UINT(2,r.route_count);
    TEST_ASSERT_FALSE(web_shell_registry_seal(&r));
    init(&r,3,1);
    TEST_ASSERT_TRUE(web_shell_registry_routes(&r,routes,1));
    web_shell_route_rule_t overlap={.uri="/api/items/1",.method=2};
    TEST_ASSERT_FALSE(web_shell_registry_routes(&r,&overlap,1));
    TEST_ASSERT_EQUAL_UINT(1,r.route_count);
    static const char *const reserved[]={"/ws","/*","/api/status","/api/session","/api/setup/admin","/api/admin-password","/api/*"};
    for (size_t i=0;i<sizeof(reserved)/sizeof(reserved[0]);i++) {
        init(&r,3,1); web_shell_route_rule_t rt={.uri=reserved[i],.method=0};
        TEST_ASSERT_FALSE(web_shell_registry_routes(&r,&rt,1)); TEST_ASSERT_EQUAL_UINT(0,r.route_count);
    }
    init(&r,3,1);
    web_shell_route_rule_t batch[]={ {.uri="/api/a",.method=0},{.uri="/api/a",.method=0} };
    TEST_ASSERT_FALSE(web_shell_registry_routes(&r,batch,2)); TEST_ASSERT_EQUAL_UINT(0,r.route_count);
    init(&r,3,1); batch[1].method=INT_MAX;
    TEST_ASSERT_FALSE(web_shell_registry_routes(&r,batch,2));
    init(&r,3,1); batch[1].method=1;
    TEST_ASSERT_TRUE(web_shell_registry_routes(&r,batch,2));
    TEST_ASSERT_TRUE(web_shell_registry_seal(&r));
    TEST_ASSERT_FALSE(web_shell_registry_routes(&r,NULL,0));
    init(&r,1,1);
    web_shell_route_rule_t generic={.uri="/api/generic/*",.method=0};
    TEST_ASSERT_TRUE(web_shell_registry_routes(&r,&generic,1));
    TEST_ASSERT_TRUE(web_shell_registry_path(&r,"/api/generic/anything"));
}

typedef struct { unsigned value, items; bool fail, big, duplicate; } sample_t;
static bool value(void *ctx, const void *snapshot, bool pub, size_t item, wj_t *w, char *key)
{
    (void)pub; (void)item; (void)key;
    const sample_t *s=snapshot;
    wj_obj_open(w,ctx); wj_uint(w,"value",s->value);
    if (s->big) wj_str(w,"extra","a long value that exceeds the declared bound for this section");
    wj_obj_close(w);
    return !s->fail;
}
static bool items(void *ctx, const void *snapshot, bool pub, size_t item, wj_t *w, char *key)
{
    (void)ctx; (void)pub;
    const sample_t *s=snapshot;
    if (!(s->items & (1u<<item))) return true;
    key[0]=s->duplicate ? 'a' : (char)('a'+item); key[1]=0;
    wj_obj_open(w,NULL); wj_str(w,"id",key); wj_uint(w,"value",s->value+item); wj_obj_close(w);
    return !s->fail;
}
static const web_shell_section_t sections[]={
    {.key="product",.visibility=WEB_SHELL_PUBLIC,.max_bytes=64,.max_items=1,.max_item_bytes=64,.ctx="product",.serialize=value},
    {.key="private",.max_bytes=64,.max_items=1,.max_item_bytes=64,.ctx="private",.serialize=value},
    {.key="items",.visibility=WEB_SHELL_PUBLIC,.merge=WEB_SHELL_KEYED_ARRAY,.max_bytes=80,.max_items=2,.max_item_bytes=32,.serialize=items},
};

TEST_CASE("web provider: public opt-in section collisions and item bounds", "[web_provider]")
{
    web_shell_registry_t r; init(&r,1,3);
    TEST_ASSERT_TRUE(web_shell_registry_sections(&r,sections,3));
    TEST_ASSERT_FALSE(web_shell_registry_sections(&r,sections,1));
    TEST_ASSERT_EQUAL_UINT(3,r.section_count);
    init(&r,1,3);
    web_shell_section_t dup[]={sections[0],sections[0]};
    TEST_ASSERT_FALSE(web_shell_registry_sections(&r,dup,2)); TEST_ASSERT_EQUAL_UINT(0,r.section_count);
    const char *reserved[]={"api","public","uptime_s"};
    for (size_t i=0;i<3;i++) {
        init(&r,1,3); dup[0]=sections[0]; dup[0].key=reserved[i];
        TEST_ASSERT_FALSE(web_shell_registry_sections(&r,dup,1));
    }
    init(&r,1,3); dup[0]=sections[0]; dup[0].max_items=WEB_SHELL_ITEMS_MAX+1;
    TEST_ASSERT_FALSE(web_shell_registry_sections(&r,dup,1));
    init(&r,1,3); TEST_ASSERT_TRUE(web_shell_registry_sections(&r,sections,3));
    sample_t s={.value=7,.items=3}; web_shell_frame_t f={0}; char text[512];
    TEST_ASSERT_TRUE(web_shell_frame_build(&r,&s,42,true,&f));
    TEST_ASSERT_GREATER_THAN(0,web_shell_frame_render(&r,&f,NULL,false,true,0,text,sizeof(text)));
    TEST_ASSERT_NOT_NULL(strstr(text,"\"api\":1,\"public\":true,\"uptime_s\":42"));
    TEST_ASSERT_NOT_NULL(strstr(text,"\"product\"")); TEST_ASSERT_NULL(strstr(text,"\"private\""));
    web_shell_frame_dispose(&f);
    TEST_ASSERT_TRUE(web_shell_frame_build(&r,&s,42,false,&f));
    TEST_ASSERT_GREATER_THAN(0,web_shell_frame_render(&r,&f,NULL,false,true,0,text,sizeof(text)));
    TEST_ASSERT_NOT_NULL(strstr(text,"\"private\"")); web_shell_frame_dispose(&f);
}

TEST_CASE("web provider: overflow and serializer failure emit nothing and do not advance baseline", "[web_provider]")
{
    web_shell_registry_t r; init(&r,1,3); TEST_ASSERT_TRUE(web_shell_registry_sections(&r,sections,3));
    sample_t s={.value=1,.items=3}; web_shell_frame_t f={0}; web_shell_baseline_t b={0};
    TEST_ASSERT_TRUE(web_shell_frame_build(&r,&s,42,false,&f)); web_shell_frame_commit(&f,&b);
    web_shell_baseline_t old=b;
    s.value=2; s.fail=true;
    TEST_ASSERT_FALSE(web_shell_frame_build(&r,&s,43,false,&f)); TEST_ASSERT_FALSE(f.built);
    char out[512]="partial";
    TEST_ASSERT_EQUAL_UINT(0,web_shell_frame_render(&r,&f,&b,true,false,2,out,sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("",out); TEST_ASSERT_EQUAL_MEMORY(&old,&b,sizeof(b));
    s.fail=false; s.big=true;
    TEST_ASSERT_FALSE(web_shell_frame_build(&r,&s,43,false,&f)); TEST_ASSERT_FALSE(f.built);
    TEST_ASSERT_EQUAL_MEMORY(&old,&b,sizeof(b));
    s.big=false; TEST_ASSERT_TRUE(web_shell_frame_build(&r,&s,44,false,&f));
    TEST_ASSERT_EQUAL_UINT(0,web_shell_frame_render(&r,&f,&b,true,false,2,out,12));
    TEST_ASSERT_EQUAL_STRING("",out); TEST_ASSERT_EQUAL_MEMORY(&old,&b,sizeof(b));
    TEST_ASSERT_GREATER_THAN(0,web_shell_frame_render(&r,&f,&b,true,false,2,out,sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out,"\"value\":2")); web_shell_frame_dispose(&f);
    /* Collection total bound is distinct from the per-item bound. */
    init(&r,1,1); web_shell_section_t d=sections[2]; d.max_bytes=33;
    TEST_ASSERT_TRUE(web_shell_registry_sections(&r,&d,1));
    TEST_ASSERT_FALSE(web_shell_frame_build(&r,&s,45,false,&f)); TEST_ASSERT_FALSE(f.built);
    init(&r,1,1); TEST_ASSERT_TRUE(web_shell_registry_sections(&r,&sections[2],1));
    s.duplicate=true;
    TEST_ASSERT_FALSE(web_shell_frame_build(&r,&s,46,false,&f)); TEST_ASSERT_FALSE(f.built);
    TEST_ASSERT_EQUAL_MEMORY(&old,&b,sizeof(b));
}

TEST_CASE("web provider: keyed deltas contain changed items and deletion forces full resync", "[web_provider]")
{
    web_shell_registry_t r; init(&r,1,1); TEST_ASSERT_TRUE(web_shell_registry_sections(&r,&sections[2],1));
    sample_t s={.value=1,.items=1}; web_shell_frame_t f={0}; web_shell_baseline_t b={0}; char out[512];
    TEST_ASSERT_TRUE(web_shell_frame_build(&r,&s,42,true,&f)); web_shell_frame_commit(&f,&b);
    s.items=3;
    TEST_ASSERT_TRUE(web_shell_frame_build(&r,&s,43,true,&f));
    TEST_ASSERT_FALSE(web_shell_frame_removed(&f,&b));
    TEST_ASSERT_GREATER_THAN(0,web_shell_frame_render(&r,&f,&b,true,false,2,out,sizeof(out)));
    TEST_ASSERT_NULL(strstr(out,"\"id\":\"a\"")); TEST_ASSERT_NOT_NULL(strstr(out,"\"id\":\"b\""));
    web_shell_frame_commit(&f,&b); s.items=0;
    TEST_ASSERT_TRUE(web_shell_frame_build(&r,&s,44,true,&f));
    TEST_ASSERT_TRUE(web_shell_frame_removed(&f,&b));
    TEST_ASSERT_GREATER_THAN(0,web_shell_frame_render(&r,&f,&b,true,false,3,out,sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out,"\"full\":true")); TEST_ASSERT_NOT_NULL(strstr(out,"\"items\":[]"));
    web_shell_frame_dispose(&f);
}

typedef struct { char *out; size_t cap, len; } sink_t;
static bool sink_flush(void *ctx, const char *data, size_t len)
{
    sink_t *s=ctx;
    if (s->len+len >= s->cap) return false;
    memcpy(s->out+s->len,data,len); s->len+=len;
    return true;
}

TEST_CASE("web provider: station REST composition preserves both legacy forms and caster removal", "[web_provider]")
{
    web_shell_registry_t r; init(&r,1,WEB_SHELL_SECTIONS_MAX);
    TEST_ASSERT_TRUE(web_shell_registry_sections(&r,web_station_sections,web_station_section_count));
    web_status_snap_t *s=calloc(1,sizeof(*s)); TEST_ASSERT_NOT_NULL(s);
    s->uptime_s=42; s->slots=1; s->cfg_valid=true; strcpy(s->cfg[0].host,"example.org");
    char *expected=malloc(WEB_SHELL_STATUS_MAX),*actual=malloc(WEB_SHELL_STATUS_MAX);
    TEST_ASSERT_NOT_NULL(expected); TEST_ASSERT_NOT_NULL(actual);
    web_shell_frame_t f={0}; web_shell_baseline_t b={0};
    for (unsigned pub=0;pub<2;pub++) {
        wj_t w; wj_init(&w,expected,WEB_SHELL_STATUS_MAX,NULL,NULL); web_build_status(&w,s,pub);
        TEST_ASSERT_TRUE(wj_ok(&w));
        TEST_ASSERT_TRUE(web_shell_frame_build(&r,s,s->uptime_s,pub,&f));
        TEST_ASSERT_GREATER_THAN(0,web_shell_frame_render(&r,&f,NULL,false,true,0,actual,WEB_SHELL_STATUS_MAX));
        TEST_ASSERT_EQUAL_STRING(expected,actual);
        /* What GET /api/status sends: the same bytes through a writer of the response buffer's size. */
        sink_t sink={actual,WEB_SHELL_STATUS_MAX,0}; char small[2048];
        wj_init(&w,small,sizeof(small),sink_flush,&sink);
        TEST_ASSERT_TRUE(web_shell_status_stream(&r,s,s->uptime_s,pub,&w)); TEST_ASSERT_TRUE(wj_finish(&w));
        actual[sink.len]=0;
        TEST_ASSERT_EQUAL_STRING(expected,actual);
    }
    web_shell_frame_commit(&f,&b); s->cfg[0].host[0]=0;
    TEST_ASSERT_TRUE(web_shell_frame_build(&r,s,43,true,&f)); TEST_ASSERT_TRUE(web_shell_frame_removed(&f,&b));
    TEST_ASSERT_GREATER_THAN(0,web_shell_frame_render(&r,&f,&b,true,false,2,actual,WEB_SHELL_STATUS_MAX));
    TEST_ASSERT_NOT_NULL(strstr(actual,"\"full\":true")); TEST_ASSERT_NOT_NULL(strstr(actual,"\"casters\":[]"));
    web_shell_frame_dispose(&f); free(s); free(expected); free(actual);
}

static bool nested_item(void *ctx, const void *snapshot, bool pub, size_t item, wj_t *w, char *key)
{
    (void)snapshot; (void)pub; (void)item;
    size_t n=(size_t)(uintptr_t)ctx;
    key[0]='a'; key[1]=0;
    for (size_t i=0;i<n;i++) wj_arr_open(w,NULL);
    for (size_t i=0;i<n;i++) wj_arr_close(w);
    return wj_ok(w);
}

static bool writes_nothing(void *ctx, const void *snapshot, bool pub, size_t item, wj_t *w, char *key)
{
    (void)ctx; (void)snapshot; (void)pub; (void)item; (void)w; (void)key;
    return true;
}

static bool stream_status(const web_shell_registry_t *r, const sample_t *s)
{
    static char out[WEB_SHELL_STATUS_MAX];
    sink_t sink={out,sizeof(out),0}; char small[32]; wj_t w;
    wj_init(&w,small,sizeof(small),sink_flush,&sink);
    return web_shell_status_stream(r,s,42,false,&w) && wj_finish(&w);
}

TEST_CASE("web provider: the streamed REST form is held to the limits a frame is", "[web_provider]")
{
    web_shell_registry_t r; init(&r,1,3); TEST_ASSERT_TRUE(web_shell_registry_sections(&r,sections,3));
    sample_t s={.value=1,.items=3};
    TEST_ASSERT_TRUE(stream_status(&r,&s));
    s.fail=true; TEST_ASSERT_FALSE(stream_status(&r,&s)); s.fail=false;
    s.big=true;  TEST_ASSERT_FALSE(stream_status(&r,&s)); s.big=false;
    init(&r,1,1); web_shell_section_t d=sections[2]; d.max_bytes=33;
    TEST_ASSERT_TRUE(web_shell_registry_sections(&r,&d,1));
    TEST_ASSERT_FALSE(stream_status(&r,&s));
    /* A section of a single value that writes nothing is a provider fault, an empty collection is not. */
    init(&r,1,1); d=sections[0]; d.serialize=writes_nothing;
    TEST_ASSERT_TRUE(web_shell_registry_sections(&r,&d,1));
    TEST_ASSERT_FALSE(stream_status(&r,&s));
    init(&r,1,1); TEST_ASSERT_TRUE(web_shell_registry_sections(&r,&sections[2],1));
    s.items=0; TEST_ASSERT_TRUE(stream_status(&r,&s));
    /* The whole answer is bounded as a frame render is. */
    web_shell_section_t many[4];
    static const char *const names[4]={"one","two","three","four"};
    for (size_t i=0;i<4;i++) {
        many[i]=(web_shell_section_t){.key=names[i],.max_bytes=128,.max_items=1,.max_item_bytes=128,.ctx=(void *)names[i],.serialize=value};
    }
    s.big=true;
    web_shell_registry_t tight; TEST_ASSERT_TRUE(web_shell_registry_init(&tight,1,4,256));
    TEST_ASSERT_TRUE(web_shell_registry_sections(&tight,many,4));
    TEST_ASSERT_FALSE(stream_status(&tight,&s));
    web_shell_registry_t roomy; TEST_ASSERT_TRUE(web_shell_registry_init(&roomy,1,4,WEB_SHELL_STATUS_MAX));
    TEST_ASSERT_TRUE(web_shell_registry_sections(&roomy,many,4));
    TEST_ASSERT_TRUE(stream_status(&roomy,&s));
}

TEST_CASE("web provider: REST and WebSocket accept the same nesting in a keyed item", "[web_provider]")
{
    const size_t max_ok=WJ_MAX_DEPTH-2;   /* an item sits two levels down: the answer, then its array */
    for (size_t extra=0;extra<2;extra++) {
        web_shell_registry_t r; init(&r,1,1);
        web_shell_section_t d={.key="items",.merge=WEB_SHELL_KEYED_ARRAY,.max_bytes=256,.max_items=1,.max_item_bytes=128,
                               .ctx=(void *)(uintptr_t)(max_ok+extra),.serialize=nested_item};
        TEST_ASSERT_TRUE(web_shell_registry_sections(&r,&d,1));
        sample_t s={.value=1}; web_shell_frame_t f={0};
        bool frame=web_shell_frame_build(&r,&s,1,false,&f);
        TEST_ASSERT_EQUAL(extra==0,frame);
        TEST_ASSERT_EQUAL(extra==0,stream_status(&r,&s));
        web_shell_frame_dispose(&f);
    }
}

TEST_CASE("web provider: a frame keeps its block across builds and follows a changed capacity", "[web_provider]")
{
    web_shell_registry_t r; init(&r,1,3); TEST_ASSERT_TRUE(web_shell_registry_sections(&r,sections,3));
    sample_t s={.value=1,.items=3}; web_shell_frame_t f={0};
    TEST_ASSERT_TRUE(web_shell_frame_build(&r,&s,1,false,&f));
    TEST_ASSERT_TRUE(f.built);
    const char *block=f.text;
    TEST_ASSERT_EQUAL_size_t(WEB_SHELL_STATUS_MAX,f.cap);
    TEST_ASSERT_TRUE(web_shell_frame_build(&r,&s,2,false,&f));
    TEST_ASSERT_EQUAL_PTR(block,f.text);
    s.fail=true; TEST_ASSERT_FALSE(web_shell_frame_build(&r,&s,3,false,&f));
    TEST_ASSERT_FALSE(f.built); TEST_ASSERT_EQUAL_PTR(block,f.text);
    s.fail=false;

    web_shell_registry_t bigger; TEST_ASSERT_TRUE(web_shell_registry_init(&bigger,1,3,WEB_SHELL_STATUS_MAX-1));
    TEST_ASSERT_TRUE(web_shell_registry_sections(&bigger,sections,3));
    TEST_ASSERT_TRUE(web_shell_frame_build(&bigger,&s,4,false,&f));
    TEST_ASSERT_EQUAL_size_t(WEB_SHELL_STATUS_MAX-1,f.cap);
    web_shell_frame_dispose(&f);
    TEST_ASSERT_NULL(f.text); TEST_ASSERT_EQUAL_size_t(0,f.cap);

    /* An attached block is used in place, refused when too small, and never freed. */
    static char text[WEB_SHELL_STATUS_MAX];
    web_shell_frame_attach(&f,text,sizeof(text));
    TEST_ASSERT_TRUE(web_shell_frame_build(&r,&s,5,false,&f));
    TEST_ASSERT_EQUAL_PTR(text,f.text); TEST_ASSERT_TRUE(f.built);
    web_shell_frame_dispose(&f);
    web_shell_frame_attach(&f,text,sizeof(text));
    TEST_ASSERT_TRUE(web_shell_frame_build(&r,&s,5,false,&f));
    web_shell_registry_t too_big=r; too_big.status_cap=sizeof(text)+1;
    TEST_ASSERT_FALSE(web_shell_frame_build(&too_big,&s,6,false,&f));
    TEST_ASSERT_EQUAL_PTR(text,f.text);
    /* The frame built before the refused one is gone: nothing renders or commits. */
    TEST_ASSERT_FALSE(f.built);
    char none[64]="x"; web_shell_baseline_t kept={0};
    TEST_ASSERT_EQUAL_UINT(0,web_shell_frame_render(&r,&f,NULL,false,true,0,none,sizeof(none)));
    web_shell_frame_commit(&f,&kept); TEST_ASSERT_FALSE(kept.valid);
    web_shell_frame_dispose(&f);
    TEST_ASSERT_NULL(f.text);
}

typedef struct { bool acquire_ok, enqueue_ok; unsigned acquired, queued, owned, restored, closed, completed; } owner_t;
static void *acquire_req(void *ctx, void *req) { owner_t *o=ctx; o->acquired++; return o->acquire_ok ? req : NULL; }
static void deadline(void *ctx, void *req, bool owned) { (void)req; owner_t *o=ctx; if (owned) o->owned++; else o->restored++; }
static bool enqueue_req(void *ctx, void *req) { (void)req; owner_t *o=ctx; o->queued++; return o->enqueue_ok; }
static void close_req(void *ctx, void *req) { (void)req; ((owner_t *)ctx)->closed++; }
static void complete_req(void *ctx, void *req) { (void)req; ((owner_t *)ctx)->completed++; }

TEST_CASE("web provider: worker acquire queue and completion failures retain exactly one owner", "[web_provider]")
{
    owner_t o={0}; int request=0;
    web_shell_owner_ops_t ops={&o,acquire_req,deadline,enqueue_req,close_req,complete_req};
    TEST_ASSERT_FALSE(web_shell_owner_submit(&ops,&request));
    TEST_ASSERT_EQUAL_UINT(0,o.owned); TEST_ASSERT_EQUAL_UINT(0,o.completed);
    o.acquire_ok=true;
    TEST_ASSERT_FALSE(web_shell_owner_submit(&ops,&request));
    TEST_ASSERT_EQUAL_UINT(1,o.owned); TEST_ASSERT_EQUAL_UINT(1,o.restored); TEST_ASSERT_EQUAL_UINT(1,o.completed);
    o.enqueue_ok=true;
    TEST_ASSERT_TRUE(web_shell_owner_submit(&ops,&request));
    TEST_ASSERT_EQUAL_UINT(2,o.owned); TEST_ASSERT_EQUAL_UINT(1,o.completed);
    void *owned=&request;
    web_shell_owner_finish(&ops,&owned,true);
    TEST_ASSERT_NULL(owned); TEST_ASSERT_EQUAL_UINT(1,o.closed); TEST_ASSERT_EQUAL_UINT(2,o.completed);
    web_shell_owner_finish(&ops,&owned,true);
    TEST_ASSERT_EQUAL_UINT(1,o.closed); TEST_ASSERT_EQUAL_UINT(2,o.completed);
    TEST_ASSERT_TRUE(web_shell_owner_submit(&ops,&request)); owned=&request;
    web_shell_owner_finish(&ops,&owned,false);
    TEST_ASSERT_EQUAL_UINT(1,o.closed); TEST_ASSERT_EQUAL_UINT(3,o.completed); TEST_ASSERT_EQUAL_UINT(3,o.restored);
}

TEST_CASE("web provider: declarative CSP preserves station policy and rejects injection", "[web_provider]")
{
    const char *origins[]={"https://tiles.openfreemap.org"}; char out[1536];
    TEST_ASSERT_TRUE(web_shell_csp(out,sizeof(out),"device.local",origins,1,origins,1));
    TEST_ASSERT_EQUAL_STRING("default-src 'self'; script-src 'self'; worker-src 'self'; style-src 'self'; img-src 'self' data: blob: https://tiles.openfreemap.org; font-src 'self'; connect-src 'self' https://tiles.openfreemap.org ws://device.local; object-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'",out);
    const char *bad[]={"https://host/path","https://host; unsafe-inline","https://*.example.org","http://example.org","https://user@host","https://host?x","https://host:443","https://-host","https://host-.org"};
    for (size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        TEST_ASSERT_FALSE(web_shell_origin_valid(bad[i]));
        TEST_ASSERT_FALSE(web_shell_csp(out,sizeof(out),"device.local",&bad[i],1,NULL,0)); TEST_ASSERT_EQUAL_STRING("",out);
    }
    TEST_ASSERT_FALSE(web_shell_csp(out,8,"device.local",origins,1,origins,1)); TEST_ASSERT_EQUAL_STRING("",out);
    /* Blanks the Host check accepts around the name stay out of the policy; anything inside is refused. */
    TEST_ASSERT_TRUE(web_shell_csp(out,sizeof(out)," \tdevice.local:8080 \t",NULL,0,NULL,0));
    TEST_ASSERT_NOT_NULL(strstr(out,"connect-src 'self' ws://device.local:8080; object-src"));
    const char *bad_hosts[]={"","  ","dev ice.local","device.local;x","device'.local","device\".local","dev\\ice","a\r\nb"};
    for (size_t i=0;i<sizeof(bad_hosts)/sizeof(bad_hosts[0]);i++) {
        TEST_ASSERT_FALSE(web_shell_csp(out,sizeof(out),bad_hosts[i],NULL,0,NULL,0)); TEST_ASSERT_EQUAL_STRING("",out);
    }
}

/* The shared transfer drives a product's service without taking ownership of
 * its firmware policy. All failure phases abort once and retain unread counts
 * so the station can preserve refusal draining and its existing error codes. */
typedef struct {
    unsigned preflights, consumes, finishes, aborts, reads, progresses;
    unsigned bytes;
    int fail_preflight, fail_consume, fail_finish;
    bool disconnect;
} service_t;
static int service_preflight(void *ctx, size_t total, const void *head, size_t n)
{
    (void)total; (void)head; (void)n;
    service_t *s=ctx; s->preflights++; return s->fail_preflight;
}
static int service_consume(void *ctx, const void *data, size_t n)
{
    (void)data; service_t *s=ctx; s->consumes++; s->bytes+=(unsigned)n; return s->fail_consume;
}
static int service_finish(void *ctx) { service_t *s=ctx; s->finishes++; return s->fail_finish; }
static void service_abort(void *ctx) { ((service_t *)ctx)->aborts++; }
static void service_progress(void *ctx, size_t n, size_t total) { (void)n; (void)total; ((service_t *)ctx)->progresses++; }
static int service_read(void *ctx, void *out, size_t want)
{
    service_t *s=ctx; s->reads++;
    if (s->disconnect) return -1;
    memset(out,0,want); return (int)want;
}

TEST_CASE("web provider: upload service bounds failures and disconnect abort exactly once", "[web_provider]")
{
    service_t s={0}; char buf[4]={0};
    const web_shell_upload_service_t adapter={.maximum=16,.preflight=service_preflight,.consume=service_consume,
        .finalize=service_finish,.abort=service_abort,.progress=service_progress};
    web_shell_transfer_result_t r=web_shell_transfer(&adapter,&s,20,4,buf,sizeof(buf),service_read,&s);
    TEST_ASSERT_EQUAL(WEB_SHELL_TRANSFER_INVALID,r.stage); TEST_ASSERT_EQUAL_UINT(0,s.preflights);
    TEST_ASSERT_EQUAL_UINT(16,r.left); TEST_ASSERT_EQUAL_UINT(0,s.aborts);
    s.fail_preflight=123;
    r=web_shell_transfer(&adapter,&s,12,4,buf,sizeof(buf),service_read,&s);
    TEST_ASSERT_EQUAL(WEB_SHELL_TRANSFER_PREFLIGHT,r.stage); TEST_ASSERT_EQUAL_INT(123,r.error);
    TEST_ASSERT_EQUAL_UINT(8,r.left); TEST_ASSERT_EQUAL_UINT(1,s.aborts); TEST_ASSERT_EQUAL_UINT(0,s.consumes);
    s=(service_t){.fail_consume=234};
    r=web_shell_transfer(&adapter,&s,12,4,buf,sizeof(buf),service_read,&s);
    TEST_ASSERT_EQUAL(WEB_SHELL_TRANSFER_CONSUME,r.stage); TEST_ASSERT_EQUAL_UINT(8,r.left);
    TEST_ASSERT_EQUAL_INT(234,r.error); TEST_ASSERT_EQUAL_UINT(1,s.aborts); TEST_ASSERT_EQUAL_UINT(0,s.finishes);
    s=(service_t){.disconnect=true};
    r=web_shell_transfer(&adapter,&s,12,4,buf,sizeof(buf),service_read,&s);
    TEST_ASSERT_EQUAL(WEB_SHELL_TRANSFER_READ,r.stage); TEST_ASSERT_EQUAL_UINT(8,r.left);
    TEST_ASSERT_EQUAL_UINT(4,r.received); TEST_ASSERT_EQUAL_UINT(1,s.aborts); TEST_ASSERT_EQUAL_UINT(0,s.finishes);
    s=(service_t){.fail_finish=345};
    r=web_shell_transfer(&adapter,&s,12,4,buf,sizeof(buf),service_read,&s);
    TEST_ASSERT_EQUAL(WEB_SHELL_TRANSFER_FINALIZE,r.stage); TEST_ASSERT_EQUAL_UINT(0,r.left);
    TEST_ASSERT_EQUAL_INT(345,r.error); TEST_ASSERT_EQUAL_UINT(1,s.aborts); TEST_ASSERT_EQUAL_UINT(1,s.finishes);
    s=(service_t){0};
    r=web_shell_transfer(&adapter,&s,12,4,buf,sizeof(buf),service_read,&s);
    TEST_ASSERT_EQUAL(WEB_SHELL_TRANSFER_DONE,r.stage); TEST_ASSERT_EQUAL_UINT(0,r.left);
    TEST_ASSERT_EQUAL_UINT(12,s.bytes); TEST_ASSERT_EQUAL_UINT(3,s.consumes); TEST_ASSERT_EQUAL_UINT(2,s.progresses);
    TEST_ASSERT_EQUAL_UINT(1,s.finishes); TEST_ASSERT_EQUAL_UINT(0,s.aborts);
    s=(service_t){0};
    r=web_shell_transfer(&adapter,&s,12,0,buf,sizeof(buf),service_read,&s);
    TEST_ASSERT_EQUAL(WEB_SHELL_TRANSFER_DONE,r.stage); TEST_ASSERT_EQUAL_UINT(12,s.bytes);
    TEST_ASSERT_EQUAL_UINT(3,s.progresses); TEST_ASSERT_EQUAL_UINT(0,s.aborts);
}

TEST_CASE("web provider: product asset descriptors reject reserved paths collisions and inconsistent totals", "[web_provider]")
{
    static const uint8_t bytes[]={0x1f,0x8b};
    web_asset_t table[]={ {"/index.html","text/html",bytes,sizeof(bytes)},
                          {"/binary.dat","application/octet-stream",bytes,sizeof(bytes)} };
    web_shell_assets_t set={table,2,"0123456789abcdef",4};
    TEST_ASSERT_TRUE(web_shell_assets_valid(&set));
    set.total=3; TEST_ASSERT_FALSE(web_shell_assets_valid(&set)); set.total=4;
    table[1].path="/index.html"; TEST_ASSERT_FALSE(web_shell_assets_valid(&set));
    table[1].path="/api/status"; TEST_ASSERT_FALSE(web_shell_assets_valid(&set));
    table[1].path="/ws"; TEST_ASSERT_FALSE(web_shell_assets_valid(&set));
    table[1].path="/binary.dat"; table[1].type="text/plain\r\nX-Foo: bar";
    TEST_ASSERT_FALSE(web_shell_assets_valid(&set)); table[1].type="application/octet-stream";
    set.digest="bad"; TEST_ASSERT_FALSE(web_shell_assets_valid(&set)); set.digest="0123456789abcdef";
    set.count=33; TEST_ASSERT_FALSE(web_shell_assets_valid(&set));
    set=(web_shell_assets_t){0}; TEST_ASSERT_TRUE(web_shell_assets_valid(&set));
}
