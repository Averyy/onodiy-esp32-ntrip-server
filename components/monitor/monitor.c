/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The monitor's task around its sums (monitor_core.c): one sample every
 * MONITOR_SAMPLE_S, the summary worked out again once per sample (readers
 * copy it: the Status page's once-a-second status costs no summing), all
 * under one mutex (the series copy is too long for a spinlock).
 *
 * The server figures are one server's: when another is pinged (Onocoy
 * turned off, so the first enabled slot instead), the ones before go.
 * The fixed internet target has its own history, independent of the slots.
 *
 * Pings go out on one raw ICMP socket, open for the task's life: what it
 * was handed between samples (late replies, other ICMP) is read off before
 * each request. lwIP hands raw sockets copies, so ICMP still works as
 * before for everything else.
 *
 * Its stack is in PSRAM: the task pings (lwIP), reads counters and logs; it
 * never reads or writes flash, so it never runs while the cache is off. It
 * is never deleted.
 */
#include "monitor.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip.h"
#include "lwip/prot/icmp.h"
#include "lwip/sockets.h"
#include "monitor_core.h"
#include "network.h"
#include "ntrip_server.h"

#define TASK_STACK 4096
#define TASK_PRIO  (tskIDLE_PRIORITY + 1)   /* below everything that does the station's work */
#define PING_DATA  32
#define PING_ID    0x4d4fu                  /* "MO": this monitor's echo requests */
#define RX_BUF     128                      /* an IP header with options, the echo header and its data */

static const char *TAG = "monitor";

static SemaphoreHandle_t s_mux;
static mon_core_t s_core;               /* under s_mux */
static monitor_status_t s_summary;      /* under s_mux: worked out at every sample */
static bool s_running;

static int s_sock = -1;
static uint16_t s_seq;

static uint64_t s_idle_prev[portNUM_PROCESSORS];
static int64_t s_cpu_prev_us;
static bool s_cpu_have;

/* ---- ping ------------------------------------------------------------------------------- */

/* One echo request to `addr` (IPv4, network order) and its reply within MONITOR_PING_TIMEOUT_MS. */
static monitor_ping_t ping(uint32_t addr, uint16_t *ms)
{
    if (s_sock < 0) {
        s_sock = socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
        if (s_sock < 0) {
            return MONITOR_PING_NONE;   /* no socket: no sample, not a loss */
        }
    }
    uint8_t buf[RX_BUF];
    while (recv(s_sock, buf, sizeof(buf), MSG_DONTWAIT) > 0) {
    }

    struct {
        struct icmp_echo_hdr h;
        uint8_t data[PING_DATA];
    } req;
    memset(&req, 0, sizeof(req));
    req.h.type = ICMP_ECHO;
    req.h.id = lwip_htons(PING_ID);
    req.h.seqno = lwip_htons(++s_seq);
    for (size_t i = 0; i < PING_DATA; i++) {
        req.data[i] = (uint8_t)i;
    }
    req.h.chksum = inet_chksum(&req, sizeof(req));

    struct sockaddr_in to = { .sin_family = AF_INET };
    to.sin_addr.s_addr = addr;
    int64_t t0 = esp_timer_get_time();
    if (sendto(s_sock, &req, sizeof(req), 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
        return MONITOR_PING_LOST;
    }
    int64_t deadline = t0 + (int64_t)MONITOR_PING_TIMEOUT_MS * 1000;
    for (;;) {
        int64_t left = deadline - esp_timer_get_time();
        if (left <= 0) {
            return MONITOR_PING_LOST;
        }
        struct timeval tv = { .tv_sec = (time_t)(left / 1000000), .tv_usec = (suseconds_t)(left % 1000000) };
        (void)setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n < 0) {
            return MONITOR_PING_LOST;   /* timed out (or the socket failed: the next sample tries again) */
        }
        int64_t t1 = esp_timer_get_time();
        if ((size_t)n < sizeof(struct ip_hdr)) {
            continue;
        }
        const struct ip_hdr *ip = (const struct ip_hdr *)buf;
        size_t hl = IPH_HL_BYTES(ip);
        if ((size_t)n < hl + sizeof(struct icmp_echo_hdr)) {
            continue;
        }
        const struct icmp_echo_hdr *e = (const struct icmp_echo_hdr *)(buf + hl);
        if (from.sin_addr.s_addr != addr || e->type != ICMP_ER || e->id != req.h.id || e->seqno != req.h.seqno) {
            continue;
        }
        uint64_t ms_r = ((uint64_t)(t1 - t0) + 500u) / 1000u;
        *ms = (uint16_t)(ms_r > UINT16_MAX ? UINT16_MAX : ms_r);
        return MONITOR_PING_OK;
    }
}

/*
 * The server to ping: the Onocoy slot's address while it is on and its own
 * lookup found one, else the first enabled slot's (IPv4 only). No enabled
 * slot with an address means no server target; the internet probe still runs.
 */
