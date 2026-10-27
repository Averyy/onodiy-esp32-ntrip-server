/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Explicit, temporary ICMP sampling, independent of the 30-second monitor.
 * One low-priority task owns the raw socket. The record lock covers only
 * bounded copies/core operations; a separate command lock orders start/stop
 * against sends. Socket calls never hold the record lock or an RTCM lock.
 *
 * The ring and task stack allocate from PSRAM only, once on first start. The
 * idle task remains asleep and the socket closes; a stopped run's records
 * remain readable. No flash writes, per-packet logs or DNS lookups occur here.
 */
#include "monitor_diag.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip.h"
#include "lwip/prot/icmp.h"
#include "lwip/sockets.h"
#include "monitor.h"
#include "monitor_diag_core.h"
#include "monitor_targets.h"
#include "network.h"

#define TASK_STACK 4096
#define TASK_PRIO (tskIDLE_PRIORITY + 1)
#define PING_ID 0x4d44u             /* "MD", distinct from the normal monitor's "MO" */
#define PING_MAGIC 0x4d444731u      /* "MDG1" */
#define RX_BYTES 128u
#define RX_BATCH 12u
#define RX_CAPACITY 2048
#define WAIT_US 50000u

_Static_assert(MONITOR_DIAG_CAPACITY * sizeof(monitor_diag_sample_t) + TASK_STACK <= 64u * 1024u,
               "diagnostic ring and stack must fit their fixed PSRAM budget");

typedef struct {
    struct icmp_echo_hdr header;
    uint32_t magic, nonce, generation, seq, target;
} diag_echo_t;

_Static_assert(sizeof(diag_echo_t) == 28, "diagnostic echo wire layout changed");

static portMUX_TYPE s_init_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_records_mux;
static SemaphoreHandle_t s_command_mux;
static mon_diag_t s_core;             /* s_records_mux */
static TaskHandle_t s_task;           /* s_command_mux; never deleted */

/* Allocation is outside the spinlock. Concurrent first callers can allocate
 * candidates, but only one pair is installed and the unused pair is freed. */
static bool locks_ready(void)
{
    portENTER_CRITICAL(&s_init_lock);
    bool ready = s_records_mux != NULL;
    portEXIT_CRITICAL(&s_init_lock);
    if (ready) {
        return true;
    }
    SemaphoreHandle_t records = xSemaphoreCreateMutex();
    SemaphoreHandle_t command = xSemaphoreCreateMutex();
    if (records == NULL || command == NULL) {
        if (records != NULL) vSemaphoreDelete(records);
        if (command != NULL) vSemaphoreDelete(command);
        return false;
    }
    portENTER_CRITICAL(&s_init_lock);
    bool installed = s_records_mux == NULL;
    if (installed) {
        s_command_mux = command;
        s_records_mux = records;
    }
    portEXIT_CRITICAL(&s_init_lock);
    if (!installed) {
        vSemaphoreDelete(records);
        vSemaphoreDelete(command);
    }
    return true;
}

/* Late dispatch plus the reply window can keep the newest five cycles
 * pending; keep receiving those briefly after natural expiry. */
static bool pending_locked(void)
{
    for (uint32_t i = 0; i < MON_DIAG_PENDING_CYCLES && i < s_core.seq; i++) {
        uint32_t seq = s_core.seq - i;
        const monitor_diag_sample_t *s = &s_core.ring[(seq - 1u) % s_core.capacity];
        for (unsigned j = 0; j < MONITOR_DIAG_TARGETS; j++) {
            if (s->ping[j].result == MONITOR_DIAG_PENDING) {
                return true;
            }
        }
    }
    return false;
}

