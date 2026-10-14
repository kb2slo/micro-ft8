// The Port A outbound queue (components/porta/porta_out_queue.c).
// A busy slot's decodes are queued in one burst between two drains; with 24
// slots the oldest were dropped, and decodes are sent with those addressed to
// us first, so ours were the first to go.
#include <cstdio>
#include <cstdint>
#include <cstring>

#include "decode_types.h"
#include "porta_out_queue.h"
#include "porta_proto.h"

static int g_fails = 0;

static void expect_u(unsigned long got, unsigned long want, const char *msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %lu want %lu)\n", msg, got, want);
        ++g_fails;
    }
}

static void expect_true(bool ok, const char *msg)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    }
}

// Re-frames a queued frame's bytes the way the sidekick does.
static bool reframe(const PortaOutFrame *queued, porta_frame_t *frame)
{
    porta_decoder_t decoder;
    porta_decoder_init(&decoder);
    bool complete = false;
    for (uint16_t i = 0; i < queued->len; ++i) {
        complete = porta_decoder_push(&decoder, queued->bytes[i], frame);
    }
    return complete;
}

// The main loop's once-a-second status that can land in the same 10 ms as a
// slot's decode: SLOT_STATE, TX_HUD and a QUEUE_ENTRY for each of 13 QSOs.
static const unsigned kStatusFrames = 15;

static void test_a_busy_slot_fits()
{
    expect_true(PORTA_OUT_QUEUE_LEN >= RX_MAX_DECODES + 1 + kStatusFrames,
                "the queue holds a full slot's decodes, its SLOT line and the status beside them");

    static PortaOutQueue queue;
    porta_out_queue_init(&queue);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    for (unsigned i = 0; i < kStatusFrames; ++i) {
        const size_t n = porta_proto_encode_log(1760000000u, "status", buf, sizeof(buf));
        porta_out_queue_push(&queue, buf, n);
    }
    // The decode task's burst, in the order decode_sort.h sends it: to us first.
    for (unsigned i = 0; i < RX_MAX_DECODES; ++i) {
        porta_decode_event_t ev = {};
        ev.epoch_secs = 1760000000u;
        ev.decode_id = 1000 + i;
        snprintf(ev.text, sizeof(ev.text), "%s K%uABC FN42", i < 2 ? "KB2SLO" : "CQ", i);
        ev.is_to_me = i < 2;
        ev.is_cq = i >= 2;
        ev.slot_idx = 117333333u;
        ev.slot_len_ds = 150;
        ev.dial_hz = 14074000u;
        const size_t n = porta_proto_encode_decode(&ev, buf, sizeof(buf));
        expect_true(n > 0 && n <= PORTA_PROTO_MAX_FRAME, "a decode encodes into one frame");
        porta_out_queue_push(&queue, buf, n);
    }
    const size_t n = porta_proto_encode_log(1760000000u, "SLOT 117333333 c256/256 m32 t2 dec315 left900 b2/100",
                                            buf, sizeof(buf));
    porta_out_queue_push(&queue, buf, n);

    expect_u(porta_out_queue_dropped(&queue), 0, "nothing is dropped");
    expect_u(porta_out_queue_count(&queue), kStatusFrames + RX_MAX_DECODES + 1, "every frame is queued");

    for (unsigned i = 0; i < kStatusFrames; ++i) {
        porta_out_queue_pop(&queue);
    }
    for (unsigned i = 0; i < RX_MAX_DECODES; ++i) {
        porta_frame_t frame;
        porta_decode_event_t ev = {};
        const PortaOutFrame *queued = porta_out_queue_front(&queue);
        expect_true(queued != nullptr, "a decode is still queued");
        if (!queued) return;
        expect_true(reframe(queued, &frame) && porta_proto_parse_decode(&frame, &ev),
                    "a queued decode re-frames and parses");
        expect_u(ev.decode_id, 1000 + i, "decodes leave in the order they were sent");
        if (i < 2) {
            expect_true(ev.is_to_me, "the decodes to us are still there");
        }
        porta_out_queue_pop(&queue);
    }
    porta_frame_t frame;
    char text[PORTA_EVENT_TEXT_MAX + 1];
    uint32_t epoch = 0;
    expect_true(reframe(porta_out_queue_front(&queue), &frame) &&
                    porta_proto_parse_log(&frame, &epoch, text) && strncmp(text, "SLOT ", 5) == 0,
                "the SLOT line follows the decodes");
    porta_out_queue_pop(&queue);
    expect_true(porta_out_queue_front(&queue) == nullptr, "the queue is empty after the slot");
}

static void test_a_full_queue_drops_the_oldest()
{
    static PortaOutQueue queue;
    porta_out_queue_init(&queue);
    for (unsigned i = 0; i < PORTA_OUT_QUEUE_LEN + 3; ++i) {
        const uint8_t byte = (uint8_t)i;
        expect_true(porta_out_queue_push(&queue, &byte, 1), "a one-byte frame is queued");
    }
    expect_u(porta_out_queue_dropped(&queue), 3, "three over capacity are counted");
    expect_u(porta_out_queue_count(&queue), PORTA_OUT_QUEUE_LEN, "the queue stays full");
    for (unsigned i = 3; i < PORTA_OUT_QUEUE_LEN + 3; ++i) {
        const PortaOutFrame *queued = porta_out_queue_front(&queue);
        expect_true(queued != nullptr && queued->len == 1, "a frame is left");
        if (!queued) return;
        expect_u(queued->bytes[0], (uint8_t)i, "the oldest went, the rest keep their order");
        porta_out_queue_pop(&queue);
    }
}

static void test_edges()
{
    static PortaOutQueue queue;
    porta_out_queue_init(&queue);
    uint8_t big[PORTA_PROTO_MAX_FRAME + 1] = {};
    expect_true(!porta_out_queue_push(&queue, big, 0), "an empty frame is refused");
    expect_true(!porta_out_queue_push(&queue, big, sizeof(big)), "an oversized frame is refused");
    expect_true(porta_out_queue_push(&queue, big, PORTA_PROTO_MAX_FRAME), "a maximum frame is queued");
    expect_u(porta_out_queue_dropped(&queue), 0, "a refusal is not a drop");
    porta_out_queue_pop(&queue);
    expect_true(porta_out_queue_front(&queue) == nullptr, "front of an empty queue is null");
    porta_out_queue_pop(&queue);
    expect_u(porta_out_queue_count(&queue), 0, "pop on an empty queue does nothing");

    // Across the end of the array, order holds.
    for (unsigned round = 0; round < 3 * PORTA_OUT_QUEUE_LEN; ++round) {
        const uint8_t a = (uint8_t)round;
        const uint8_t b = (uint8_t)(round + 100);
        porta_out_queue_push(&queue, &a, 1);
        porta_out_queue_push(&queue, &b, 1);
        expect_u(porta_out_queue_front(&queue)->bytes[0], a, "first in, first out");
        porta_out_queue_pop(&queue);
        expect_u(porta_out_queue_front(&queue)->bytes[0], b, "then the second");
        porta_out_queue_pop(&queue);
    }
    expect_u(porta_out_queue_dropped(&queue), 0, "nothing dropped while draining");
}

int main()
{
    test_a_busy_slot_fits();
    test_a_full_queue_drops_the_oldest();
    test_edges();
    if (g_fails) {
        fprintf(stderr, "%d failed\n", g_fails);
        return 1;
    }
    printf("ok\n");
    return 0;
}
