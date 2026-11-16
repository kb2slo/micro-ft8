#pragma once

// http_worker.h -- the task that serves the requests which wait on the main board, so the web server's own
// task keeps accepting connections and answering events, state and files while one of them waits
// (docs/architecture/sidekick.md, Tasks and cores).

#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

typedef esp_err_t (*http_worker_fn)(httpd_req_t *req);

// Creates the worker task and its queue. Call once, before the web server starts. If it cannot, every
// request runs on the server's task as before, which is the direction a failure should go.
void http_worker_start(void);

// From the server's task only: hands `req` to the worker, which runs `fn` on a copy of it and answers. On
// ESP_OK the request is the worker's and the caller returns at once; otherwise nothing was handed over and
// the caller runs `fn` itself. `t0_us` is when the server took the request, for the slow-request note.
esp_err_t http_worker_submit(httpd_req_t *req, http_worker_fn fn, int64_t t0_us);

// Writes `SK slow <method> <uri> <ms> ms <client>` into the event ring when a request took longer than
// HOST_LINK_SLOW_MS from `t0_us`: on the worker that is its wait in the queue as well as its handling.
void http_worker_note_slow(httpd_req_t *req, int64_t t0_us);

// The most requests waiting for the worker at once since boot, and the worker task's stack never used.
uint32_t http_worker_queue_peak(void);
uint32_t http_worker_stack_free(void);
