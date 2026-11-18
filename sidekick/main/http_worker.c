// The workers that serve requests off the web server's own task (http_worker.h; docs/architecture/sidekick.md,
// Tasks and cores). esp_http_server runs every handler on its one task, and that task also accepts new
// connections, so a handler that waited, on the main board or on a client slow to take its reply, stopped
// every client, connections included. Such a request is handed here as an async copy
// (httpd_req_async_handler_begin()); the server's task goes back to its other sockets, and the socket being
// answered is left alone until the copy is completed.

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
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
// A sender runs only files, events and state, none of which waits on the main board or touches mbedtls. Its
// stack never used is in the alive line (sstack) and /api/config (sk_send_stack_free).
#define SENDER_STACK 6144
// A check of the server's task that has not come back in this long is given up and reported.
#define WATCH_GIVE_UP_US (30 * 1000 * 1000)
#define WORKER_TASKS_MAX HTTP_SENDERS
// The server's own send timeout (HTTPD_DEFAULT_CONFIG()'s send_wait_timeout, which wifi_prov.c keeps), put back
// on a socket after a request whose deadline shortened it.
#define SERVER_SEND_WAIT_MS 5000

typedef struct {
    httpd_req_t   *req;
    http_worker_fn fn;
    int64_t        t0_us;
    bool           file;  // counts toward HTTP_FILE_SENDERS
} work_t;

typedef struct {
    const char       *name;
    int               ntasks;
    uint32_t          stack;
    QueueHandle_t     queue;
    TaskHandle_t      tasks[WORKER_TASKS_MAX];
    volatile uint32_t queue_peak;
} worker_t;

static worker_t s_workers[HTTP_WORKER_COUNT] = {
    [HTTP_WORKER_MAIN_BOARD] = { .name = "http_main_board", .ntasks = 1, .stack = WORKER_STACK },
    [HTTP_WORKER_SENDERS]    = { .name = "http_send", .ntasks = HTTP_SENDERS, .stack = SENDER_STACK },
    [HTTP_WORKER_FILES]      = { .name = "http_files" },  // no tasks of its own: the senders'
};

// Files in the senders' queue or being sent, at most HTTP_FILE_SENDERS; the rest wait in s_files_wait, in order,
// and go into the queue as one finishes. Under s_files_lock: the server's task submits, a sender finishes.
static SemaphoreHandle_t s_files_lock;
static QueueHandle_t     s_files_wait;
static int               s_files_active;

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

// Answers a handed-over request busy and hands its socket back: the page asks again.
static void answer_busy(httpd_req_t *copy)
{
    httpd_resp_set_status(copy, "503 Service Unavailable");
    httpd_resp_set_type(copy, "application/json");
    httpd_resp_sendstr(copy, "{\"ok\":false,\"why\":\"sidekick busy\"}");
    http_worker_send_end(httpd_req_to_sockfd(copy));
    httpd_req_async_handler_complete(copy);
}

