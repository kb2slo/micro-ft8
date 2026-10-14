#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "porta_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

// Frames waiting for the Port A UART. Only the main loop sends them, every
// 10 ms, so the queue must hold whatever arrives in between. The largest burst
// is a slot's decode: RX_MAX_DECODES decodes and the SLOT line, queued at once
// by the decode task. The 15 left over cover the main loop's once-a-second
// status landing in the same 10 ms: SLOT_STATE, TX_HUD and a QUEUE_ENTRY for
// each of up to 13 active QSOs. host_mock's test_porta_out_queue.cpp holds
// this to RX_MAX_DECODES.
#define PORTA_OUT_QUEUE_LEN 48

typedef struct PortaOutFrame {
    uint8_t bytes[PORTA_PROTO_MAX_FRAME];
    uint16_t len;
} PortaOutFrame;

// Not thread-safe: the caller holds one lock around every call. A push to a
// full queue drops the oldest frame and counts it: on a link that has fallen
// behind, the recent frames are the ones worth having.
typedef struct PortaOutQueue {
    PortaOutFrame frames[PORTA_OUT_QUEUE_LEN];
    size_t head;
    size_t count;
    uint32_t dropped;
} PortaOutQueue;

void porta_out_queue_init(PortaOutQueue *queue);

// False, and nothing queued, for an empty frame or one over PORTA_PROTO_MAX_FRAME.
bool porta_out_queue_push(PortaOutQueue *queue, const uint8_t *bytes, size_t len);

// The next frame to send, or NULL when empty. Valid until the next push or pop.
const PortaOutFrame *porta_out_queue_front(const PortaOutQueue *queue);

// Removes the front frame; does nothing when empty.
void porta_out_queue_pop(PortaOutQueue *queue);

size_t porta_out_queue_count(const PortaOutQueue *queue);
uint32_t porta_out_queue_dropped(const PortaOutQueue *queue);

#ifdef __cplusplus
}
#endif
