#pragma once

// net_health.h -- the sidekick's TCP connections as lwIP holds them, sampled once a second, so a client that
// could not connect can be told apart from a slow reply after the fact (docs/architecture/sidekick.md, Tasks
// and cores). lwIP drops a new connection's SYN without a word when its connection slots are all in use or a
// listening socket's queue of connections not yet accepted is full; the client only sees its connect time out.

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t used;     // TCP connection slots in use (every state but LISTEN), of max
    uint32_t max;      // the slots lwIP has (CONFIG_LWIP_MAX_ACTIVE_TCP)
    uint32_t held;     // of `used`, those lwIP cannot take back for a new connection: syn, est, cw, and bound
    uint32_t syn;      // half open: SYN answered, waiting for the client's ACK
    uint32_t est;      // established
    uint32_t cw;       // CLOSE_WAIT: the client closed, the server has not yet
    uint32_t fin;      // FIN_WAIT_1/2, CLOSING, LAST_ACK: closing, and taken back when slots run out
    uint32_t tw;       // TIME_WAIT: taken back first when slots run out
    uint32_t backlog;  // connections waiting to be accepted, on the fullest listening socket
    uint32_t backlog_of;  // that socket's limit
} net_tcp_t;

// From the main loop, once a second: counts, keeps the peaks, and writes `SK tcp full ...` into the event
// ring when the slots lwIP cannot take back fill, or a listening socket's queue does (once, until they
// drain). A slot in TIME_WAIT or closing is not full: tcp_alloc() takes the oldest back for a new connection,
// so 16 in use of 16 with most in TIME_WAIT drops nothing.
void net_health_sample(void);

// The last sample, and the peaks since boot.
net_tcp_t net_health_last(void);
uint32_t  net_health_held_peak(void);
uint32_t  net_health_backlog_peak(void);

// `tcp=<used>/<max> held=.. syn=.. est=.. cw=.. fin=.. tw=.. bk=<n>/<of> peak=<held>/<backlog>`, for the alive line.
void net_health_str(char *out, size_t cap);
