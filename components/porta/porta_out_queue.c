#include "porta_out_queue.h"

#include <string.h>

void porta_out_queue_init(PortaOutQueue *queue)
{
    queue->head = 0;
    queue->count = 0;
    queue->dropped = 0;
}

bool porta_out_queue_push(PortaOutQueue *queue, const uint8_t *bytes, size_t len)
{
    if (len == 0 || len > PORTA_PROTO_MAX_FRAME) {
        return false;
    }
    if (queue->count == PORTA_OUT_QUEUE_LEN) {
        queue->head = (queue->head + 1) % PORTA_OUT_QUEUE_LEN;
        queue->count--;
        queue->dropped++;
    }
    PortaOutFrame *frame = &queue->frames[(queue->head + queue->count) % PORTA_OUT_QUEUE_LEN];
    memcpy(frame->bytes, bytes, len);
    frame->len = (uint16_t)len;
    queue->count++;
    return true;
}

const PortaOutFrame *porta_out_queue_front(const PortaOutQueue *queue)
{
    if (queue->count == 0) {
        return NULL;
    }
    return &queue->frames[queue->head];
}

void porta_out_queue_pop(PortaOutQueue *queue)
{
    if (queue->count == 0) {
        return;
    }
    queue->head = (queue->head + 1) % PORTA_OUT_QUEUE_LEN;
    queue->count--;
}

size_t porta_out_queue_count(const PortaOutQueue *queue)
{
    return queue->count;
}

uint32_t porta_out_queue_dropped(const PortaOutQueue *queue)
{
    return queue->dropped;
}
