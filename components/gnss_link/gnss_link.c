/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * GNSS link: UART driver, RX task, lease handshake and the TX side. The
 * decisions (who may do what, routing, counters, drain sizing) live in the
 * pure core (gnss_link_core.c); this file owns the hardware, the task and
 * the locks.
 *
 * Locks:
 *  - s_lock (spinlock): the core state, the sinks and the request mailbox.
 *    Held only for copies and counter updates, never across a driver call.
 *  - s_slot (binary semaphore): the one lease slot. Taken by acquire, given
 *    by release, possibly from another task (the boot step hands its RAW
 *    lease on), hence not a mutex.
 *  - s_tx (mutex): one writer at a time. Also held while a lease is granted
 *    (so no NULL-lease write is still in flight when STREAM ends) and while
 *    the line is reconfigured (baud, break, reinstall).
 *  - s_req_mutex (mutex): one request to the RX task at a time.
 *
 * Requests: everything that changes the input side (mode switches, baud
 * change, resync, reinstall, prompt detector) runs in the RX task between
 * two reads, handed over through a one-slot mailbox and acknowledged on
 * s_req_done. So no byte is ever read by two readers, and the framer is only
 * ever touched by one task. Before the RX task exists, and for a baud change
 * under RAW (the RX task is parked and the lease holder is the only reader),
 * the request runs in the caller instead.
 *
 * The RX task never logs and never touches flash (counters only); the driver
 * is opened and reinstalled by a short-lived helper task pinned to the RX
 * task's core, which puts the UART interrupt on that core and keeps the
 * driver's own install messages out of the RX task.
 */
#include "gnss_link.h"

#include "board.h"
#include "driver/uart.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gnss_link_core.h"
#include "sdkconfig.h"
#include "settings.h"
#include "settings_schema.h"

static const char *TAG = "gnss_link";

#if CONFIG_FREERTOS_UNICORE
#define RX_CORE 0
#else
#define RX_CORE CONFIG_GNSS_LINK_RX_TASK_CORE
#endif

#define RX_NOTIFY_IDX   1       /* index 0 belongs to IDF APIs */
#define OPEN_TASK_STACK 4096
#define REQ_PICKUP_MS   1000    /* the RX task looks at the mailbox at least every 20 ms */
#define TX_LOCK_MS      1000    /* line reconfiguration waits this long for a writer to finish */
#define TX_DRAIN_MS     500     /* pending TX before a baud switch or break (commands are < 128 B) */
#define FLUSH_LIMIT     ((size_t)CONFIG_GNSS_LINK_RX_BUF_SIZE + 4096u)

_Static_assert(CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES >= 2,
               "the GNSS RX task parks and wakes on task notification index 1");

#if CONFIG_UART_ISR_IN_IRAM
/* The flag the driver would add on its own with the ISR in IRAM; passing it
 * keeps the allocation the same and the driver quiet. */
#define UART_INTR_FLAGS ESP_INTR_FLAG_IRAM
#else
#define UART_INTR_FLAGS 0
#endif

/* ---- shared state ------------------------------------------------------------ */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static gnss_link_core_t s_core;        /* under s_lock; mode OFF until init */
static gnss_link_sinks_t s_sinks;      /* under s_lock */
static bool s_inited;                  /* set once by gnss_link_init */
static bool s_driver_ok;               /* under s_lock */
static QueueHandle_t s_events;         /* under s_lock: the driver's event queue */
static uart_port_t s_port;
static TaskHandle_t s_rx_task;         /* set once by gnss_link_start */

static bool s_sync_ready;
static SemaphoreHandle_t s_slot;
static SemaphoreHandle_t s_tx;
static SemaphoreHandle_t s_req_mutex;
static SemaphoreHandle_t s_req_done;
static SemaphoreHandle_t s_open_done;
static StaticSemaphore_t s_slot_buf;
static StaticSemaphore_t s_tx_buf;
static StaticSemaphore_t s_req_mutex_buf;
static StaticSemaphore_t s_req_done_buf;
static StaticSemaphore_t s_open_done_buf;

static StaticTask_t s_rx_tcb;
static StackType_t s_rx_stack[CONFIG_GNSS_LINK_RX_TASK_STACK];

/* Reader-side state: used by the RX task, or by the single reader while the
 * RX task does not run yet or is parked (see "Requests" above). */
static gnss_framer_t s_framer;
static uint8_t s_rx_buf[GNSS_LINK_READ_CHUNK];
static gnss_link_sinks_t s_cur;        /* sinks snapshot for the frame callback */
static bool s_wdt_on;                  /* RX task only */
static bool s_wdt_failed;              /* RX task only: subscription refused, do not retry */

/* ---- requests to the RX task -------------------------------------------------- */

