// Host test for the sidekick's current-state snapshot behind GET /api/state
// (docs/architecture/sidekick.md (Port A)).
//
// Pins what a reloading page is shown: only the newest slots it asks for, every live queue entry
// and no removed or stale one, the last of each single event, all in the order they happened and
// none at or past the poll cursor the reply hands back. Also pins that a TX HUD's not_keyed
// reaches the browser row, which a field-by-field copy in host_link.c once dropped.

#include "event_state.h"

#include "autoseq.h"
#include "decode_types.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static_assert(EVENT_STATE_SLOT_DECODES_MAX == RX_MAX_DECODES,
              "a slot's decodes: event_state.h must match decode_types.h");
static_assert(EVENT_STATE_QUEUE_CAP == AUTOSEQ_MAX_QUEUE,
              "queue entries: event_state.h must match autoseq.h");
static_assert(PORTA_QUEUE_STATE_IDLE == static_cast<unsigned>(AutoseqState::IDLE),
              "IDLE's wire value: porta_proto.h must match autoseq.h");

static int g_fail = 0;

static void check(bool ok, const char *what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        g_fail++;
    }
}

static uint32_t g_seq = 1;

static event_t decode(uint32_t slot_idx, uint32_t id)
{
    event_t e;
    std::memset(&e, 0, sizeof(e));
    e.seq = g_seq++;
    e.kind = PORTA_EVT_DECODE;
    e.u.decode.slot_idx = slot_idx;
    e.u.decode.slot_len_ds = 150;
    e.u.decode.decode_id = id;
    std::snprintf(e.u.decode.text, sizeof(e.u.decode.text), "CQ K%u", (unsigned)id);
    return e;
}

static event_t queue_entry(uint16_t id, uint8_t state)
{
    event_t e;
    std::memset(&e, 0, sizeof(e));
    e.seq = g_seq++;
    e.kind = PORTA_EVT_QUEUE_ENTRY;
    e.u.queue.entry_id = id;
    e.u.queue.state = state;
    std::snprintf(e.u.queue.dxcall, sizeof(e.u.queue.dxcall), "W%u", (unsigned)id);
    return e;
}

static event_t single(porta_event_subtype_t kind)
{
    event_t e;
    std::memset(&e, 0, sizeof(e));
    e.seq = g_seq++;
    e.kind = kind;
    return e;
}

// Every event the snapshot gives, walked the way get_state() walks it.
static std::vector<event_t> walk(const event_state_t &s, unsigned slots, uint32_t now_ms,
                                 uint32_t before = UINT32_MAX)
{
    std::vector<event_t> out;
    event_t e;
    uint32_t after = 0;
    while (event_state_next(&s, after, before, slots, now_ms, &e)) {
        out.push_back(e);
        after = e.seq;
    }
    return out;
}

static size_t count_kind(const std::vector<event_t> &v, porta_event_subtype_t kind)
{
    size_t n = 0;
    for (const event_t &e : v) {
        n += e.kind == kind;
    }
    return n;
}

static bool in_seq_order(const std::vector<event_t> &v)
{
    for (size_t i = 1; i < v.size(); ++i) {
        if (v[i].seq <= v[i - 1].seq) {
            return false;
        }
    }
    return true;
}

static void test_newest_slots()
{
    static event_state_t s;
    event_state_init(&s);
    // Six slots of three decodes; only the newest EVENT_STATE_SLOTS_CAP are kept.
    for (uint32_t slot = 100; slot < 106; ++slot) {
        for (uint32_t i = 0; i < 3; ++i) {
            const event_t e = decode(slot, slot * 10 + i);
            event_state_note(&s, &e, 0);
        }
    }
    check(s.n_decodes == EVENT_STATE_SLOTS_CAP * 3, "only the newest slots are kept");

    std::vector<event_t> got = walk(s, 3, 0);
    check(got.size() == 9, "slots=3 gives three slots");
    check(!got.empty() && got.front().u.decode.slot_idx == 103, "the oldest slot given is the third newest");
    check(!got.empty() && got.back().u.decode.slot_idx == 105, "the newest slot is last");
    check(in_seq_order(got), "decodes in the order they arrived");

    check(walk(s, 1, 0).size() == 3, "slots=1 gives one slot");
    check(walk(s, 0, 0).empty(), "slots=0 gives no decodes");
    check(walk(s, 99, 0).size() == EVENT_STATE_SLOTS_CAP * 3, "slots past the cap give the cap");
}

static void test_full_slots_fit()
{
    static event_state_t s;
    event_state_init(&s);
    for (uint32_t slot = 1; slot <= EVENT_STATE_SLOTS_CAP + 1; ++slot) {
        for (uint32_t i = 0; i < EVENT_STATE_SLOT_DECODES_MAX; ++i) {
            const event_t e = decode(slot, i);
            event_state_note(&s, &e, 0);
        }
    }
    const std::vector<event_t> got = walk(s, EVENT_STATE_SLOTS_CAP, 0);
    check(got.size() == EVENT_STATE_DECODES_CAP, "the cap's worth of full slots all fit");
    check(!got.empty() && got.front().u.decode.slot_idx == 2, "a full store drops the oldest slot whole");
}

