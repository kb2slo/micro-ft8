// L5: the CAT command sequence of a QMX transmission, from TX; to RX;.
//
// Holds the host model (tx_state_machine.cpp) to the contract in tx_timing.h
// that poe_p4/main/tx_scheduler.cpp shares with it: TX; at the slot boundary
// with no RF, first TA at +500 ms, TA0; when the last symbol has run, RX; 50 ms
// later, and RX; at once on a cancel. The firmware itself is not run here.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "tx_state_machine.h"

static int g_failed = 0;

static void check(bool ok, const char* what, const std::string& label) {
    if (!ok) {
        printf("  FAIL %s: %s\n", label.c_str(), what);
        g_failed++;
    }
}

static const CatEvent* first_of(const std::vector<CatEvent>& ev, CatEvent::Kind k) {
    for (const auto& e : ev) if (e.kind == k) return &e;
    return nullptr;
}

static int count_of(const std::vector<CatEvent>& ev, CatEvent::Kind k) {
    int n = 0;
    for (const auto& e : ev) if (e.kind == k) n++;
    return n;
}

struct Proto {
    const char* name;
    ftx_protocol_t id;
    int sym_ms;
    int nn;
    int64_t slot_ms;
};

static const Proto kProtos[] = {
    {"FT8", FTX_PROTOCOL_FT8, 160, 79, 15000},
    {"FT4", FTX_PROTOCOL_FT4, 48, 105, 7500},
};

static const int64_t kDelay = 10;  // the main loop's tick

static void on_time(const Proto& p) {
    const std::string label = std::string(p.name) + " on time";
    const int64_t s = 3 * p.slot_ms;
    TxConfig cfg = {p.name, p.id, 1500.0f, s, 0, "W1ABC K9XYZ -12"};
    auto ev = run_tx(cfg, kDelay);

    check(ev.front().kind == CatEvent::TX_BEGIN && ev.front().time_ms == s,
          "TX; at the slot boundary", label);
    const CatEvent* ta = first_of(ev, CatEvent::TA);
    check(ta && ta->time_ms >= s + kTxToneLeadMs && ta->time_ms <= s + kTxToneLeadMs + kDelay,
          "first TA at +500 ms", label);
    check(count_of(ev, CatEvent::TX_BEGIN) == 1, "one TX;", label);
    check(count_of(ev, CatEvent::TA) > 0, "tones sent", label);
    check(verify_event_timing(ev, cfg, p.sym_ms, kDelay).empty(), "every TA slot-anchored", label);

    const int64_t last_symbol_ends = s + kTxToneLeadMs + (int64_t)p.nn * p.sym_ms;
    const CatEvent* up = first_of(ev, CatEvent::KEY_UP);
    check(up && up->time_ms >= last_symbol_ends && up->time_ms <= last_symbol_ends + kDelay,
          "TA0; when the last symbol has run", label);
    const CatEvent* rx = first_of(ev, CatEvent::TX_END);
    check(up && rx && rx->time_ms - up->time_ms >= kTxKeyUpTailMs &&
              rx->time_ms - up->time_ms <= kTxKeyUpTailMs + kDelay,
          "RX; 50 ms after TA0;", label);
    check(ev.back().kind == CatEvent::TX_END && rx == &ev.back(), "RX; is last", label);
    // The TX ends in time for the next slot's decode and arm logic.
    check(rx && rx->time_ms < s + p.slot_ms,
          "RX; before the next slot", label);
    if (p.id == FTX_PROTOCOL_FT8) {
        check(rx && rx->time_ms - s <= 13200 + kDelay, "FT8 ends by about 13.2 s", label);
    }
}