// A file has been sent: the next one waiting takes its place in the senders' queue, or there is one fewer.
static void file_done(void)
{
    xSemaphoreTake(s_files_lock, portMAX_DELAY);
    work_t next;
    bool   moved = false;
    while (!moved && xQueueReceive(s_files_wait, &next, 0) == pdTRUE) {
        moved = xQueueSend(s_workers[HTTP_WORKER_SENDERS].queue, &next, 0) == pdTRUE;
        if (!moved) {
            answer_busy(next.req);  // not reachable: the queue holds every socket the server has
        }
    }
    if (!moved) {
        s_files_active--;
    }
    xSemaphoreGive(s_files_lock);
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
    http_worker_send_end(fd);
    httpd_req_async_handler_complete(w->req);
    if (err != ESP_OK) {
        httpd_sess_trigger_close(hd, fd);
    }
    if (w->file) {
        file_done();
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
        if (wk->queue || wk->ntasks == 0) {
            continue;
        }
        wk->queue = xQueueCreate(WORKER_QUEUE_LEN, sizeof(work_t));
        if (!wk->queue) {
            ESP_LOGE(TAG, "%s not started: its requests stay on the server's task", wk->name);
            continue;
        }
        // The server task's priority (5, esp_http_server's default), so none starves another. Not pinned: the
        // scheduler spreads them over both cores.
        for (int t = 0; t < wk->ntasks; ++t) {
            char name[16];
            snprintf(name, sizeof(name), wk->ntasks > 1 ? "%s%d" : "%s", wk->name, t);
            if (xTaskCreate(worker_task, name, wk->stack, wk, 5, &wk->tasks[t]) != pdPASS) {
                wk->tasks[t] = NULL;
                ESP_LOGE(TAG, "%s not started", name);
            }
        }
        if (i == HTTP_WORKER_SENDERS && wk->tasks[0]) {
            s_files_lock = xSemaphoreCreateMutex();
            s_files_wait = xQueueCreate(WORKER_QUEUE_LEN, sizeof(work_t));
            if (!s_files_lock || !s_files_wait) {
                ESP_LOGE(TAG, "files not limited: no memory for their queue");  // they share the pool unlimited
            }
        }
        if (!wk->tasks[0]) {
            // Not even one: leave its requests on the server's task. Any other that did start only waits.
            vQueueDelete(wk->queue);
            wk->queue = NULL;
            ESP_LOGE(TAG, "%s not started: its requests stay on the server's task", wk->name);
        }
    }
}