static int socket_open(void)
{
    int fd = socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
    if (fd < 0) {
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    int capacity = RX_CAPACITY;
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &capacity, sizeof(capacity)) < 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

static void targets(uint32_t address[MONITOR_DIAG_TARGETS], int8_t *rssi)
{
    memset(address, 0, MONITOR_DIAG_TARGETS * sizeof(*address));
    *rssi = 0;
    (void)network_get_gateway(&address[0]);
    if (network_is_online()) {
        address[1] = inet_addr(MONITOR_INTERNET_IP);
        char name[24];
        (void)monitor_server_target(&address[2], name, sizeof(name));
    }
    network_status_t ns;
    if (network_get_status(&ns) == ESP_OK && ns.associated && ns.rssi < 0) {
        *rssi = ns.rssi;
    }
}

static void send_cycle(int *fd, uint32_t generation, uint32_t nonce)
{
    uint32_t address[MONITOR_DIAG_TARGETS];
    int8_t rssi;
    targets(address, &rssi);

    /* A stop waits only for a current send batch, never for echo replies.
     * Recheck generation after collecting targets: start/stop could have run
     * during those status reads. Once stop returns no old run can send. */
    xSemaphoreTake(s_command_mux, portMAX_DELAY);
    xSemaphoreTake(s_records_mux, portMAX_DELAY);
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    uint32_t seq = generation == s_core.generation ?
        mon_diag_begin(&s_core, now_us, diag_utc_now(), address, rssi) : 0;
    xSemaphoreGive(s_records_mux);
    if (seq == 0) {
        xSemaphoreGive(s_command_mux);
        return;
    }

    int open_error = 0;
    if (*fd < 0) {
        *fd = socket_open();
        if (*fd < 0) open_error = errno != 0 ? errno : EIO;
    }
    for (unsigned j = 0; j < MONITOR_DIAG_TARGETS; j++) {
        if (address[j] == 0) continue;
        xSemaphoreTake(s_records_mux, portMAX_DELAY);
        now_us = (uint64_t)esp_timer_get_time();
        mon_diag_expire(&s_core, now_us);
        bool current = s_core.active && s_core.generation == generation &&
                       now_us - s_core.ring[(seq - 1u) % s_core.capacity].started_us < MON_DIAG_TIMEOUT_US;
        xSemaphoreGive(s_records_mux);
        int error = current ? open_error : ECANCELED;
        if (current && *fd >= 0) {
            diag_echo_t request = {
                .header = { .type = ICMP_ECHO, .id = lwip_htons(PING_ID), .seqno = lwip_htons((uint16_t)seq) },
                .magic = lwip_htonl(PING_MAGIC),
                .nonce = lwip_htonl(nonce),
                .generation = lwip_htonl(generation),
                .seq = lwip_htonl(seq),
                .target = lwip_htonl(j),
            };
            request.header.chksum = inet_chksum(&request, sizeof(request));
            struct sockaddr_in to = { .sin_family = AF_INET };
            to.sin_addr.s_addr = address[j];
            /* lwIP raw sendto marshals to TCP/IP even with a nonblocking
             * descriptor; it does not wait for a reply. No record lock is
             * held during that handoff. There are no retries/catch-up sends. */
            xSemaphoreTake(s_records_mux, portMAX_DELAY);
            now_us = (uint64_t)esp_timer_get_time();
            bool dispatch = mon_diag_sent(&s_core, generation, seq, j, now_us);
            xSemaphoreGive(s_records_mux);
            if (dispatch) {
                ssize_t sent = sendto(*fd, &request, sizeof(request), MSG_DONTWAIT,
                                      (struct sockaddr *)&to, sizeof(to));
                if (sent != sizeof(request)) error = sent < 0 && errno != 0 ? errno : EIO;
            } else {
                error = ETIMEDOUT;
            }
        }
        if (error != 0) {
            xSemaphoreTake(s_records_mux, portMAX_DELAY);
            mon_diag_error(&s_core, generation, seq, j, error);
            xSemaphoreGive(s_records_mux);
        }
    }
    xSemaphoreGive(s_command_mux);
}

static void receive_one(const uint8_t *buf, size_t size, const struct sockaddr_in *from,
                        uint32_t nonce, uint64_t now_us)
{
    if (size < sizeof(struct ip_hdr) || from->sin_family != AF_INET) return;
    struct ip_hdr ip;
    memcpy(&ip, buf, sizeof(ip));
    size_t header_size = IPH_HL_BYTES(&ip);
    size_t packet_size = lwip_ntohs(IPH_LEN(&ip));
    if (IPH_V(&ip) != 4 || IPH_PROTO(&ip) != IP_PROTO_ICMP || header_size < sizeof(ip) ||
        packet_size > size || packet_size != header_size + sizeof(diag_echo_t)) return;
    const uint8_t *icmp = buf + header_size;
    if (inet_chksum(icmp, sizeof(diag_echo_t)) != 0) return;
    diag_echo_t reply;
    memcpy(&reply, icmp, sizeof(reply));
    uint32_t seq = lwip_ntohl(reply.seq);
    uint32_t target = lwip_ntohl(reply.target);
    if (reply.header.type != ICMP_ER || reply.header.code != 0 ||
        reply.header.id != lwip_htons(PING_ID) || reply.header.seqno != lwip_htons((uint16_t)seq) ||
        lwip_ntohl(reply.magic) != PING_MAGIC || lwip_ntohl(reply.nonce) != nonce ||
        target >= MONITOR_DIAG_TARGETS) return;
    xSemaphoreTake(s_records_mux, portMAX_DELAY);
    (void)mon_diag_reply(&s_core, lwip_ntohl(reply.generation), seq, target,
                         from->sin_addr.s_addr, now_us);
    xSemaphoreGive(s_records_mux);
}

static void receive_failed(int *fd, uint32_t generation, int error)
{
    xSemaphoreTake(s_records_mux, portMAX_DELAY);
    mon_diag_receive_error(&s_core, generation, error != 0 ? error : EIO);
    xSemaphoreGive(s_records_mux);
    if (*fd >= 0) close(*fd);
    *fd = -1;
}

static void receive_batch(int *fd, uint32_t generation, uint32_t nonce)
{
    if (*fd < 0) return;
    for (unsigned i = 0; i < RX_BATCH; i++) {
        uint8_t buf[RX_BYTES];
        struct sockaddr_in from;
        socklen_t from_size = sizeof(from);
        ssize_t size = recvfrom(*fd, buf, sizeof(buf), MSG_DONTWAIT,
                                (struct sockaddr *)&from, &from_size);
        if (size < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                int error = errno;
                receive_failed(fd, generation, error);
            }
            return;
        }
        if (from_size >= sizeof(from)) {
            receive_one(buf, (size_t)size, &from, nonce, (uint64_t)esp_timer_get_time());
        }
    }
    /* An ICMP flood must not keep this low-priority task ready forever. */
    vTaskDelay(1);
}

