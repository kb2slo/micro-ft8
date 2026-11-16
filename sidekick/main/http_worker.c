// The workers that serve requests which would hold the web server's own task (http_worker.h;
// docs/architecture/sidekick.md, Tasks and cores). esp_http_server runs every handler on its one task, and that
// task also accepts new connections, so a handler waiting seconds for the main board, or sending a file to a
// slow client, stopped every client, connections included. Such a request is handed to a worker as an async
// copy (httpd_req_async_handler_begin()); the server's task goes back to its other sockets, and the socket
// being answered is left alone until the copy is completed.

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "host_link.h"
#include "http_worker.h"

static const char *TAG = "http_worker";

// One async request at most per open socket, and the server holds 10 (max_open_sockets, wifi_prov.c), so a
// queue this deep is never full; if it were, the request is answered busy (http_worker_submit()).
#define WORKER_QUEUE_LEN 10
// The server task's own stack (wifi_prov.c), since the same handlers run here. The alive line reports what
// each leaves unused (http_worker_stack_free()), which is what a smaller figure would be chosen from.
#define WORKER_STACK 8192
// A check of the server's task that has not come back in this long is given up and reported.
#define WATCH_GIVE_UP_US (30 * 1000 * 1000)

typedef struct {
    httpd_req_t   *req;
    http_worker_fn fn;
    int64_t        t0_us;
} work_t;

typedef struct {
    const char       *name;
    QueueHandle_t     queue;
    TaskHandle_t      task;
    volatile uint32_t queue_peak;
} worker_t;

static worker_t s_workers[HTTP_WORKER_COUNT] = {
    [HTTP_WORKER_MAIN_BOARD] = { .name = "http_main_board" },
    [HTTP_WORKER_FILES]      = { .name = "http_files" },
};

static const char *method_name(int m)
{
    switch (m) {
    case HTTP_GET:     return "GET";
    case HTTP_POST:    return "POST";
    case HTTP_PUT:     return "PUT";
    case HTTP_DELETE:  return "DELETE";
    case HTTP_HEAD:    return "HEAD";
    case HTTP_OPTIONS: return "OPTIONS";
    case HTTP_PATCH:   return "PATCH";
    default:           return "?";
    }
}

// The client's IPv4 address, or "" if the socket cannot say. With IPv6 built in, the server's socket is
// IPv6 and an IPv4 client shows as ::ffff:a.b.c.d, so that form is unwrapped.
static void peer_ip(httpd_req_t *req, char *out, size_t cap)
{
    out[0] = '\0';
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &len) != 0) {
        return;
    }
    if (addr.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)&addr)->sin_addr, out, cap);
    }
#if CONFIG_LWIP_IPV6
    else if (addr.ss_family == AF_INET6) {
        const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)&addr;
        if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr)) {
            inet_ntop(AF_INET, &a6->sin6_addr.s6_addr[12], out, cap);
        } else {
            inet_ntop(AF_INET6, &a6->sin6_addr, out, cap);
        }
    }
#endif
}

void http_worker_note_slow(httpd_req_t *req, int64_t t0_us)
{
    const int64_t ms = (esp_timer_get_time() - t0_us) / 1000;
    if (ms <= HOST_LINK_SLOW_MS) {
        return;
    }
    char ip[40];
    peer_ip(req, ip, sizeof(ip));
    // A log line holds PORTA_EVENT_TEXT_MAX (64) and host_link_log() cuts it there: the URI is cut first to
    // leave room for the time and client. This buffer holds everything, so the format cannot truncate.
    char line[128];
    snprintf(line, sizeof(line), "SK slow %s %.24s %lld ms %s", method_name(req->method), req->uri, (long long)ms, ip);
    host_link_log(line);
}

static void run(work_t *w)
{
    const esp_err_t err = w->fn(w->req);
    http_worker_note_slow(w->req, w->t0_us);
    // Read before completing, which frees the request. A handler that fails has the server close the
    // connection when it runs on the server's task; here that is ours to ask for, after the socket is
    // handed back, so the close cannot land on a session slot the server has reused.
    httpd_handle_t hd = w->req->handle;
    const int fd = httpd_req_to_sockfd(w->req);
    httpd_req_async_handler_complete(w->req);
    if (err != ESP_OK) {
        httpd_sess_trigger_close(hd, fd);
    }
}

static void worker_task(void *arg)
{
    worker_t *wk = (worker_t *)arg;
    work_t w;
    for (;;) {
        if (xQueueReceive(wk->queue, &w, portMAX_DELAY) == pdTRUE) {
            run(&w);
        }
    }
}

