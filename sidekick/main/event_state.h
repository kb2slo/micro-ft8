#pragma once

// event_state.h -- the sidekick's events, and the station's current state built from them
// (docs/architecture/sidekick.md (Port A)).
//
// An event is one Port A EVENT frame's parsed payload, tagged with its subtype and the ring
// sequence the browser polls with. host_link.c keeps a short ring of them for
// GET /api/events?since=; this file keeps, alongside it, what a reloading page needs to draw
// the station as it is now (GET /api/state): the newest decode slots, every live queue entry,
// and the last slot state, TX HUD, USB status and radio state. The ring is too short for that:
// the main board re-sends every live queue entry each second, so a busy minute pushes every
// decode out of it.
//
// Dependency-free (porta_proto only), so host_mock pins it without an ESP-IDF build. Not
// thread-safe: host_link.c holds its lock around every call.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "porta_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

// Most decode slots kept. A client asks for its own count (app.html's DECODE_SLOTS_MAX, sent as
// /api/state?slots=) and gets at most this many.
#define EVENT_STATE_SLOTS_CAP 4u
// Most decodes the main board sends for one slot: decode_types.h's RX_MAX_DECODES, which is C++
// and so cannot be included here. host_test_event_state asserts the two agree.
#define EVENT_STATE_SLOT_DECODES_MAX 32u
#define EVENT_STATE_DECODES_CAP (EVENT_STATE_SLOTS_CAP * EVENT_STATE_SLOT_DECODES_MAX)
// autoseq.h's AUTOSEQ_MAX_QUEUE (active and inactive), asserted by the same test.
#define EVENT_STATE_QUEUE_CAP 30u
// The main board re-sends every live queue entry each second (porta_queue_state_tick()), so one
// not heard from for this long is gone: its IDLE was lost, or the main board restarted.
#define EVENT_STATE_QUEUE_STALE_MS 4000u

typedef struct {
    uint32_t epoch_secs;
    char     text[PORTA_EVENT_TEXT_MAX + 1];
} event_log_t;

typedef struct {
    uint32_t              seq;   // ring sequence, from 1; 0 is "none"
    porta_event_subtype_t kind;
    union {
        event_log_t                 log;
        porta_decode_event_t        decode;
        porta_queue_entry_event_t   queue;
        porta_slot_state_event_t    slot;
        porta_tx_hud_event_t        tx_hud;
        porta_usb_status_event_t    usb;
        porta_sweep_event_t         sweep;
        porta_radio_state_event_t   radio;
        porta_activity_event_t      activity;
    } u;
} event_t;

// One /api/events row for `e` (porta_event_json_*). Returns the bytes written, excluding the
// NUL, or 0 if `out` is too small.
size_t event_json(const event_t *e, char *out, size_t cap);

typedef struct {
    event_t  decodes[EVENT_STATE_DECODES_CAP];  // oldest first; one slot's decodes are adjacent
    size_t   n_decodes;
    event_t  queue[EVENT_STATE_QUEUE_CAP];
    uint32_t queue_seen_ms[EVENT_STATE_QUEUE_CAP];
    size_t   n_queue;
    event_t  slot;    // the last of each, seq 0 until one arrives
    event_t  tx_hud;
    event_t  usb;
    event_t  radio;
} event_state_t;

void event_state_init(event_state_t *s);

// Notes one event, already given its seq. `now_ms` is any monotonic millisecond clock. Logs,
// sweep progress and band activity are not state and are ignored.
void event_state_note(event_state_t *s, const event_t *e, uint32_t now_ms);

// The kept event with the lowest seq in (after, before), so a caller walks the state in the order
// it happened by passing each answer's seq back as `after`. Decodes come only from the newest
// `slots` slots (at most EVENT_STATE_SLOTS_CAP); queue entries only while live. False when none
// is left.
bool event_state_next(const event_state_t *s, uint32_t after, uint32_t before, unsigned slots,
                      uint32_t now_ms, event_t *out);

#ifdef __cplusplus
}
#endif
