// The worker that serves requests which wait on the main board (http_worker.h; docs/architecture/sidekick.md,
// Tasks and cores). esp_http_server runs every handler on its one task, and that task also accepts new
// connections, so a handler waiting seconds for the main board stopped every client, connections included.
// Such a request is handed here as an async copy (httpd_req_async_handler_begin()); the server's task goes
// back to its other sockets, and the socket being answered is left alone until the copy is completed.
//
// One worker, so main-board requests still run one at a time: wait_reply() (host_link.c) matches a reply to
// its request by one byte and holds one reply slot, which needs exactly that.

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
// is left unused (http_worker_stack_free()), which is what a smaller figure would be chosen from.
#define WORKER_STACK 8192

typedef struct {
    httpd_req_t   *req;
    http_worker_fn fn;
    int64_t        t0_us;
} work_t;

static QueueHandle_t s_queue;
static TaskHandle_t  s_task;
static volatile uint32_t s_queue_peak;

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
    (void)arg;
    work_t w;
    for (;;) {
        if (xQueueReceive(s_queue, &w, portMAX_DELAY) == pdTRUE) {
            run(&w);
        }
    }
}

void http_worker_start(void)
{
    if (s_queue) {
        return;
    }
    s_queue = xQueueCreate(WORKER_QUEUE_LEN, sizeof(work_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "no queue: main-board requests stay on the server's task");
        return;
    }
    // The server task's priority (5, esp_http_server's default), so neither starves the other.
    if (xTaskCreate(worker_task, "http_worker", WORKER_STACK, NULL, 5, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        ESP_LOGE(TAG, "no task: main-board requests stay on the server's task");
    }
}

esp_err_t http_worker_submit(httpd_req_t *req, http_worker_fn fn, int64_t t0_us)
{
    if (!s_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    httpd_req_t *copy = NULL;
    const esp_err_t err = httpd_req_async_handler_begin(req, &copy);
    if (err != ESP_OK) {
        return err;  // nothing handed over: the caller runs it, as before
    }
    work_t w = { .req = copy, .fn = fn, .t0_us = t0_us };
    if (xQueueSend(s_queue, &w, 0) != pdTRUE) {
        // Not reachable while the queue is as deep as the server has sockets, but if it were: the copy owns
        // the request now, and running it here could overlap the worker's own wait on the main board, so
        // it is answered busy instead and the page asks again.
        ESP_LOGW(TAG, "queue full: %s answered busy", req->uri);
        httpd_resp_set_status(copy, "503 Service Unavailable");
        httpd_resp_set_type(copy, "application/json");
        httpd_resp_sendstr(copy, "{\"ok\":false,\"why\":\"sidekick busy\"}");
        httpd_req_async_handler_complete(copy);
        return ESP_OK;
    }
    const uint32_t waiting = (uint32_t)uxQueueMessagesWaiting(s_queue);
    if (waiting > s_queue_peak) {
        s_queue_peak = waiting;
    }
    return ESP_OK;
}

uint32_t http_worker_queue_peak(void)
{
    return s_queue_peak;
}

uint32_t http_worker_stack_free(void)
{
    return s_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) : 0;
}
