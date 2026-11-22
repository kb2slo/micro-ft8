/*
 * The busy hold (components/autoseq/include/busy_hold.h; docs/architecture/autoseq.md, The busy hold).
 *
 * An opening call to a station heard working someone else waits: it transmits nothing and spends no tries,
 * goes out once the station is free (its RR73, RRR or 73 to the other call, a CQ, a message to us), and ends
 * after AUTOSEQ_HOLD_MAX_MS held. A held entry sits behind the others of its stage, and ahead of the CQ, which
 * waits with it; a tap on a busy station does not promote it. Two opening calls on opposite periods do not take turns, keying
 * every slot and missing each answer: the period in use keeps going.
 */
#include <cstdio>
#include <string>
#include <vector>

#include "autoseq.h"
#include "busy_hold.h"
#include "host_mocks.h"
#include "queue_event.h"

static int s_failed = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        s_failed = 1;
    }
}

static const char* MY = "KB2SLO";
static int64_t s_now = 0;
static void at(int64_t ms) { s_now = ms; mock_clock_set(ms); }

static bool adif_cb(const std::string&, const std::string&, int, int, RxChannel) { return true; }

static void reset() {
    at(1000);
    autoseq_init();
    autoseq_set_station(MY, "FN30");
    autoseq_set_tries(6);
    autoseq_set_adif_callback(adif_cb);
}

static UiRxLine line(const char* to, const char* from, const char* rest, int slot) {
    UiRxLine m;
    m.field1 = to;
    m.field2 = from;
    m.field3 = rest;
    m.text = std::string(to) + " " + from + " " + rest;
    m.snr = -8;
    m.offset_hz = 1200;
    m.slot_id = slot;
    m.is_cq = std::string(to) == "CQ";
    m.is_to_me = std::string(to) == MY;
    return m;
}

// One slot's decodes, as decode_pipeline_apply() hands them over: every decode to the hold, then those to us.
static void heard(const std::vector<UiRxLine>& slot) {
    std::vector<UiRxLine> to_me;
    for (const UiRxLine& m : slot) {
        autoseq_note_heard(m);
        if (m.is_to_me) to_me.push_back(m);
    }
    autoseq_refresh_holds();
    if (!to_me.empty()) autoseq_on_decodes(to_me);
}

// What the queue sends now, as a board would: fetch, key, tick. Empty for nothing.
static std::string transmit(int slot_parity = -1) {
    AutoseqTxEntry pending;
    if (!autoseq_fetch_pending_tx(pending)) return "";
    if (slot_parity >= 0 && (pending.slot_id & 1) != slot_parity) return "";
    autoseq_on_tx_starting();
    autoseq_mark_sent(0);
    autoseq_tick(0, 0, 0);
    return pending.text;
}

static bool find(const char* dx, QsoContext* out) {
    for (int i = 0; i < autoseq_active_count(); ++i) {
        if (autoseq_get_active_context(i, out) && out->dxcall == dx) return true;
    }
    return false;
}

static void test_kind() {
    check(busy_hold_kind("W9XYZ", "K1ABC", "-10", false, MY) == HeardKind::Busy, "a report to another call is busy");
    check(busy_hold_kind("W9XYZ", "K1ABC", "R+05", false, MY) == HeardKind::Busy, "an R-report is busy");
    check(busy_hold_kind("W9XYZ", "K1ABC", "EM48", false, MY) == HeardKind::Busy, "a grid to another call is busy");
    check(busy_hold_kind("W9XYZ", "K1ABC", "R 2A EMA", false, MY) == HeardKind::Busy, "a Field Day exchange is busy");
    check(busy_hold_kind("W9XYZ", "K1ABC", "RR73", false, MY) == HeardKind::Free, "RR73 to another call frees it");
    check(busy_hold_kind("W9XYZ", "K1ABC", "RRR", false, MY) == HeardKind::Free, "RRR frees it");
    check(busy_hold_kind("W9XYZ", "K1ABC", "73", false, MY) == HeardKind::Free, "73 frees it");
    check(busy_hold_kind("DX", "K1ABC", "FN42", true, MY) == HeardKind::Free, "a CQ with a modifier frees it");
    check(busy_hold_kind(MY, "K1ABC", "-10", false, MY) == HeardKind::Free, "a message to us frees it");
    check(busy_hold_kind("TNX", "BOB", "73", false, MY) == HeardKind::None, "free text says nothing");
    check(busy_hold_kind("W9XYZ", "K1ABC", "", false, MY) == HeardKind::None, "two calls alone say nothing");
}