static bool server_target(uint32_t *addr, char *name, size_t size)
{
    ntrip_caster_cfg_t cfg;
    ntrip_caster_status_t st;
    bool found = false, found_protected = false;
    size_t n = ntrip_server_slot_count();
    for (size_t i = 0; i < n && !found_protected; i++) {
        if (ntrip_server_get_config(i, &cfg, false) != ESP_OK || !cfg.enabled ||
            ntrip_server_get_status(i, &st) != ESP_OK) {
            continue;
        }
        struct in_addr a;
        if (inet_pton(AF_INET, st.resolved_ip, &a) != 1 || (found && !cfg.protected_slot)) {
            continue;
        }
        *addr = a.s_addr;
        strlcpy(name, cfg.name, size);
        found = true;
        found_protected = cfg.protected_slot;
    }
    return found;
}

/* ---- CPU -------------------------------------------------------------------------------- */

/* The busier core's load since the last call, from the idle tasks' run-time counters (esp_timer microseconds). */
static bool cpu_load(uint8_t *pct)
{
    int64_t now = esp_timer_get_time();
    uint64_t idle[portNUM_PROCESSORS];
    for (int c = 0; c < portNUM_PROCESSORS; c++) {
        idle[c] = ulTaskGetIdleRunTimeCounterForCore(c);
    }
    bool ok = s_cpu_have && now - s_cpu_prev_us >= 1000000;   /* a few ms (boot) would say little */
    uint8_t busiest = 0;
    if (ok) {
        uint64_t span = (uint64_t)(now - s_cpu_prev_us);
        for (int c = 0; c < portNUM_PROCESSORS; c++) {
            uint64_t d = idle[c] - s_idle_prev[c];
            if (d > span) {
                d = span;
            }
            uint8_t busy = (uint8_t)(100u - (uint32_t)((d * 100u + span / 2u) / span));
            if (busy > busiest) {
                busiest = busy;
            }
        }
    }
    memcpy(s_idle_prev, idle, sizeof(s_idle_prev));
    s_cpu_prev_us = now;
    s_cpu_have = true;
    *pct = busiest;
    return ok;
}

/* ---- task ------------------------------------------------------------------------------- */

static void monitor_task(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        /* The first sample a whole interval after the start: its CPU load covers that interval. */
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(MONITOR_SAMPLE_S * 1000u));
        monitor_sample_t s;
        memset(&s, 0, sizeof(s));
        uint32_t addr = 0;
        if (network_get_gateway(&addr)) {
            s.router = ping(addr, &s.router_ms);
        }
        if (network_is_online()) {   /* a fixed target, independent of the caster's lookup and connection */
            s.internet = ping(inet_addr(MONITOR_INTERNET_IP), &s.internet_ms);
        }
        char name[sizeof(s_summary.server_name)] = "";
        if (network_is_online() && server_target(&addr, name, sizeof(name))) {   /* offline: no sample, not a loss */
            s.server = ping(addr, &s.server_ms);
        }
        network_status_t ns;
        if (network_get_status(&ns) == ESP_OK && ns.associated && ns.rssi < 0) {
            s.rssi_known = true;
            s.rssi = ns.rssi;
        }
        s.cpu_known = cpu_load(&s.cpu_pct);
        s.mem_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

        xSemaphoreTake(s_mux, portMAX_DELAY);
        char keep[sizeof(s_summary.server_name)];
        strlcpy(keep, s.server != MONITOR_PING_NONE ? name : s_summary.server_name, sizeof(keep));
        if (s_summary.server_name[0] != '\0' && strcmp(keep, s_summary.server_name) != 0) {
            mon_forget_server(&s_core);   /* another server: its figures are its own, not the one's before */
        }
        mon_add(&s_core, &s);
        mon_summary(&s_core, &s_summary);
        strlcpy(s_summary.server_name, keep, sizeof(s_summary.server_name));
        s_summary.running = true;
        xSemaphoreGive(s_mux);
    }
}

esp_err_t monitor_start(void)
{
    if (s_mux != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    monitor_bucket_t *ring = heap_caps_calloc(MONITOR_BUCKETS, sizeof(*ring), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ring == NULL) {
        ring = heap_caps_calloc(MONITOR_BUCKETS, sizeof(*ring), MALLOC_CAP_8BIT);
    }
    s_mux = xSemaphoreCreateMutex();
    if (ring == NULL || s_mux == NULL) {
        free(ring);
        return ESP_ERR_NO_MEM;
    }
    mon_init(&s_core, ring, MONITOR_BUCKETS);
    (void)cpu_load(&(uint8_t){ 0 });   /* the first sample's CPU load counts from here */
    if (xTaskCreatePinnedToCoreWithCaps(monitor_task, "monitor", TASK_STACK, NULL, TASK_PRIO, NULL, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_running = true;
    ESP_LOGI(TAG, "a sample every %us: router, internet and server ping, Wi-Fi signal, CPU, memory", (unsigned)MONITOR_SAMPLE_S);
    return ESP_OK;
}

void monitor_get_status(monitor_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_mux == NULL) {
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_summary;
    xSemaphoreGive(s_mux);
    out->running = s_running;
}

void monitor_get_series(monitor_bucket_t *out, size_t max, size_t *n)
{
    *n = 0;
    if (s_mux == NULL) {
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *n = mon_series(&s_core, out, max);
    xSemaphoreGive(s_mux);
}