static void worker(void *arg)
{
    (void)arg;
    int fd = -1;
    uint32_t generation_seen = 0, nonce = 0;
    for (;;) {
        xSemaphoreTake(s_records_mux, portMAX_DELAY);
        uint64_t now_us = (uint64_t)esp_timer_get_time();
        bool due = mon_diag_due(&s_core, now_us);
        bool running = s_core.active || pending_locked();
        uint32_t generation = s_core.generation;
        xSemaphoreGive(s_records_mux);
        if (!running) {
            if (fd >= 0) close(fd);
            fd = -1;
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (generation != generation_seen) {
            generation_seen = generation;
            nonce = esp_random();
        }
        if (due) send_cycle(&fd, generation, nonce);
        receive_batch(&fd, generation, nonce);
        if (fd < 0) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        struct timeval timeout = { .tv_usec = WAIT_US };
        if (select(fd + 1, &readfds, NULL, NULL, &timeout) < 0) {
            int error = errno;
            if (error != EINTR) receive_failed(&fd, generation, error);
            vTaskDelay(1);
        }
    }
}

esp_err_t monitor_diag_start(void)
{
    if (!locks_ready()) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_command_mux, portMAX_DELAY);
    xSemaphoreTake(s_records_mux, portMAX_DELAY);
    mon_diag_expire(&s_core, (uint64_t)esp_timer_get_time());
    bool active = s_core.active;
    bool need_ring = s_core.ring == NULL;
    xSemaphoreGive(s_records_mux);
    if (active) {
        xSemaphoreGive(s_command_mux);
        return ESP_ERR_INVALID_STATE;
    }
    if (need_ring) {
        monitor_diag_sample_t *ring = heap_caps_calloc(MONITOR_DIAG_CAPACITY, sizeof(*ring),
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ring == NULL) {
            xSemaphoreGive(s_command_mux);
            return ESP_ERR_NO_MEM;
        }
        xSemaphoreTake(s_records_mux, portMAX_DELAY);
        mon_diag_init(&s_core, ring, MONITOR_DIAG_CAPACITY);
        xSemaphoreGive(s_records_mux);
    }
    if (s_task == NULL && xTaskCreatePinnedToCoreWithCaps(worker, "mon_diag", TASK_STACK, NULL,
            TASK_PRIO, &s_task, tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        xSemaphoreTake(s_records_mux, portMAX_DELAY);
        monitor_diag_sample_t *ring = s_core.ring;
        mon_diag_init(&s_core, NULL, 0);
        xSemaphoreGive(s_records_mux);
        free(ring);
        xSemaphoreGive(s_command_mux);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_records_mux, portMAX_DELAY);
    mon_diag_start(&s_core, (uint64_t)esp_timer_get_time());
    xSemaphoreGive(s_records_mux);
    xTaskNotifyGive(s_task);
    xSemaphoreGive(s_command_mux);
    return ESP_OK;
}

void monitor_diag_stop(void)
{
    if (!locks_ready()) return;
    xSemaphoreTake(s_command_mux, portMAX_DELAY);
    xSemaphoreTake(s_records_mux, portMAX_DELAY);
    mon_diag_stop(&s_core);
    xSemaphoreGive(s_records_mux);
    if (s_task != NULL) xTaskNotifyGive(s_task);
    xSemaphoreGive(s_command_mux);
}

void monitor_diag_get(uint32_t after, monitor_diag_sample_t *out, size_t max,
                      size_t *n, monitor_diag_status_t *status)
{
    if (n != NULL) *n = 0;
    if (status != NULL) {
        memset(status, 0, sizeof(*status));
        status->now_us = (uint64_t)esp_timer_get_time();
    }
    if (!locks_ready()) return;
    if (max > MONITOR_DIAG_PAGE) max = MONITOR_DIAG_PAGE;
    if (out == NULL) max = 0;
    xSemaphoreTake(s_records_mux, portMAX_DELAY);
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    mon_diag_expire(&s_core, now_us);
    size_t count = mon_diag_copy(&s_core, after, out, max);
    if (status != NULL) mon_diag_status(&s_core, now_us, status);
    xSemaphoreGive(s_records_mux);
    if (n != NULL) *n = count;
}
