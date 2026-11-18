#pragma once

// http_worker.h -- the tasks that serve requests off the web server's own task, so it keeps accepting
// connections and reading requests while one of them waits (docs/architecture/sidekick.md, Tasks and cores):
// one worker for the requests that wait on the main board, and a pool of senders for files, events and state,
// so a client slow to take its reply holds one sender and not every client. Also each reply's send deadline,
// and the check that says when the server's task itself was held, and by what.

#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

typedef esp_err_t (*http_worker_fn)(httpd_req_t *req);

typedef enum {
    HTTP_WORKER_NONE = -1,  // runs on the server's task
    // Sends to the main board and waits on porta_rx for its answer. One task, so these run one at a time,
    // which wait_reply() (host_link.c) needs: it matches a reply to its request by one byte.
    HTTP_WORKER_MAIN_BOARD = 0,
    // Events and state: HTTP_SENDERS tasks reading one queue, so a free one takes the next request. A
    // client slow to take its reply holds the one sending to it (docs/architecture/sidekick.md, Tasks and cores; a single worker for files,
    // tried on 2026-11-16, was judged under a load with no slow client in it).
    HTTP_WORKER_SENDERS,
    // Files, sent by the same pool, but never more than HTTP_FILE_SENDERS of them queued or sending at once: the
    // rest wait their turn, so a page loaded slowly, or several, always leave a sender for events and state.
    HTTP_WORKER_FILES,
    HTTP_WORKER_COUNT,
} http_worker_id_t;

// The senders in the pool, and how many of them files may hold at once.
#define HTTP_SENDERS 3
#define HTTP_FILE_SENDERS 2

// How long one reply may take to send, from its first byte, before its connection is closed: without it a
// client that keeps taking a few bytes holds its task for as long as it likes: the server sends again after
// every partial send, so the socket's own send timeout ends a reply only when nothing moves at all
// (tools/sidekick_load/README.md, A slow client).
#define HTTP_SEND_DEADLINE_MS 20000

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

// The most requests waiting for worker `w` at once since boot (for HTTP_WORKER_FILES, files waiting their turn),
// its stack never used (bytes; for the senders, the least of theirs), and the longest hold of the server's task
// the check has seen (ms).
uint32_t http_worker_queue_peak(http_worker_id_t w);
uint32_t http_worker_stack_free(http_worker_id_t w);
uint32_t http_worker_held_peak_ms(void);

// Which sender the calling task is, 0 to HTTP_SENDERS - 1, or -1 for any other task: a sender's handler uses
// it to pick the buffer that is its own.
int http_worker_sender_index(void);

// The web server's open_fn: gives each new connection the send that keeps HTTP_SEND_DEADLINE_MS.
esp_err_t http_worker_session_open(httpd_handle_t hd, int sockfd);
// A request on `sockfd` begins (its deadline counts from its first byte sent) or ends (the socket's own send
// timeout is put back). The dispatcher calls begin; whoever answers the request calls end.
void http_worker_send_begin(int sockfd);
void http_worker_send_end(int sockfd);
// Replies cut at their deadline since boot.
uint32_t http_worker_send_cuts(void);