static void test_queue()
{
    static event_state_t s;
    event_state_init(&s);
    event_t e = queue_entry(1, 0);
    event_state_note(&s, &e, 1000);
    e = queue_entry(2, 0);
    event_state_note(&s, &e, 1000);
    e = queue_entry(1, 2);  // entry 1 moves on: one row, the newer one
    event_state_note(&s, &e, 1500);

    std::vector<event_t> got = walk(s, 0, 2000);
    check(got.size() == 2, "one row per live entry");
    check(got.size() == 2 && got[0].u.queue.entry_id == 2 && got[1].u.queue.entry_id == 1 &&
              got[1].u.queue.state == 2,
          "entries in the order last heard, with their latest state");

    e = queue_entry(2, PORTA_QUEUE_STATE_IDLE);
    event_state_note(&s, &e, 2000);
    got = walk(s, 0, 2000);
    check(got.size() == 1 && got[0].u.queue.entry_id == 1, "IDLE removes the entry");

    // Entry 1 was last heard at 1500.
    check(walk(s, 0, 1500 + EVENT_STATE_QUEUE_STALE_MS).size() == 1, "live up to the stale window");
    check(walk(s, 0, 1501 + EVENT_STATE_QUEUE_STALE_MS).empty(), "gone once not re-sent for the window");

    // Full of entries the main board no longer has: the one heard from longest ago goes.
    event_state_init(&s);
    for (uint16_t id = 1; id <= EVENT_STATE_QUEUE_CAP; ++id) {
        e = queue_entry(id, 0);
        event_state_note(&s, &e, 0);
    }
    e = queue_entry(500, 0);
    event_state_note(&s, &e, 0);
    got = walk(s, 0, 0);
    bool has_first = false, has_new = false;
    for (const event_t &q : got) {
        has_first |= q.u.queue.entry_id == 1;
        has_new |= q.u.queue.entry_id == 500;
    }
    check(got.size() == EVENT_STATE_QUEUE_CAP && !has_first && has_new, "a full queue drops the oldest");
}

static void test_singles_and_cursor()
{
    static event_state_t s;
    event_state_init(&s);
    check(walk(s, EVENT_STATE_SLOTS_CAP, 0).empty(), "nothing before anything arrives");

    event_t e = single(PORTA_EVT_TX_HUD);
    e.u.tx_hud.active = true;
    event_state_note(&s, &e, 0);
    e = decode(7, 1);
    event_state_note(&s, &e, 0);
    e = single(PORTA_EVT_TX_HUD);
    e.u.tx_hud.active = false;
    event_state_note(&s, &e, 0);
    const uint32_t hud_seq = e.seq;
    e = single(PORTA_EVT_SLOT_STATE);
    event_state_note(&s, &e, 0);
    e = single(PORTA_EVT_USB_STATUS);
    event_state_note(&s, &e, 0);
    e = single(PORTA_EVT_RADIO_STATE);
    event_state_note(&s, &e, 0);
    e = single(PORTA_EVT_LOG);
    event_state_note(&s, &e, 0);
    e = single(PORTA_EVT_SWEEP);
    event_state_note(&s, &e, 0);
    e = single(PORTA_EVT_ACTIVITY);
    event_state_note(&s, &e, 0);

    std::vector<event_t> got = walk(s, EVENT_STATE_SLOTS_CAP, 0);
    check(count_kind(got, PORTA_EVT_TX_HUD) == 1, "one TX HUD: the last");
    check(count_kind(got, PORTA_EVT_TX_HUD) == 1 && got[1].seq == hud_seq && !got[1].u.tx_hud.active,
          "the last TX HUD, after the decode it followed");
    check(count_kind(got, PORTA_EVT_SLOT_STATE) == 1 && count_kind(got, PORTA_EVT_USB_STATUS) == 1 &&
              count_kind(got, PORTA_EVT_RADIO_STATE) == 1,
          "slot state, USB status and radio state are kept");
    check(count_kind(got, PORTA_EVT_LOG) == 0 && count_kind(got, PORTA_EVT_SWEEP) == 0 &&
              count_kind(got, PORTA_EVT_ACTIVITY) == 0,
          "log lines, sweep progress and band activity are not state");
    check(got.size() == 5 && in_seq_order(got), "five events, in the order they happened");

    // Nothing at or past the cursor the reply hands back: the next poll carries it.
    got = walk(s, EVENT_STATE_SLOTS_CAP, 0, hud_seq);
    check(got.size() == 1 && got[0].kind == PORTA_EVT_DECODE, "stops before `before`");
}

static void test_json_carries_not_keyed()
{
    event_t e = single(PORTA_EVT_TX_HUD);
    e.u.tx_hud.active = true;
    e.u.tx_hud.not_keyed = true;
    std::snprintf(e.u.tx_hud.reason, sizeof(e.u.tx_hud.reason), "SWR");
    char row[512];
    check(event_json(&e, row, sizeof(row)) > 0, "TX HUD row written");
    check(std::strstr(row, "\"nk\":1") != nullptr, "not_keyed reaches the row");

    e = decode(9, 42);
    check(event_json(&e, row, sizeof(row)) > 0 && std::strstr(row, "\"d\":1") != nullptr &&
              std::strstr(row, "CQ K42") != nullptr,
          "a decode renders as a decode row");
}

int main()
{
    test_newest_slots();
    test_full_slots_fit();
    test_queue();
    test_singles_and_cursor();
    test_json_carries_not_keyed();
    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("PASS: event_state\n");
    return 0;
}