typedef enum {
    REQ_ENTER = 0,
    REQ_EXIT,
    REQ_SET_BAUD,
    REQ_RESYNC,
    REQ_RESTART,
    REQ_PROMPT,
} req_op_t;

typedef struct {
    req_op_t op;
    uint32_t token;          /* lease token, or the new token for REQ_ENTER */
    gnss_link_mode_t mode;   /* REQ_ENTER */
    const char *owner;       /* REQ_ENTER */
    uint32_t baud;           /* REQ_SET_BAUD, REQ_RESTART */
    bool armed;              /* REQ_PROMPT */
    esp_err_t result;
} req_t;

typedef enum { REQ_IDLE = 0, REQ_PENDING, REQ_RUNNING } req_state_t;
static req_t *s_req;                   /* under s_lock */
static req_state_t s_req_state;        /* under s_lock */

/* ---- small helpers -------------------------------------------------------------- */

static uint32_t token_of(gnss_link_lease_t lease)
{
    return (uint32_t)(uintptr_t)lease;
}

static gnss_link_lease_t lease_of(uint32_t token)
{
    return (gnss_link_lease_t)(uintptr_t)token;
}

static bool in_rx_task(void)
{
    return s_rx_task != NULL && xTaskGetCurrentTaskHandle() == s_rx_task;
}

static const char *mode_name(gnss_link_mode_t mode)
{
    switch (mode) {
    case GNSS_LINK_MODE_OFF:
        return "OFF";
    case GNSS_LINK_MODE_STREAM:
        return "STREAM";
    case GNSS_LINK_MODE_COMMAND:
        return "COMMAND";
    case GNSS_LINK_MODE_RAW:
        return "RAW";
    }
    return "?";
}

static TickType_t ms_to_ticks(uint32_t ms)
{
    return ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(ms);
}

static TickType_t ticks_left(TickType_t start, TickType_t total)
{
    if (total == portMAX_DELAY) {
        return portMAX_DELAY;
    }
    TickType_t used = xTaskGetTickCount() - start;
    return used >= total ? 0 : total - used;
}

static esp_err_t check(gnss_link_op_t op, uint32_t token)
{
    taskENTER_CRITICAL(&s_lock);
    esp_err_t err = gnss_link_core_check(&s_core, op, token);
    taskEXIT_CRITICAL(&s_lock);
    return err;
}

static gnss_link_mode_t current_mode(void)
{
    taskENTER_CRITICAL(&s_lock);
    gnss_link_mode_t mode = s_core.mode;
    taskEXIT_CRITICAL(&s_lock);
    return mode;
}

static bool driver_ok(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool ok = s_driver_ok;
    taskEXIT_CRITICAL(&s_lock);
    return ok;
}

static gnss_link_sinks_t sinks_snapshot(void)
{
    taskENTER_CRITICAL(&s_lock);
    gnss_link_sinks_t k = s_sinks;
    taskEXIT_CRITICAL(&s_lock);
    return k;
}

static void count_rx(size_t n)
{
    if (n == 0) {
        return;
    }
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    gnss_link_core_on_rx(&s_core, n, now);
    taskEXIT_CRITICAL(&s_lock);
}

static gnss_link_ev_t map_event(uart_event_type_t type)
{
    switch (type) {
    case UART_DATA:
        return GNSS_LINK_EV_DATA;
    case UART_FIFO_OVF:
        return GNSS_LINK_EV_FIFO_OVF;
    case UART_BUFFER_FULL:
        return GNSS_LINK_EV_BUFFER_FULL;
    case UART_FRAME_ERR:
        return GNSS_LINK_EV_FRAME_ERR;
    case UART_PARITY_ERR:
        return GNSS_LINK_EV_PARITY_ERR;
    case UART_BREAK:
        return GNSS_LINK_EV_BREAK;
    default:
        return GNSS_LINK_EV_OTHER;
    }
}

/* ---- task watchdog (RX task only) ------------------------------------------------ */

static void wdt_set(bool on)
{
#if CONFIG_ESP_TASK_WDT_EN
    if (on == s_wdt_on) {
        return;
    }
    if (on) {
        if (!s_wdt_failed) {
            if (esp_task_wdt_add(NULL) == ESP_OK) {
                s_wdt_on = true;
            } else {
                s_wdt_failed = true;
            }
        }
    } else {
        (void)esp_task_wdt_delete(NULL);
        s_wdt_on = false;
    }
#else
    (void)on;
    (void)s_wdt_on;
    (void)s_wdt_failed;
#endif
}

static void wdt_feed(void)
{
#if CONFIG_ESP_TASK_WDT_EN
    if (s_wdt_on) {
        (void)esp_task_wdt_reset();
    }
#endif
}

/* ---- driver open / reinstall ------------------------------------------------------ */

