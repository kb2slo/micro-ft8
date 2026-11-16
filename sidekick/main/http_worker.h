#pragma once

// http_worker.h -- the tasks that serve the requests which would otherwise hold the web server's own task: one
// for those that wait on the main board, one for files. The server's task keeps accepting connections and
// answering events and state while either works (docs/architecture/sidekick.md, Tasks and cores). Also the
// check that says when the server's task itself was held, and by what.

#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

typedef esp_err_t (*http_worker_fn)(httpd_req_t *req);

typedef enum {
    HTTP_WORKER_NONE = -1,  // runs on the server's task
    // Sends to the main board and waits on porta_rx for its answer. One task, so these run one at a time,
    // which wait_reply() (host_link.c) needs: it matches a reply to its request by one byte.
    HTTP_WORKER_MAIN_BOARD = 0,
    // Files, the bench push, a firmware update's start and a factory reset: everything that reads the pushed
    // app's flash mapping or unmaps it (app_serve.c), so one task is still the only one that touches it.
    HTTP_WORKER_FILES,
    HTTP_WORKER_COUNT,
} http_worker_id_t;

// Creates the workers and their queues. Call once, before the web server starts. A worker that cannot be
// created leaves its requests on the server's task, as before, which is the direction a failure should go.
void http_worker_start(void);

// From the server's task only: hands `req` to worker `w`, which runs `fn` on a copy of it and answers. On
// ESP_OK the request is answered or will be (by the worker; or busy, 503, when there was no memory for the
// copy, since running it here could overlap the worker's own handler) and the caller returns at once. Only
// when worker `w` was never started does it return an error, and the caller runs `fn` itself, as before.
// `t0_us` is when the server took the request, for the slow-request note.
esp_err_t http_worker_submit(http_worker_id_t w, httpd_req_t *req, http_worker_fn fn, int64_t t0_us);

// Writes `SK slow <method> <uri> <ms> ms <client>` into the event ring when a request took longer than
// HOST_LINK_SLOW_MS from `t0_us`: on a worker that is its wait in the queue as well as its handling.
void http_worker_note_slow(httpd_req_t *req, int64_t t0_us);

// From the server's task, after each request it ran itself: what the held check below reports it spent.
void http_worker_note_inline(httpd_req_t *req, int64_t ms);

// From the main loop, once a second: queues a no-op on the server's task (httpd_queue_work()), which runs it
// at the top of its next pass. If it ran more than HOST_LINK_SLOW_MS late, the server's task was held, and
// `SK httpd held <ms> ms: <n> req <ms> ms, top <method> <uri>` goes into the event ring: the requests it ran
// itself since the last check, their time, and the longest. Their time well short of the hold means it was
// held outside any handler: reading a request, or in its own loop.
void http_worker_watch(httpd_handle_t server);

// The most requests waiting for worker `w` at once since boot, its stack never used (bytes), and the longest
// hold of the server's task the check has seen (ms).
uint32_t http_worker_queue_peak(http_worker_id_t w);
uint32_t http_worker_stack_free(http_worker_id_t w);
uint32_t http_worker_held_peak_ms(void);
