#include "event_state.h"

#include <string.h>

size_t event_json(const event_t *e, char *out, size_t cap)
{
    switch (e->kind) {
    case PORTA_EVT_LOG:
        return porta_event_json_log(out, cap, e->seq, e->u.log.epoch_secs, e->u.log.text);
    case PORTA_EVT_DECODE:
        return porta_event_json_decode(out, cap, e->seq, &e->u.decode);
    case PORTA_EVT_QUEUE_ENTRY:
        return porta_event_json_queue(out, cap, e->seq, &e->u.queue);
    case PORTA_EVT_SLOT_STATE:
        return porta_event_json_slot(out, cap, e->seq, &e->u.slot);
    case PORTA_EVT_TX_HUD:
        return porta_event_json_tx_hud(out, cap, e->seq, &e->u.tx_hud);
    case PORTA_EVT_USB_STATUS:
        return porta_event_json_usb(out, cap, e->seq, &e->u.usb);
    case PORTA_EVT_SWEEP:
        return porta_event_json_sweep(out, cap, e->seq, &e->u.sweep);
    case PORTA_EVT_RADIO_STATE:
        return porta_event_json_radio(out, cap, e->seq, &e->u.radio);
    case PORTA_EVT_ACTIVITY:
        return porta_event_json_activity(out, cap, e->seq, &e->u.activity);
    }
    return 0;
}

void event_state_init(event_state_t *s)
{
    memset(s, 0, sizeof(*s));
}

// The slot a decode was heard in. 0 (the main board's clock not set) groups every such decode
// as one slot, which is the best that can be said of them.
static uint32_t slot_of(const event_t *e)
{
    return e->u.decode.slot_idx;
}

// Index of the first decode in the newest `slots` slots: one slot's decodes are adjacent, so a
// slot starts wherever the slot changes from the decode before it.
static size_t newest_slots_start(const event_state_t *s, unsigned slots)
{
    if (slots == 0) {
        return s->n_decodes;
    }
    size_t i = s->n_decodes;
    unsigned seen = 0;
    while (i > 0) {
        if (i == s->n_decodes || slot_of(&s->decodes[i - 1]) != slot_of(&s->decodes[i])) {
            if (++seen > slots) {
                return i;
            }
        }
        --i;
    }
    return 0;
}

static void drop_oldest_decodes(event_state_t *s, size_t n)
{
    memmove(&s->decodes[0], &s->decodes[n], (s->n_decodes - n) * sizeof(s->decodes[0]));
    s->n_decodes -= n;
}

static void note_decode(event_state_t *s, const event_t *e)
{
    // A new slot: keep room for it by dropping whatever falls outside the newest
    // EVENT_STATE_SLOTS_CAP - 1 already kept.
    if (s->n_decodes > 0 && slot_of(&s->decodes[s->n_decodes - 1]) != slot_of(e)) {
        drop_oldest_decodes(s, newest_slots_start(s, EVENT_STATE_SLOTS_CAP - 1u));
    }
    if (s->n_decodes == EVENT_STATE_DECODES_CAP) {
        drop_oldest_decodes(s, 1);
    }
    s->decodes[s->n_decodes++] = *e;
}

static void note_queue(event_state_t *s, const event_t *e, uint32_t now_ms)
{
    size_t i = 0;
    while (i < s->n_queue && s->queue[i].u.queue.entry_id != e->u.queue.entry_id) {
        ++i;
    }
    if (e->u.queue.state == PORTA_QUEUE_STATE_IDLE) {
        if (i < s->n_queue) {
            --s->n_queue;
            s->queue[i] = s->queue[s->n_queue];
            s->queue_seen_ms[i] = s->queue_seen_ms[s->n_queue];
        }
        return;
    }
    if (i == s->n_queue) {
        if (s->n_queue < EVENT_STATE_QUEUE_CAP) {
            ++s->n_queue;
        } else {
            // Full of entries the main board no longer has (it holds at most this many): the one
            // heard from longest ago goes.
            i = 0;
            for (size_t j = 1; j < s->n_queue; ++j) {
                if (s->queue[j].seq < s->queue[i].seq) {
                    i = j;
                }
            }
        }
    }
    s->queue[i] = *e;
    s->queue_seen_ms[i] = now_ms;
}

void event_state_note(event_state_t *s, const event_t *e, uint32_t now_ms)
{
    switch (e->kind) {
    case PORTA_EVT_DECODE:
        note_decode(s, e);
        break;
    case PORTA_EVT_QUEUE_ENTRY:
        note_queue(s, e, now_ms);
        break;
    case PORTA_EVT_SLOT_STATE:
        s->slot = *e;
        break;
    case PORTA_EVT_TX_HUD:
        s->tx_hud = *e;
        break;
    case PORTA_EVT_USB_STATUS:
        s->usb = *e;
        break;
    case PORTA_EVT_RADIO_STATE:
        s->radio = *e;
        break;
    case PORTA_EVT_LOG:
    case PORTA_EVT_SWEEP:
    case PORTA_EVT_ACTIVITY:
        break;
    }
}

// Keeps `e` in `best` if it is in (after, before) and earlier than what `best` has.
static void consider(const event_t *e, uint32_t after, uint32_t before, const event_t **best)
{
    if (e->seq > after && e->seq < before && (!*best || e->seq < (*best)->seq)) {
        *best = e;
    }
}

bool event_state_next(const event_state_t *s, uint32_t after, uint32_t before, unsigned slots,
                      uint32_t now_ms, event_t *out)
{
    const event_t *best = NULL;
    if (slots > EVENT_STATE_SLOTS_CAP) {
        slots = EVENT_STATE_SLOTS_CAP;
    }
    for (size_t i = newest_slots_start(s, slots); i < s->n_decodes; ++i) {
        consider(&s->decodes[i], after, before, &best);
    }
    for (size_t i = 0; i < s->n_queue; ++i) {
        if ((uint32_t)(now_ms - s->queue_seen_ms[i]) <= EVENT_STATE_QUEUE_STALE_MS) {
            consider(&s->queue[i], after, before, &best);
        }
    }
    consider(&s->slot, after, before, &best);
    consider(&s->tx_hud, after, before, &best);
    consider(&s->usb, after, before, &best);
    consider(&s->radio, after, before, &best);
    if (!best) {
        return false;
    }
    *out = *best;
    return true;
}