static void apply_rx_thresholds(uint32_t baud)
{
    /* The only place that touches them, right after every baud change. */
    (void)uart_set_rx_full_threshold(s_port, gnss_link_core_rx_full_threshold(baud));
    (void)uart_set_rx_timeout(s_port, gnss_link_core_rx_timeout(baud));
}

static esp_err_t driver_open_here(uint32_t baud, bool close_first)
{
    taskENTER_CRITICAL(&s_lock);
    s_driver_ok = false;
    s_events = NULL;
    taskEXIT_CRITICAL(&s_lock);
    if (close_first && uart_is_driver_installed(s_port)) {
        (void)uart_driver_delete(s_port);
    }

    const board_desc_t *b = board_get();
    const uart_config_t cfg = {
        .baud_rate = (int)baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    QueueHandle_t q = NULL;
    esp_err_t err = uart_param_config(s_port, &cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(s_port, b->gnss_tx_gpio, b->gnss_rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        /* RX ring in internal RAM, no TX ring (writes block until the bytes
         * are in the FIFO), no pattern detection: lines come from the framer. */
        err = uart_driver_install(s_port, CONFIG_GNSS_LINK_RX_BUF_SIZE, 0, (int)GNSS_LINK_EVENT_QUEUE_LEN, &q,
                                  UART_INTR_FLAGS);
    }
    if (err != ESP_OK) {
        return err;
    }
    apply_rx_thresholds(baud);
    taskENTER_CRITICAL(&s_lock);
    s_events = q;
    s_driver_ok = true;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

typedef struct {
    uint32_t baud;
    bool close_first;
    esp_err_t err;
} open_job_t;

static void open_task(void *arg)
{
    open_job_t *job = arg;
    job->err = driver_open_here(job->baud, job->close_first);
    xSemaphoreGive(s_open_done);
    vTaskDelete(NULL);
}

/* (Re)open the driver from a helper pinned to the RX core and wait for it. */
static esp_err_t driver_open(uint32_t baud, bool close_first)
{
    open_job_t job = { .baud = baud, .close_first = close_first, .err = ESP_FAIL };
    if (xTaskCreatePinnedToCore(open_task, "gnss_open", OPEN_TASK_STACK, &job, uxTaskPriorityGet(NULL), NULL,
                                RX_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    (void)xSemaphoreTake(s_open_done, portMAX_DELAY);
    return job.err;
}

/* ---- reader side ------------------------------------------------------------------- */

static void publish_framing(void)
{
    gnss_framing_stats_t f;
    gnss_framer_get_stats(&s_framer, &f);
    taskENTER_CRITICAL(&s_lock);
    gnss_link_core_set_framing(&s_core, &f);
    taskEXIT_CRITICAL(&s_lock);
}

/* Drop the partial frame or line. Sinks hear about it only from the RX task:
 * before it runs nothing has been framed, and they are RX-task callbacks. */
static void framer_reset(void)
{
    gnss_framer_reset(&s_framer);
    publish_framing();
    if (in_rx_task()) {
        gnss_link_sinks_t k = sinks_snapshot();
        if (k.on_reset != NULL) {
            k.on_reset(k.ctx);
        }
    }
}

static void on_frame(const gnss_frame_t *fr, void *ctx)
{
    (void)ctx;
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    gnss_link_route_t route = gnss_link_core_route(&s_core, fr->kind, now);
    taskEXIT_CRITICAL(&s_lock);
    switch (route) {
    case GNSS_LINK_ROUTE_PUBLISH:
        if (s_cur.on_rtcm != NULL) {
            s_cur.on_rtcm(fr->data, fr->len, fr->rtcm_msg_type, s_cur.ctx);
        }
        break;
    case GNSS_LINK_ROUTE_LINE:
        if (s_cur.on_line != NULL) {
            s_cur.on_line((const char *)fr->data, fr->len, s_cur.ctx);
        }
        break;
    case GNSS_LINK_ROUTE_PROMPT:
        if (s_cur.on_prompt != NULL) {
            s_cur.on_prompt(s_cur.ctx);
        }
        break;
    case GNSS_LINK_ROUTE_HOLD:
    case GNSS_LINK_ROUTE_DROP:
        break;
    }
}

/* Read out and drop what is buffered now; returns the byte count. */
static size_t read_out(void)
{
    size_t total = 0;
    while (total < FLUSH_LIMIT) {
        size_t avail = 0;
        if (uart_get_buffered_data_len(s_port, &avail) != ESP_OK || avail == 0) {
            break;
        }
        size_t want = avail < sizeof(s_rx_buf) ? avail : sizeof(s_rx_buf);
        int n = uart_read_bytes(s_port, s_rx_buf, (uint32_t)want, 0);
        if (n <= 0) {
            break;
        }
        total += (size_t)n;
    }
    return total;
}

/* Deliberate drop (lease release, resync, reinstall, start), counted. */
static size_t drop_input(void)
{
    if (!driver_ok()) {
        return 0;
    }
    size_t n = read_out();
    count_rx(n);
    taskENTER_CRITICAL(&s_lock);
    gnss_link_core_on_flush(&s_core, n);
    taskEXIT_CRITICAL(&s_lock);
    return n;
}

/* After a baud switch or reopen: let the line settle, drop what arrived. */
static void settle_and_discard(uint32_t baud)
{
    vTaskDelay(pdMS_TO_TICKS(GNSS_LINK_BAUD_SETTLE_MS));
    size_t n = read_out();
    count_rx(n);
    taskENTER_CRITICAL(&s_lock);
    gnss_link_core_on_baud(&s_core, baud, n);
    taskEXIT_CRITICAL(&s_lock);
}

/* Events queued while nobody read the queue. count = add them to the counters. */
static void take_pending_events(bool count)
{
    taskENTER_CRITICAL(&s_lock);
    QueueHandle_t q = s_events;
    taskEXIT_CRITICAL(&s_lock);
    if (q == NULL) {
        return;
    }
    uart_event_t ev;
    for (unsigned i = 0; i < 2 * GNSS_LINK_EVENT_QUEUE_LEN && xQueueReceive(q, &ev, 0) == pdTRUE; i++) {
        if (count) {
            gnss_link_ev_t e = map_event(ev.type);
            taskENTER_CRITICAL(&s_lock);
            (void)gnss_link_core_on_event(&s_core, e);
            taskEXIT_CRITICAL(&s_lock);
        }
    }
}

/* One UART event (RX task). Returns true after a buffer-full event. */
static bool handle_event(const uart_event_t *ev)
{
    gnss_link_ev_t e = map_event(ev->type);
    taskENTER_CRITICAL(&s_lock);
    bool reset = gnss_link_core_on_event(&s_core, e);
    taskEXIT_CRITICAL(&s_lock);
    if (reset) {
        framer_reset();   /* never a flush: everything buffered is still read */
    }
    return e == GNSS_LINK_EV_BUFFER_FULL;
}

/* One drain pass (RX task). Returns the wait before the next event. */
static uint32_t drain(bool pull_stash)
{
    gnss_link_pass_t pass;
    gnss_link_pass_begin(&pass);
    s_cur = sinks_snapshot();
    for (;;) {
        size_t avail = 0;
        if (uart_get_buffered_data_len(s_port, &avail) != ESP_OK) {
            break;
        }
        if (avail == 0 && pull_stash) {
            /* After buffer-full the driver may still hold a stashed chunk
             * that only a read moves into the ring. */
            avail = 1;
        }
        pull_stash = false;
        size_t want = gnss_link_pass_next(&pass, avail);
        if (want == 0) {
            break;
        }
        int n = uart_read_bytes(s_port, s_rx_buf, (uint32_t)want, 0);
        if (n <= 0) {
            break;
        }
        gnss_link_pass_add(&pass, (size_t)n);
        count_rx((size_t)n);
        gnss_framer_feed(&s_framer, s_rx_buf, (size_t)n);
    }
    if (pass.done > 0) {
        publish_framing();
    }
    return gnss_link_pass_wait_ms(&pass);
}

/* ---- request execution -------------------------------------------------------------- */

static esp_err_t do_set_baud(uint32_t baud, bool reset_framer)
{
    if (!driver_ok()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = uart_set_baudrate(s_port, baud);
    if (err != ESP_OK) {
        return err;
    }
    apply_rx_thresholds(baud);
    settle_and_discard(baud);
    if (reset_framer) {
        framer_reset();
    }
    return ESP_OK;
}

static void exec_req(req_t *r)
{
    esp_err_t err = ESP_ERR_INVALID_STATE;
    switch (r->op) {
    case REQ_ENTER: {
        uint32_t token = 0;
        taskENTER_CRITICAL(&s_lock);
        err = gnss_link_core_acquire(&s_core, r->mode, r->owner, &token);
        taskEXIT_CRITICAL(&s_lock);
        r->token = token;
        if (err == ESP_OK && r->mode == GNSS_LINK_MODE_RAW && in_rx_task()) {
            wdt_set(false);   /* parks next; the lease holder feeds its own watchdog */
        }
        break;
    }
    case REQ_EXIT: {
        err = check(GNSS_LINK_OP_RELEASE, r->token);
        if (err != ESP_OK) {
            break;
        }
        if (current_mode() == GNSS_LINK_MODE_RAW) {
            take_pending_events(true);   /* they describe the link, whoever read it */
        }
        drop_input();
        gnss_framer_set_prompt_detect(&s_framer, false);
        framer_reset();
        taskENTER_CRITICAL(&s_lock);
        err = gnss_link_core_release(&s_core, r->token, NULL);
        taskEXIT_CRITICAL(&s_lock);
        if (in_rx_task()) {
            wdt_set(true);
        }
        break;
    }
    case REQ_SET_BAUD:
        err = check(GNSS_LINK_OP_SET_BAUD, r->token);
        if (err == ESP_OK) {
            err = do_set_baud(r->baud, current_mode() != GNSS_LINK_MODE_RAW);
        }
        break;
    case REQ_RESYNC:
        err = check(GNSS_LINK_OP_RESYNC, r->token);
        if (err == ESP_OK) {
            drop_input();
            framer_reset();
        }
        break;
    case REQ_RESTART:
        err = check(GNSS_LINK_OP_RESTART, r->token);
        if (err == ESP_OK) {
            drop_input();
            err = driver_open(r->baud, true);
            if (err == ESP_OK) {
                settle_and_discard(r->baud);
            }
            framer_reset();
        }
        break;
    case REQ_PROMPT:
        taskENTER_CRITICAL(&s_lock);
        err = gnss_link_core_set_prompt(&s_core, r->token, r->armed);
        taskEXIT_CRITICAL(&s_lock);
        if (err == ESP_OK) {
            gnss_framer_set_prompt_detect(&s_framer, r->armed);
        }
        break;
    }
    r->result = err;
}

static void wake_rx(void)
{
    taskENTER_CRITICAL(&s_lock);
    QueueHandle_t q = s_events;
    bool parked = s_core.mode == GNSS_LINK_MODE_RAW;
    taskEXIT_CRITICAL(&s_lock);
    if (q != NULL && !parked && uxQueueMessagesWaiting(q) == 0) {
        /* Ends the 20 ms event wait early (ignored as an unknown event
         * type). Only into an empty queue, so it never takes the place of
         * a driver event; with events queued the RX task is not waiting. */
        const uart_event_t wake = { .type = UART_EVENT_MAX };
        (void)xQueueSendToFront(q, &wake, 0);
    }
    xTaskNotifyGiveIndexed(s_rx_task, RX_NOTIFY_IDX);   /* wakes a parked RX task */
}

static esp_err_t run_req(req_t *r)
{
    (void)xSemaphoreTake(s_req_mutex, portMAX_DELAY);
    bool here = s_rx_task == NULL || (r->op == REQ_SET_BAUD && current_mode() == GNSS_LINK_MODE_RAW);
    if (here) {
        exec_req(r);
    } else {
        taskENTER_CRITICAL(&s_lock);
        s_req = r;
        s_req_state = REQ_PENDING;
        taskEXIT_CRITICAL(&s_lock);
        wake_rx();
        for (;;) {
            if (xSemaphoreTake(s_req_done, pdMS_TO_TICKS(REQ_PICKUP_MS)) == pdTRUE) {
                break;
            }
            bool cancelled = false;
            taskENTER_CRITICAL(&s_lock);
            if (s_req_state == REQ_PENDING) {
                s_req_state = REQ_IDLE;   /* never picked up: withdraw it */
                s_req = NULL;
                cancelled = true;
            }
            taskEXIT_CRITICAL(&s_lock);
            if (cancelled) {
                r->result = ESP_ERR_TIMEOUT;
                break;
            }
        }
    }
    xSemaphoreGive(s_req_mutex);
    return r->result;
}

static void serve_request(void)
{
    req_t *r = NULL;
    taskENTER_CRITICAL(&s_lock);
    if (s_req_state == REQ_PENDING) {
        r = s_req;
        s_req_state = REQ_RUNNING;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (r == NULL) {
        return;
    }
    exec_req(r);
    taskENTER_CRITICAL(&s_lock);
    s_req_state = REQ_IDLE;
    s_req = NULL;
    taskEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_req_done);
}

/* ---- RX task ------------------------------------------------------------------------ */

static void rx_task(void *arg)
{
    (void)arg;
    /* gnss_link_start() lets us go once s_rx_task is set. */
    (void)ulTaskNotifyTakeIndexed(RX_NOTIFY_IDX, pdTRUE, portMAX_DELAY);
    uint32_t wait_ms = GNSS_LINK_EVENT_WAIT_MS;
    for (;;) {
        serve_request();

        taskENTER_CRITICAL(&s_lock);
        gnss_link_mode_t mode = s_core.mode;
        QueueHandle_t q = s_driver_ok ? s_events : NULL;
        taskEXIT_CRITICAL(&s_lock);

        if (mode == GNSS_LINK_MODE_RAW) {
            /* The lease holder is the only reader: neither the ring nor the
             * event queue is touched until the release wakes us. */
            wdt_set(false);
            (void)ulTaskNotifyTakeIndexed(RX_NOTIFY_IDX, pdTRUE, portMAX_DELAY);
            wait_ms = GNSS_LINK_EVENT_WAIT_MS;
            continue;
        }
        wdt_set(true);

        if (q == NULL) {
            /* Driver closed after a failed reinstall: wait for the next request. */
            (void)ulTaskNotifyTakeIndexed(RX_NOTIFY_IDX, pdTRUE, pdMS_TO_TICKS(GNSS_LINK_EVENT_WAIT_MS));
            wdt_feed();
            continue;
        }

        uart_event_t ev;
        bool pull_stash = false;
        if (xQueueReceive(q, &ev, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
            pull_stash = handle_event(&ev);
        }
        wait_ms = drain(pull_stash);
        wdt_feed();
    }
}

/* ---- public API ----------------------------------------------------------------------- */

static void sync_init(void)
{
    if (s_sync_ready) {
        return;
    }
    s_slot = xSemaphoreCreateBinaryStatic(&s_slot_buf);
    xSemaphoreGive(s_slot);
    s_tx = xSemaphoreCreateMutexStatic(&s_tx_buf);
    s_req_mutex = xSemaphoreCreateMutexStatic(&s_req_mutex_buf);
    s_req_done = xSemaphoreCreateBinaryStatic(&s_req_done_buf);
    s_open_done = xSemaphoreCreateBinaryStatic(&s_open_done_buf);
    s_sync_ready = true;
}

esp_err_t gnss_link_init(void)
{
    if (s_inited) {
        return ESP_OK;   /* an early boot step may have opened the link already */
    }
    if (!board_is_valid()) {
        /* A rejected pin map may point the UART at flash or PSRAM lines. */
        ESP_LOGE(TAG, "board pin map not validated: GNSS UART left closed");
        return ESP_ERR_INVALID_STATE;
    }
    sync_init();
    const board_desc_t *b = board_get();
    s_port = (uart_port_t)b->gnss_uart_num;

    uint32_t stored = 0;
    esp_err_t serr = settings_get_u32(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_BAUD, &stored);
    uint32_t baud = gnss_link_core_open_baud(serr, stored, CONFIG_GNSS_LINK_DEFAULT_BAUD);
    if (serr == ESP_OK && baud != stored) {
        ESP_LOGW(TAG, "stored baud %u is not a receiver rate: using %u", (unsigned)stored, (unsigned)baud);
    }

    gnss_framer_init(&s_framer, on_frame, NULL);
    esp_err_t err = driver_open(baud, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART%d not opened: %s", b->gnss_uart_num, esp_err_to_name(err));
        return err;
    }
    taskENTER_CRITICAL(&s_lock);
    gnss_link_core_init(&s_core, baud);
    taskEXIT_CRITICAL(&s_lock);
    s_inited = true;
    ESP_LOGI(TAG, "UART%d open: RX GPIO%d TX GPIO%d @ %u baud (%s)", b->gnss_uart_num, b->gnss_rx_gpio,
             b->gnss_tx_gpio, (unsigned)baud, serr == ESP_OK && baud == stored ? "stored" : "default");
    return ESP_OK;
}

esp_err_t gnss_link_set_sinks(const gnss_link_sinks_t *sinks)
{
    if (sinks == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_lock);
    s_sinks = *sinks;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t gnss_link_start(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    (void)xSemaphoreTake(s_req_mutex, portMAX_DELAY);
    if (s_rx_task != NULL) {
        xSemaphoreGive(s_req_mutex);
        return ESP_OK;
    }
    size_t dropped = 0;
    gnss_link_mode_t mode = current_mode();
    if (mode == GNSS_LINK_MODE_STREAM) {
        /* Nobody read the link since it was opened: its events say nothing
         * about the stream, and its bytes are stale. */
        take_pending_events(false);
        dropped = drop_input();
        gnss_framer_reset(&s_framer);
        publish_framing();
    }
    TaskHandle_t t = xTaskCreateStaticPinnedToCore(rx_task, "gnss_rx", CONFIG_GNSS_LINK_RX_TASK_STACK, NULL,
                                                   CONFIG_GNSS_LINK_RX_TASK_PRIO, s_rx_stack, &s_rx_tcb, RX_CORE);
    if (t == NULL) {
        xSemaphoreGive(s_req_mutex);
        ESP_LOGE(TAG, "RX task not created");
        return ESP_ERR_NO_MEM;
    }
    s_rx_task = t;
    xTaskNotifyGiveIndexed(t, RX_NOTIFY_IDX);
    xSemaphoreGive(s_req_mutex);
    ESP_LOGI(TAG, "RX task running on core %d in %s mode (%u stale bytes dropped)", RX_CORE, mode_name(mode),
             (unsigned)dropped);
    return ESP_OK;
}

gnss_link_mode_t gnss_link_get_mode(void)
{
    return current_mode();
}

esp_err_t gnss_link_acquire(gnss_link_mode_t mode, const char *owner, uint32_t timeout_ms,
                            gnss_link_lease_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (mode != GNSS_LINK_MODE_COMMAND && mode != GNSS_LINK_MODE_RAW) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited || in_rx_task()) {
        return ESP_ERR_INVALID_STATE;
    }
    TickType_t start = xTaskGetTickCount();
    TickType_t total = ms_to_ticks(timeout_ms);
    if (xSemaphoreTake(s_slot, total) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    /* No NULL-lease write may still be on the wire when STREAM ends. */
    if (xSemaphoreTake(s_tx, ticks_left(start, total)) != pdTRUE) {
        xSemaphoreGive(s_slot);
        return ESP_ERR_TIMEOUT;
    }
    req_t r = { .op = REQ_ENTER, .mode = mode, .owner = owner };
    esp_err_t err = run_req(&r);
    xSemaphoreGive(s_tx);
    if (err != ESP_OK) {
        xSemaphoreGive(s_slot);
        ESP_LOGW(TAG, "%s lease for %s not granted: %s", mode_name(mode), owner ? owner : "?",
                 esp_err_to_name(err));
        return err;
    }
    *out = lease_of(r.token);
    ESP_LOGI(TAG, "%s lease taken by %s", mode_name(mode), owner ? owner : "?");
    return ESP_OK;
}

esp_err_t gnss_link_release(gnss_link_lease_t lease)
{
    if (!s_inited || in_rx_task()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t token = token_of(lease);
    esp_err_t err = check(GNSS_LINK_OP_RELEASE, token);
    if (err != ESP_OK) {
        return err;
    }
    taskENTER_CRITICAL(&s_lock);
    gnss_link_mode_t was = s_core.mode;
    const char *owner = s_core.owner;
    taskEXIT_CRITICAL(&s_lock);
    req_t r = { .op = REQ_EXIT, .token = token };
    err = run_req(&r);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s lease release failed: %s", mode_name(was), esp_err_to_name(err));
        return err;
    }
    xSemaphoreGive(s_slot);
    ESP_LOGI(TAG, "%s lease released by %s", mode_name(was), owner ? owner : "?");
    return ESP_OK;
}

esp_err_t gnss_link_set_prompt_detect(gnss_link_lease_t lease, bool armed)
{
    if (!s_inited || in_rx_task()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t token = token_of(lease);
    esp_err_t err = check(GNSS_LINK_OP_PROMPT, token);
    if (err != ESP_OK) {
        return err;
    }
    req_t r = { .op = REQ_PROMPT, .token = token, .armed = armed };
    return run_req(&r);
}

esp_err_t gnss_link_set_baud(gnss_link_lease_t lease, uint32_t baud)
{
    if (!gnss_link_core_baud_supported(baud)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited || in_rx_task()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t token = token_of(lease);
    esp_err_t err = check(GNSS_LINK_OP_SET_BAUD, token);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t old = gnss_link_get_baud();
    if (xSemaphoreTake(s_tx, pdMS_TO_TICKS(TX_LOCK_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (driver_ok()) {
        (void)uart_wait_tx_done(s_port, pdMS_TO_TICKS(TX_DRAIN_MS));   /* a command must not be cut */
    }
    req_t r = { .op = REQ_SET_BAUD, .token = token, .baud = baud };
    err = run_req(&r);
    xSemaphoreGive(s_tx);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "host baud %u -> %u", (unsigned)old, (unsigned)baud);
    } else {
        ESP_LOGW(TAG, "host baud %u -> %u failed: %s", (unsigned)old, (unsigned)baud, esp_err_to_name(err));
    }
    return err;
}

esp_err_t gnss_link_send_break(gnss_link_lease_t lease, uint32_t duration_ms)
{
    if (!gnss_link_core_break_ok(duration_ms)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited || in_rx_task()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = check(GNSS_LINK_OP_BREAK, token_of(lease));
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_tx, pdMS_TO_TICKS(TX_LOCK_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!driver_ok()) {
        xSemaphoreGive(s_tx);
        return ESP_ERR_INVALID_STATE;
    }
    (void)uart_wait_tx_done(s_port, pdMS_TO_TICKS(TX_DRAIN_MS));
    /* The driver's own break is limited to 255 bit times; an inverted TX
     * line holds the space condition for as long as needed. */
    err = uart_set_line_inverse(s_port, UART_SIGNAL_TXD_INV);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(duration_ms) + 1);
        err = uart_set_line_inverse(s_port, UART_SIGNAL_INV_DISABLE);
    }
    xSemaphoreGive(s_tx);
    ESP_LOGI(TAG, "break %u ms on TX: %s", (unsigned)duration_ms, esp_err_to_name(err));
    return err;
}

esp_err_t gnss_link_resync(gnss_link_lease_t lease)
{
    if (!s_inited || in_rx_task()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t token = token_of(lease);
    esp_err_t err = check(GNSS_LINK_OP_RESYNC, token);
    if (err != ESP_OK) {
        return err;
    }
    req_t r = { .op = REQ_RESYNC, .token = token };
    err = run_req(&r);
    ESP_LOGI(TAG, "resync: %s", esp_err_to_name(err));
    return err;
}

esp_err_t gnss_link_restart(gnss_link_lease_t lease)
{
    if (!s_inited || in_rx_task()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t token = token_of(lease);
    esp_err_t err = check(GNSS_LINK_OP_RESTART, token);
    if (err != ESP_OK) {
        return err;
    }
    /* The stored rate, not the current one: a scan or a failed baud change
     * may have left the host elsewhere. */
    uint32_t stored = 0;
    esp_err_t serr = settings_get_u32(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_BAUD, &stored);
    uint32_t baud = gnss_link_core_open_baud(serr, stored, CONFIG_GNSS_LINK_DEFAULT_BAUD);
    if (xSemaphoreTake(s_tx, pdMS_TO_TICKS(TX_LOCK_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    req_t r = { .op = REQ_RESTART, .token = token, .baud = baud };
    err = run_req(&r);
    xSemaphoreGive(s_tx);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "UART reinstalled @ %u baud", (unsigned)baud);
    } else {
        ESP_LOGE(TAG, "UART reinstall @ %u baud failed: %s", (unsigned)baud, esp_err_to_name(err));
    }
    return err;
}

uint32_t gnss_link_get_baud(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool off = s_core.mode == GNSS_LINK_MODE_OFF;
    uint32_t baud = s_core.baud;
    taskEXIT_CRITICAL(&s_lock);
    return off ? gnss_link_core_open_baud(ESP_FAIL, 0, CONFIG_GNSS_LINK_DEFAULT_BAUD) : baud;
}

esp_err_t gnss_link_write(gnss_link_lease_t lease, const void *data, size_t len, uint32_t timeout_ms)
{
    if (data == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited || in_rx_task()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t token = token_of(lease);
    TickType_t start = xTaskGetTickCount();
    TickType_t total = ms_to_ticks(timeout_ms);
    if (xSemaphoreTake(s_tx, total) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = check(GNSS_LINK_OP_WRITE, token);
    if (err == ESP_OK && !driver_ok()) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK && len > 0) {
        int n = uart_write_bytes(s_port, data, len);
        if (n > 0) {
            taskENTER_CRITICAL(&s_lock);
            gnss_link_core_on_tx(&s_core, (size_t)n);
            taskEXIT_CRITICAL(&s_lock);
        }
        if (n < 0 || (size_t)n != len) {
            err = ESP_FAIL;
        } else if (uart_wait_tx_done(s_port, ticks_left(start, total)) != ESP_OK) {
            err = ESP_ERR_TIMEOUT;
        }
    }
    xSemaphoreGive(s_tx);
    return err;
}

esp_err_t gnss_link_raw_read(gnss_link_lease_t lease, void *buf, size_t len, size_t *out_len,
                             uint32_t timeout_ms)
{
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (buf == NULL || len == 0 || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited || in_rx_task()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = check(GNSS_LINK_OP_RAW_IO, token_of(lease));
    if (err != ESP_OK) {
        return err;
    }
    if (!driver_ok()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *p = buf;
    int n = uart_read_bytes(s_port, p, 1, ms_to_ticks(timeout_ms));
    if (n <= 0) {
        return ESP_ERR_TIMEOUT;
    }
    size_t got = 1;
    size_t avail = 0;
    if (len > 1 && uart_get_buffered_data_len(s_port, &avail) == ESP_OK && avail > 0) {
        size_t want = avail < len - 1 ? avail : len - 1;
        int m = uart_read_bytes(s_port, p + 1, (uint32_t)want, 0);
        if (m > 0) {
            got += (size_t)m;
        }
    }
    count_rx(got);
    *out_len = got;
    return ESP_OK;
}

esp_err_t gnss_link_raw_flush(gnss_link_lease_t lease)
{
    if (!s_inited || in_rx_task()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = check(GNSS_LINK_OP_RAW_IO, token_of(lease));
    if (err != ESP_OK) {
        return err;
    }
    if (!driver_ok()) {
        return ESP_ERR_INVALID_STATE;
    }
    return uart_flush_input(s_port);
}

esp_err_t gnss_link_get_stats(gnss_link_stats_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    gnss_link_core_snapshot(&s_core, now, out);
    taskEXIT_CRITICAL(&s_lock);
    if (out->mode == GNSS_LINK_MODE_OFF) {
        out->baud = gnss_link_core_open_baud(ESP_FAIL, 0, CONFIG_GNSS_LINK_DEFAULT_BAUD);
    }
    return ESP_OK;
}
