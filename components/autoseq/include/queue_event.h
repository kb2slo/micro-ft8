#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "autoseq.h"
#include "porta_proto.h"

// What a queue entry's QUEUE_ENTRY event says of it (porta_queue_entry_event_t, porta_proto.h;
// docs/architecture/sidekick.md, Port A). Header only, so host_mock builds the same event the main
// board sends; the main board adds the epoch and sends it.

// The dial a mode transmits from on one QMX: FT8's dial on the band, and FT4's beside it is FT8's
// plus the band's FT4 offset, as WSJT-X logs it. An unknown dial (0) stays unknown.
inline uint32_t queue_mode_dial_hz(RxMode mode, uint32_t ft8_dial_hz, int32_t ft4_dial_offset_hz) {
    if (mode != RxMode::FT4 || ft8_dial_hz == 0) {
        return ft8_dial_hz;
    }
    return (uint32_t)((int64_t)ft8_dial_hz + ft4_dial_offset_hz);
}

inline porta_queue_entry_event_t queue_entry_event(const QsoContext& ctx, uint32_t ft8_dial_hz,
                                                   int32_t ft4_dial_offset_hz) {
    porta_queue_entry_event_t q;
    memset(&q, 0, sizeof(q));
    q.entry_id = ctx.entry_id;
    q.state = (uint8_t)ctx.state;
    q.tries_sent = (uint8_t)ctx.tries_sent;
    q.try_limit = (uint8_t)ctx.try_limit;
    q.flags = (uint8_t)(((ctx.slot_id & 1) ? PORTA_QUEUE_ODD : 0) |
                        (ctx.tries_custom ? PORTA_QUEUE_CUSTOM : 0));
    q.mode = ctx.mode == RxMode::FT4 ? PORTA_QUEUE_MODE_FT4 : PORTA_QUEUE_MODE_FT8;
    q.dial_hz = queue_mode_dial_hz(ctx.mode, ft8_dial_hz, ft4_dial_offset_hz);
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