static void test_hold_and_release() {
    reset();
    heard({line("CQ", "K1ABC", "FN42", 1)});
    autoseq_on_touch(line("CQ", "K1ABC", "FN42", 1));
    check(transmit() == "K1ABC KB2SLO FN30", "the opening call goes out");
    QsoContext c;
    find("K1ABC", &c);
    const int tries = c.tries_sent;

    at(16000);
    heard({line("W9XYZ", "K1ABC", "-10", 1)});
    check(find("K1ABC", &c) && c.held, "working W9XYZ: held");
    check((queue_entry_event(c).flags & PORTA_QUEUE_HELD) != 0, "its QUEUE_ENTRY says held");
    for (int k = 0; k < 3; ++k) {
        at(31000 + k * 15000);
        check(transmit().empty(), "held: nothing goes out");
        heard({line("W9XYZ", "K1ABC", "R-12", 1)});
    }
    check(find("K1ABC", &c) && c.held && c.tries_sent == tries, "a hold spends no tries");

    at(91000);
    heard({line("W9XYZ", "K1ABC", "RR73", 1)});
    check(find("K1ABC", &c) && !c.held, "its RR73 releases it");
    check((queue_entry_event(c).flags & PORTA_QUEUE_HELD) == 0, "and its QUEUE_ENTRY says so");
    check(transmit() == "K1ABC KB2SLO FN30", "it goes out in the next slot");
    find("K1ABC", &c);
    check(c.tries_sent == tries + 1, "its tries resume where they were");
}

static void test_busy_goes_stale() {
    reset();
    autoseq_on_touch(line("W9XYZ", "K1ABC", "-10", 1));
    heard({line("W9XYZ", "K1ABC", "-10", 1)});
    QsoContext c;
    check(find("K1ABC", &c) && c.held, "a station last heard working someone is held");
    at(1000 + AUTOSEQ_BUSY_SLOTS * 15000);
    heard({});
    check(find("K1ABC", &c) && !c.held, "not heard for four slots: no longer busy");
}

static void test_hold_ends() {
    reset();
    autoseq_on_touch(line("CQ", "K1ABC", "FN42", 1));
    check(transmit() == "K1ABC KB2SLO FN30", "the opening call goes out");
    QsoContext c;
    int64_t t = 16000;
    at(t);
    heard({line("W9XYZ", "K1ABC", "-10", 1)});
    check(find("K1ABC", &c) && c.held, "held");
    while (t < 16000 + AUTOSEQ_HOLD_MAX_MS - 15000) {
        t += 15000;
        at(t);
        heard({line("W9XYZ", "K1ABC", "R-12", 1)});
    }
    check(find("K1ABC", &c) && c.held, "still held before five minutes");
    at(t + 15000);
    heard({line("W9XYZ", "K1ABC", "R-12", 1)});
    check(!find("K1ABC", &c), "five minutes held: the entry ends, as if its last try were used");
}

static void test_cq_while_held() {
    reset();
    autoseq_call_cq(RxChannel{}, 0);
    heard({line("W9XYZ", "K1ABC", "-10", 1)});
    autoseq_on_touch(line("W9XYZ", "K1ABC", "-10", 1));
    check(transmit().empty(), "the CQ waits behind a held opening call: nothing goes out");
    at(16000);
    heard({line("W9XYZ", "K1ABC", "RR73", 1)});
    check(transmit() == "K1ABC KB2SLO FN30", "released, the opening call goes before the CQ");
}

static void test_tap_on_busy_not_promoted() {
    reset();
    autoseq_on_touch(line("CQ", "N6HAN", "CM87", 1));
    heard({line("W9XYZ", "K1ABC", "-10", 1)});
    autoseq_on_touch(line("W9XYZ", "K1ABC", "-10", 1));
    QsoContext head;
    check(autoseq_get_active_context(0, &head) && head.dxcall == "N6HAN", "a tap on a busy station waits behind one that can answer");
    check(transmit() == "N6HAN KB2SLO FN30", "and that one goes out");
}

static void test_mixed_parity() {
    reset();
    autoseq_set_tries(4);
    autoseq_on_touch(line("CQ", "K1ABC", "FN42", 1));  // we answer on even
    autoseq_on_touch(line("CQ", "N6HAN", "CM87", 0));  // we answer on odd
    std::vector<int> parities;
    for (int k = 0; k < 8; ++k) {
        AutoseqTxEntry p;
        if (!autoseq_fetch_pending_tx(p)) break;
        parities.push_back(p.slot_id & 1);
        transmit();
    }
    check(parities.size() == 8, "both get all their tries");
    bool alternates = false;
    int changes = 0;
    for (size_t i = 1; i < parities.size(); ++i) {
        if (parities[i] != parities[i - 1]) ++changes;
    }
    alternates = changes > 1;
    check(!alternates, "two opening calls on opposite periods do not take turns: one period, then the other");
}

int main() {
    test_kind();
    test_hold_and_release();
    test_busy_goes_stale();
    test_hold_ends();
    test_cq_while_held();
    test_tap_on_busy_not_promoted();
    test_mixed_parity();
    if (s_failed) return 1;
    printf("PASS: the busy hold\n");
    return 0;
}