// However late the TX is armed, the first TA is at least min_delay_ms after TX;, and
// the tone it sounds is the one that is due then.
static void armed_late(const Proto& p) {
    for (int delay : {0, 50, 200, 500}) {
        for (int64_t arm : {0, 10, 300, 450, 499, 500, 1000, 3000}) {
            const std::string label = std::string(p.name) + " armed at " + std::to_string(arm) +
                                      " ms, min delay " + std::to_string(delay);
            const int64_t s = p.slot_ms;
            const int skip = tx_skip_tones((int)arm, p.sym_ms, kTxToneLeadMs, delay);
            TxConfig cfg = {p.name, p.id, 1500.0f, s, skip, "W1ABC K9XYZ -12"};
            cfg.arm_ms = arm;
            cfg.min_delay_ms = delay;
            auto ev = run_tx(cfg, kDelay);
            const CatEvent* tx = first_of(ev, CatEvent::TX_BEGIN);
            const CatEvent* ta = first_of(ev, CatEvent::TA);
            check(tx && tx->time_ms == s + arm, "TX; when armed", label);
            if (!tx || !ta) { check(false, "events", label); continue; }
            const int64_t due = s + kTxToneLeadMs + (int64_t)skip * p.sym_ms;
            check(ta->time_ms - tx->time_ms >= delay, "first TA at least the min delay after TX;",
                  label);
            check(ta->time_ms >= due, "first TA not before its tone is due", label);
            check(ta->time_ms <= std::max(due, tx->time_ms + delay) + kDelay,
                  "first TA no later than it must be", label);
            check(ta->time_ms - tx->time_ms >= 0 && ta->time_ms < due + p.sym_ms + kDelay,
                  "first TA sounds the tone due then", label);
            // Every later TA is anchored to the slot again.
            std::vector<CatEvent> rest;
            bool seen = false;
            for (const auto& e : ev) {
                if (e.kind == CatEvent::TA && !seen) { seen = true; continue; }
                rest.push_back(e);
            }
            check(verify_event_timing(rest, cfg, p.sym_ms, kDelay).empty(),
                  "later TAs slot-anchored", label);
            // A repeated tone sends no second TA, so at most one per remaining symbol.
            const int sent = count_of(rest, CatEvent::TA);
            check(sent > (p.nn - skip) / 2 && sent <= p.nn - skip - 1,
                  "tones after the skip are sent", label);
            const int64_t end = s + kTxToneLeadMs + (int64_t)p.nn * p.sym_ms;
            const CatEvent* up = first_of(ev, CatEvent::KEY_UP);
            check(up && up->time_ms >= end && up->time_ms <= end + kDelay,
                  "TA0; at the same end", label);
        }
    }
}

static void skip_values() {
    const std::string label = "tx_skip_tones";
    check(tx_skip_tones(0, 160, 500, 50) == 0, "on the boundary", label);
    check(tx_skip_tones(450, 160, 500, 50) == 0, "RF at 500 ms is tone 0", label);
    check(tx_skip_tones(609, 160, 500, 50) == 0, "RF at 659 ms is still tone 0", label);
    check(tx_skip_tones(610, 160, 500, 50) == 1, "RF at 660 ms is tone 1", label);
    check(tx_skip_tones(3199, 160, 500, 50) == 17, "3199 ms", label);
    check(tx_skip_tones(300, 160, 500, 500) == 1, "a 500 ms delay at 300 ms", label);
    check(tx_skip_tones(1000, 160, 0, 0) == 6, "no lead or delay is slot_ms / symbol", label);
    check(tx_first_rf_ms(1234, 50) == 1284, "first RF is the delay after TX;", label);
}

static void cancelled(const Proto& p) {
    const std::string label = std::string(p.name) + " cancel";
    const int64_t s = p.slot_ms;
    for (int64_t at : {200, 2000, 5000}) {
        TxConfig cfg = {p.name, p.id, 1500.0f, s, 0, "W1ABC K9XYZ -12"};
        cfg.cancel_ms = at;
        auto ev = run_tx(cfg, kDelay);
        const CatEvent* rx = first_of(ev, CatEvent::TX_END);
        check(rx && rx->time_ms >= s + at && rx->time_ms <= s + at + kDelay, "RX; at the cancel", label);
        check(count_of(ev, CatEvent::KEY_UP) == 0, "no TA0; on a cancel", label);
    }
    // A cancel inside the tail still ends at once.
    TxConfig cfg = {p.name, p.id, 1500.0f, s, 0, "W1ABC K9XYZ -12"};
    cfg.cancel_ms = kTxToneLeadMs + (int64_t)p.nn * p.sym_ms + 20;
    auto ev = run_tx(cfg, 1);
    const CatEvent* rx = first_of(ev, CatEvent::TX_END);
    check(rx && rx->time_ms - s <= cfg.cancel_ms + 1, "RX; at a cancel inside the tail", label);
}

int main() {
    printf("L5 TX CAT Sequence Test\n");
    printf("======================================\n");
    skip_values();
    for (const auto& p : kProtos) {
        on_time(p);
        armed_late(p);
        cancelled(p);
    }
    printf("%s\n", g_failed ? "FAILED" : "PASSED");
    return g_failed ? 1 : 0;
}