esp_err_t http_worker_submit(http_worker_id_t id, httpd_req_t *req, http_worker_fn fn, int64_t t0_us)
{
    const bool file = id == HTTP_WORKER_FILES;
    const http_worker_id_t to = file ? HTTP_WORKER_SENDERS : id;
    if (id < 0 || id >= HTTP_WORKER_COUNT || !s_workers[to].queue) {
        return ESP_ERR_INVALID_STATE;
    }
    worker_t *wk = &s_workers[to];
    httpd_req_t *copy = NULL;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        // No memory for the copy. Running it here instead could overlap the worker's own handler: a second
        // wait on the main board's one reply slot. So it is answered busy, on this task, and the page asks
        // again.
        ESP_LOGW(TAG, "%s: no memory to hand %s over, answered busy", wk->name, req->uri);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"why\":\"sidekick busy\"}");
        http_worker_send_end(httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    work_t w = { .req = copy, .fn = fn, .t0_us = t0_us, .file = file && s_files_lock && s_files_wait };
    bool queued;
    if (w.file) {
        xSemaphoreTake(s_files_lock, portMAX_DELAY);
        if (s_files_active < HTTP_FILE_SENDERS) {
            queued = xQueueSend(wk->queue, &w, 0) == pdTRUE;
            if (queued) {
                s_files_active++;
            }
        } else {
            queued = xQueueSend(s_files_wait, &w, 0) == pdTRUE;
            const uint32_t waiting = (uint32_t)uxQueueMessagesWaiting(s_files_wait);
            if (queued && waiting > s_workers[HTTP_WORKER_FILES].queue_peak) {
                s_workers[HTTP_WORKER_FILES].queue_peak = waiting;
            }
        }
        xSemaphoreGive(s_files_lock);
    } else {
        queued = xQueueSend(wk->queue, &w, 0) == pdTRUE;
    }
    if (!queued) {
        // Not reachable while the queues are as deep as the server has sockets, but if it were: the copy owns
        // the request now, and running it here could overlap the worker's own handler, so it is answered
        // busy instead and the page asks again.
        ESP_LOGW(TAG, "%s queue full: %s answered busy", wk->name, req->uri);
        answer_busy(copy);
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
    if (id < 0 || id >= HTTP_WORKER_COUNT) {
        return 0;
    }
    uint32_t least = 0;
    bool     any = false;
    for (int t = 0; t < s_workers[id].ntasks; ++t) {
        if (s_workers[id].tasks[t]) {
            const uint32_t f = (uint32_t)uxTaskGetStackHighWaterMark(s_workers[id].tasks[t]);
            if (!any || f < least) {
                least = f;
            }
            any = true;
        }
    }
    return least;
}

int http_worker_sender_index(void)
{
    const TaskHandle_t self = xTaskGetCurrentTaskHandle();
    for (int t = 0; t < HTTP_SENDERS; ++t) {
        if (s_workers[HTTP_WORKER_SENDERS].tasks[t] == self) {
            return t;
        }
    }
    return -1;
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

// Each reply's send deadline (HTTP_SEND_DEADLINE_MS). Per socket, indexed from LWIP_SOCKET_OFFSET: `armed` is set
// by the dispatcher on the server's task before the request is answered, and `started_us` by the send itself
// on whichever task answers it. One request is answered on a socket at a time (the server leaves a socket
// alone while its async copy is out), so no lock.
typedef struct {
    volatile bool    armed;
    volatile int64_t started_us;
} send_clock_t;

static send_clock_t      s_send[CONFIG_LWIP_MAX_SOCKETS];
static volatile uint32_t s_send_cuts;

static send_clock_t *send_clock(int fd)
{
    const int i = fd - LWIP_SOCKET_OFFSET;
    return (i >= 0 && i < CONFIG_LWIP_MAX_SOCKETS) ? &s_send[i] : NULL;
}

static void set_send_timeout(int fd, int64_t ms)
{
    const struct timeval tv = { .tv_sec = (time_t)(ms / 1000), .tv_usec = (suseconds_t)((ms % 1000) * 1000) };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

// esp_http_server's own send (httpd_default_send()), but no single send may run past the request's deadline:
// the socket's timeout is cut to what is left before each one, and once nothing is left the send fails, which
// ends the reply and closes the connection. lwIP returns what it wrote when the timeout passes, so a client
// that takes a little at a time is cut within a send of its deadline.
static int deadline_send(httpd_handle_t hd, int fd, const char *buf, size_t len, int flags)
{
    (void)hd;
    if (!buf) {
        return HTTPD_SOCK_ERR_INVALID;
    }
    send_clock_t *c = send_clock(fd);
    if (c && c->armed) {
        const int64_t now = esp_timer_get_time();
        if (!c->started_us) {
            c->started_us = now;
        }
        const int64_t left_ms = HTTP_SEND_DEADLINE_MS - (now - c->started_us) / 1000;
        if (left_ms <= 0) {
            c->armed = false;  // one line a reply
            s_send_cuts++;
            char line[64];
            snprintf(line, sizeof(line), "SK send cut at %d ms", HTTP_SEND_DEADLINE_MS);
            host_link_log(line);
            return HTTPD_SOCK_ERR_FAIL;
        }
        set_send_timeout(fd, left_ms);
    }
    const int ret = send(fd, buf, len, flags);
    if (ret < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? HTTPD_SOCK_ERR_TIMEOUT
                                                                            : HTTPD_SOCK_ERR_FAIL;
    }
    return ret;
}

esp_err_t http_worker_session_open(httpd_handle_t hd, int sockfd)
{
    send_clock_t *c = send_clock(sockfd);
    if (c) {
        c->armed = false;
        c->started_us = 0;
    }
    // A connection without it only loses the deadline; it is still served.
    httpd_sess_set_send_override(hd, sockfd, deadline_send);
    return ESP_OK;
}

void http_worker_send_begin(int sockfd)
{
    send_clock_t *c = send_clock(sockfd);
    if (c) {
        c->started_us = 0;
        c->armed = true;
    }
}

void http_worker_send_end(int sockfd)
{
    send_clock_t *c = send_clock(sockfd);
    if (c && c->started_us) {
        // The next request on this connection starts from the server's own send timeout, not what was left.
        set_send_timeout(sockfd, SERVER_SEND_WAIT_MS);
    }
    if (c) {
        c->armed = false;
        c->started_us = 0;
    }
}

uint32_t http_worker_send_cuts(void)
{
    return s_send_cuts;
}
