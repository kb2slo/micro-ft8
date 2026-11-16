// The sidekick's TCP connections, sampled (net_health.h). lwIP's connection lists may only be walked on its own
// thread, and this build has no core locking, so the walk is a tcpip_api_call(): the caller waits the few
// microseconds lwIP's thread takes to count at most CONFIG_LWIP_MAX_ACTIVE_TCP entries.

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "lwip/priv/tcp_priv.h"
#include "lwip/priv/tcpip_priv.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"

#include "host_link.h"
#include "net_health.h"
#include "porta_proto.h"

typedef struct {
    struct tcpip_api_call_data call;  // first: lwIP hands back this pointer
    net_tcp_t t;
} count_call_t;

static net_tcp_t s_last;
static uint32_t  s_used_peak;
static uint32_t  s_backlog_peak;
static bool      s_full_logged;

static err_t count_on_lwip(struct tcpip_api_call_data *call)
{
    net_tcp_t *t = &((count_call_t *)call)->t;
    for (struct tcp_pcb *p = tcp_active_pcbs; p; p = p->next) {
        t->used++;
        if (p->state == SYN_RCVD || p->state == SYN_SENT) {
            t->syn++;
        } else if (p->state == ESTABLISHED) {
            t->est++;
        } else {
            t->closing++;
        }
    }
    for (struct tcp_pcb *p = tcp_tw_pcbs; p; p = p->next) {
        t->used++;
        t->tw++;
    }
    for (struct tcp_pcb *p = tcp_bound_pcbs; p; p = p->next) {
        t->used++;
    }
#if TCP_LISTEN_BACKLOG
    for (struct tcp_pcb_listen *l = tcp_listen_pcbs.listen_pcbs; l; l = l->next) {
        if (l->accepts_pending >= t->backlog) {
            t->backlog = l->accepts_pending;
            t->backlog_of = l->backlog;
        }
    }
#endif
    return ERR_OK;
}

void net_health_sample(void)
{
    count_call_t c;
    memset(&c, 0, sizeof(c));
    if (tcpip_api_call(count_on_lwip, &c.call) != ERR_OK) {
        return;
    }
    c.t.max = MEMP_NUM_TCP_PCB;
    s_last = c.t;
    if (c.t.used > s_used_peak) {
        s_used_peak = c.t.used;
    }
    if (c.t.backlog > s_backlog_peak) {
        s_backlog_peak = c.t.backlog;
    }
    // Either one is a new connection's SYN dropped unanswered: say so where /api/events and the app's log
    // show it, once per episode.
    const bool full = c.t.used >= c.t.max || (c.t.backlog_of && c.t.backlog >= c.t.backlog_of);
    if (full && !s_full_logged) {
        char line[PORTA_EVENT_TEXT_MAX + 1];
        snprintf(line, sizeof(line), "SK tcp full %lu/%lu syn=%lu cls=%lu tw=%lu bk=%lu/%lu",
                 (unsigned long)c.t.used, (unsigned long)c.t.max, (unsigned long)c.t.syn,
                 (unsigned long)c.t.closing, (unsigned long)c.t.tw, (unsigned long)c.t.backlog,
                 (unsigned long)c.t.backlog_of);
        host_link_log(line);
    }
    s_full_logged = full;
}

net_tcp_t net_health_last(void)
{
    return s_last;
}

uint32_t net_health_used_peak(void)
{
    return s_used_peak;
}

uint32_t net_health_backlog_peak(void)
{
    return s_backlog_peak;
}

void net_health_str(char *out, size_t cap)
{
    const net_tcp_t t = s_last;
    snprintf(out, cap, "tcp=%lu/%lu syn=%lu est=%lu cls=%lu tw=%lu bk=%lu/%lu peak=%lu/%lu",
             (unsigned long)t.used, (unsigned long)t.max, (unsigned long)t.syn, (unsigned long)t.est,
             (unsigned long)t.closing, (unsigned long)t.tw, (unsigned long)t.backlog,
             (unsigned long)t.backlog_of, (unsigned long)s_used_peak, (unsigned long)s_backlog_peak);
}