void http_worker_start(void)
{
    for (int i = 0; i < HTTP_WORKER_COUNT; ++i) {
        worker_t *wk = &s_workers[i];
        if (wk->queue) {
            continue;
        }
        wk->queue = xQueueCreate(WORKER_QUEUE_LEN, sizeof(work_t));
        // The server task's priority (5, esp_http_server's default), so none starves another.
        if (!wk->queue || xTaskCreate(worker_task, wk->name, WORKER_STACK, wk, 5, &wk->task) != pdPASS) {
            if (wk->queue) {
                vQueueDelete(wk->queue);
                wk->queue = NULL;
            }
            ESP_LOGE(TAG, "%s not started: its requests stay on the server's task", wk->name);
        }
    }
}

esp_err_t http_worker_submit(http_worker_id_t id, httpd_req_t *req, http_worker_fn fn, int64_t t0_us)
{
    if (id < 0 || id >= HTTP_WORKER_COUNT || !s_workers[id].queue) {
        return ESP_ERR_INVALID_STATE;
    }
    worker_t *wk = &s_workers[id];
    httpd_req_t *copy = NULL;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        // No memory for the copy. Running it here instead could overlap the worker's own handler: a second
        // wait on the main board's one reply slot, or a file read while the files worker unmaps the pushed app.
        // So it is answered busy, on this task, and the page asks again.
        ESP_LOGW(TAG, "%s: no memory to hand %s over, answered busy", wk->name, req->uri);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"why\":\"sidekick busy\"}");
        return ESP_OK;
    }
    work_t w = { .req = copy, .fn = fn, .t0_us = t0_us };
    if (xQueueSend(wk->queue, &w, 0) != pdTRUE) {
        // Not reachable while the queue is as deep as the server has sockets, but if it were: the copy owns
        // the request now, and running it here could overlap the worker's own handler, so it is answered
        // busy instead and the page asks again.
        ESP_LOGW(TAG, "%s queue full: %s answered busy", wk->name, req->uri);
        httpd_resp_set_status(copy, "503 Service Unavailable");
        httpd_resp_set_type(copy, "application/json");
        httpd_resp_sendstr(copy, "{\"ok\":false,\"why\":\"sidekick busy\"}");
        httpd_req_async_handler_complete(copy);
        return ESP_OK;
    }
    const uint32_t waiting = (uint32_t)uxQueueMessagesWaiting(wk->queue);
    if (waiting > wk->queue_peak) {
        wk->queue_peak = waiting;
    }
    return ESP_OK;
}

uint32_t http_worker_queue_peak(http_worker_id_t id)
{
    return (id >= 0 && id < HTTP_WORKER_COUNT) ? s_workers[id].queue_peak : 0;
}

uint32_t http_worker_stack_free(http_worker_id_t id)
{
    if (id < 0 || id >= HTTP_WORKER_COUNT || !s_workers[id].task) {
        return 0;
    }
    return (uint32_t)uxTaskGetStackHighWaterMark(s_workers[id].task);
}

// The held check. What the server's task ran itself since the last check, written by
// http_worker_note_inline() and read and reset by watch_landed(): both on the server's task, so no lock.
static uint32_t s_inline_n;
static int64_t  s_inline_ms;
static int64_t  s_inline_top_ms;
static char     s_inline_top[40];
// The check in flight: written by the main loop before queuing it, read by watch_landed() after.
static volatile int64_t s_watch_sent_us;
static volatile bool    s_watch_out;
static volatile uint32_t s_held_peak_ms;

void http_worker_note_inline(httpd_req_t *req, int64_t ms)
{
    s_inline_n++;
    s_inline_ms += ms;
    if (ms >= s_inline_top_ms) {
        s_inline_top_ms = ms;
        snprintf(s_inline_top, sizeof(s_inline_top), "%s %.30s", method_name(req->method), req->uri);
    }
}

static void watch_landed(void *arg)
{
    (void)arg;
    const int64_t held_ms = (esp_timer_get_time() - s_watch_sent_us) / 1000;
    if (held_ms > s_held_peak_ms) {
        s_held_peak_ms = (uint32_t)held_ms;
    }
    if (held_ms > HOST_LINK_SLOW_MS) {
        char line[128];
        snprintf(line, sizeof(line), "SK httpd held %lld ms: %lu req %lld ms, top %s", (long long)held_ms,
                 (unsigned long)s_inline_n, (long long)s_inline_ms, s_inline_n ? s_inline_top : "-");
        host_link_log(line);
    }
    s_inline_n = 0;
    s_inline_ms = 0;
    s_inline_top_ms = 0;
    s_inline_top[0] = '\0';
    s_watch_out = false;
}

void http_worker_watch(httpd_handle_t server)
{
    if (!server) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (s_watch_out) {
        if (now - s_watch_sent_us < WATCH_GIVE_UP_US) {
            return;  // still held, or still on its way: it reports when it lands
        }
        host_link_log("SK httpd held over 30 s: check given up");
    }
    s_watch_sent_us = now;
    s_watch_out = true;
    if (httpd_queue_work(server, watch_landed, NULL) != ESP_OK) {
        s_watch_out = false;
    }
}

uint32_t http_worker_held_peak_ms(void)
{
    return s_held_peak_ms;
}
