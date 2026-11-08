/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */
#include <stdatomic.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "web_shell_internal.h"
#include "web_shell_worker.h"

#define WORKER_STACK 6144
#define WORKER_PRIO 4

typedef struct {
    httpd_req_t *req;
    web_shell_job_fn fn;
    void *ctx;
    size_t size;
    bool upload;
    union { max_align_t align; unsigned char bytes[WEB_SHELL_JOB_BYTES]; } payload;
} shell_job_t;
static QueueHandle_t s_jobs;
static SemaphoreHandle_t s_admission;
static SemaphoreHandle_t s_active_lock;
static TaskHandle_t s_task;
static atomic_bool s_busy;
static atomic_bool s_accept;
static atomic_uint s_pending;
static atomic_int s_holds;
/* Protect only the active socket, including completion, against stop's
 * shutdown. Do not hold this lock while draining or completing HTTP. */
static httpd_req_t *s_active;

bool web_shell_worker_upload_begin(void)
{
    return atomic_load(&s_accept) && !atomic_exchange(&s_busy,true);
}
void web_shell_worker_upload_release(void) { atomic_store(&s_busy,false); }
bool web_shell_worker_upload_busy(void) { return atomic_load(&s_busy); }
void web_shell_worker_response_hold(bool hold) { atomic_fetch_add(&s_holds,hold ? 1 : -1); }
int web_worker_pending_responses(void) { return atomic_load(&s_holds); }
bool web_shell_worker_idle(void) { return atomic_load(&s_pending) == 0; }
void web_shell_worker_accept(bool accept)
{
    if (s_admission) xSemaphoreTake(s_admission,portMAX_DELAY);
    atomic_store(&s_accept,accept);
    if (s_admission) xSemaphoreGive(s_admission);
}

static void *acquire(void *ctx, void *req)
{
    (void)ctx;
    httpd_req_t *copy = NULL;
    return httpd_req_async_handler_begin(req,&copy) == ESP_OK ? copy : NULL;
}
static void deadline(void *ctx, void *req, bool on)
{
    (void)ctx; web_conn_worker(httpd_req_to_sockfd(req),on);
}
static bool enqueue(void *ctx, void *copy)
{
    shell_job_t *job = ctx;
    job->req = copy;
    return xQueueSend(s_jobs,job,0) == pdTRUE;
}
static void close_owned(void *ctx, void *req)
{
    (void)ctx;
    web_linger_close(req);
    web_conn_shut(httpd_req_to_sockfd(req));
}
static void complete(void *ctx, void *req)
{
    (void)ctx; (void)httpd_req_async_handler_complete(req);
}
static const web_shell_owner_ops_t s_ops = {
    .acquire=acquire,.deadline=deadline,.enqueue=enqueue,.close=close_owned,.complete=complete,
};

esp_err_t web_shell_worker_submit(httpd_req_t *req, web_shell_job_fn fn, void *ctx,
                                 const void *data, size_t size, bool upload)
{
    if (!fn || size > WEB_SHELL_JOB_BYTES || (size && !data)) {
        if (upload) web_shell_worker_upload_release();
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_admission || xSemaphoreTake(s_admission,0) != pdTRUE) {
        if (upload) web_shell_worker_upload_release();
        return ESP_ERR_NO_MEM;
    }
    if (!s_jobs || (req && !atomic_load(&s_accept)) || uxQueueSpacesAvailable(s_jobs) == 0) {
        xSemaphoreGive(s_admission);
        if (upload) web_shell_worker_upload_release();
        return ESP_ERR_NO_MEM;
    }
    shell_job_t job = {.fn=fn,.ctx=ctx,.size=size,.upload=upload};
    if (size) memcpy(job.payload.bytes,data,size);
    /* Count before enqueue: worker may finish before submission returns. */
    atomic_fetch_add(&s_pending,1);
    bool ok;
    if (req) {
        web_shell_owner_ops_t ops=s_ops; ops.ctx=&job;
        ok=web_shell_owner_submit(&ops,req);
    } else ok=enqueue(&job,NULL);
    xSemaphoreGive(s_admission);
    if (!ok) {
        atomic_fetch_sub(&s_pending,1);
        if (upload) web_shell_worker_upload_release();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void web_shell_worker_interrupt(void)
{
    if (!s_active_lock) return;
    xSemaphoreTake(s_active_lock,portMAX_DELAY);
    if (s_active) web_conn_shut(httpd_req_to_sockfd(s_active));
    xSemaphoreGive(s_active_lock);
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        shell_job_t job;
        if (xQueueReceive(s_jobs,&job,portMAX_DELAY) != pdTRUE) continue;
        xSemaphoreTake(s_active_lock,portMAX_DELAY);
        s_active=job.req;
        xSemaphoreGive(s_active_lock);
        /* Stop unblocks bodies of accepted queued jobs too. Service code
         * still runs and gets to clean up its own reservations/writers. */
        if (job.req && !atomic_load(&s_accept)) web_conn_shut(httpd_req_to_sockfd(job.req));
        bool cut=job.fn(job.ctx,job.req,job.payload.bytes,job.size);
        if (job.req) {
            xSemaphoreTake(s_active_lock,portMAX_DELAY);
            s_active=NULL;
            xSemaphoreGive(s_active_lock);
            void *owned=job.req;
            web_shell_owner_finish(&s_ops,&owned,cut || web_conn_req_close(job.req));
        }
        if (job.upload) web_shell_worker_upload_release();
        atomic_fetch_sub(&s_pending,1);
    }
}

esp_err_t web_shell_worker_init(void)
{
    if (s_task) return ESP_OK;
    if (!s_admission) s_admission=xSemaphoreCreateMutex();
    if (!s_admission) return ESP_ERR_NO_MEM;
    if (!s_active_lock) s_active_lock=xSemaphoreCreateMutex();
    if (!s_active_lock) return ESP_ERR_NO_MEM;
    if (!s_jobs) s_jobs=xQueueCreate(WEB_SHELL_WORKER_QUEUE,sizeof(shell_job_t));
    if (!s_jobs) return ESP_ERR_NO_MEM;
    /* Explicit capabilities: never place a flash-writing stack in PSRAM. */
    if (xTaskCreateWithCaps(worker,"web_io",WORKER_STACK,NULL,WORKER_PRIO,&s_task,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) return ESP_ERR_NO_MEM;
    atomic_store(&s_accept,true);
    return ESP_OK;
}

int web_shell_worker_recv(httpd_req_t *req, void *buf, size_t want)
{
    for (int tries=0;tries<3;tries++) {
        int n=httpd_req_recv(req,buf,want);
        if (n>0) return n;
        if (n != HTTPD_SOCK_ERR_TIMEOUT) return -1;
    }
    return -1;
}
bool web_shell_worker_recv_exact(httpd_req_t *req, void *buf, size_t len)
{
    size_t got=0;
    while (got<len) {
        int n=web_shell_worker_recv(req,(char *)buf+got,len-got);
        if (n<0) return false;
        got+=(size_t)n;
    }
    return true;
}
bool web_shell_worker_drain(httpd_req_t *req, uint32_t left, void *buf, size_t cap)
{
    if (!cap) return false;
    while (left) {
        int n=web_shell_worker_recv(req,buf,left<cap ? left : cap);
        if (n<0) return false;
        left-=(uint32_t)n;
    }
    return true;
}
