#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "autoseq.h"
#include "porta_proto.h"
#include "station_channel.h"

// What a queue entry's QUEUE_ENTRY event says of it (porta_queue_entry_event_t, porta_proto.h;
// docs/architecture/sidekick.md, Port A). Header only, so host_mock builds the same event the main
// board sends; the main board adds the epoch and sends it.

// The entry's own channel names it: its dial and its mode.
inline porta_queue_entry_event_t queue_entry_event(const QsoContext& ctx) {
    porta_queue_entry_event_t q;
    memset(&q, 0, sizeof(q));
    q.entry_id = ctx.entry_id;
    q.state = (uint8_t)ctx.state;
    q.tries_sent = (uint8_t)ctx.tries_sent;
    q.try_limit = (uint8_t)ctx.try_limit;
    q.flags = (uint8_t)(((ctx.slot_id & 1) ? PORTA_QUEUE_ODD : 0) |
                        (ctx.tries_custom ? PORTA_QUEUE_CUSTOM : 0) |
                        (ctx.held ? PORTA_QUEUE_HELD : 0));
    q.mode = ctx.channel.mode == RxMode::FT4 ? PORTA_QUEUE_MODE_FT4 : PORTA_QUEUE_MODE_FT8;
    q.dial_hz = ctx.channel.dial_hz;
    q.offset_hz = (uint16_t)(ctx.tx_offset_hz > 0 ? ctx.tx_offset_hz : 0);
    snprintf(q.dxcall, sizeof(q.dxcall), "%s", ctx.dxcall.c_str());
    return q;
}

// An entry that has left the queue: IDLE, and nothing else.
inline porta_queue_entry_event_t queue_entry_gone_event(uint16_t entry_id) {
    porta_queue_entry_event_t q;
    memset(&q, 0, sizeof(q));
    q.entry_id = entry_id;
    q.state = PORTA_QUEUE_STATE_IDLE;
    return q;
}
