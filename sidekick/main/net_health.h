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
    uint32_t syn;      // half open: SYN answered, waiting for the client's ACK
    uint32_t est;      // established
    uint32_t closing;  // FIN_WAIT, CLOSING, LAST_ACK, CLOSE_WAIT
    uint32_t tw;       // TIME_WAIT
    uint32_t backlog;  // connections waiting to be accepted, on the fullest listening socket
    uint32_t backlog_of;  // that socket's limit
} net_tcp_t;

// From the main loop, once a second: counts, keeps the peaks, and writes `SK tcp full ...` into the event
// ring when the slots or a listening socket's queue fill (once, until they drain).
void net_health_sample(void);

// The last sample, and the peaks since boot.
net_tcp_t net_health_last(void);
uint32_t  net_health_used_peak(void);
uint32_t  net_health_backlog_peak(void);

// `tcp=<used>/<max> syn=.. est=.. cls=.. tw=.. bk=<n>/<of> peak=<used>/<backlog>`, for the alive line.
void net_health_str(char *out, size_t cap);
