/*
 * An entry runs in the mode it was heard in (docs/architecture/autoseq.md, The queue; epic u8mc, row grqg).
 *
 * - A tap on an FT4 decode queues an FT4 entry, and the TX autoseq hands the board says FT4.
 * - A decode continues the entry for its call in its own mode only: the same station on FT8 and on FT4 is
 *   two QSOs, as POTA and the awards count them, and an FT8 report must not advance the FT4 QSO.
 * - A tap in one mode leaves a QSO with that station in the other mode alone.
 * - A parked entry is found again only by a decode in its own mode.
 * - The QSO is logged with its entry's mode.
 * - The CQ is FT8's, so Call CQ takes an FT8 QSO's parity, never an FT4 one's.
 * - Recently worked is asked in the decode's mode: a station just worked on FT8 can start a QSO on FT4.
 * - An entry's QUEUE_ENTRY event carries its mode, its mode's dial and its held TX offset (queue_event.h).
 */
#include <cstdio>
#include <string>

#include "autoseq.h"
#include "host_mocks.h"
#include "queue_event.h"

static int s_failed = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        s_failed = 1;
    }
}

static RxMode s_logged_mode = RxMode::FT8;
static int s_log_count = 0;
static bool adif_cb(const std::string&, const std::string&, int, int, RxMode mode) {
    s_logged_mode = mode;
    ++s_log_count;
    return true;
}

static UiRxLine line(const char* f1, const char* dx, const char* f3, RxMode mode, int slot) {
    UiRxLine m;
    m.field1 = f1;
    m.field2 = dx;
    m.field3 = f3;
    m.text = std::string(f1) + " " + dx + (f3[0] ? " " : "") + f3;
    m.snr = -8;
    m.offset_hz = 1200;
    m.slot_id = slot;
    m.is_cq = std::string(f1) == "CQ";
    m.is_to_me = std::string(f1) == "KB2SLO";
    m.mode = mode;
    return m;
}

static void decode(const UiRxLine& m) { autoseq_on_decodes(std::vector<UiRxLine>{m}); }

static void reset() {
    mock_clock_set(1000);
    autoseq_init();
    autoseq_set_station("KB2SLO", "EM10");
    autoseq_set_tries(3);
    autoseq_set_adif_callback(adif_cb);
    s_log_count = 0;
}

static int count(const char* dx, RxMode mode, QsoContext* out = nullptr) {
    int n = 0;
    QsoContext c;
    for (int i = 0; i < autoseq_active_count(); ++i) {
        if (autoseq_get_active_context(i, &c) && c.dxcall == dx && c.mode == mode) {
            ++n;
            if (out) *out = c;
        }
    }
    return n;
}

static std::string transmit(RxMode* mode = nullptr) {
    AutoseqTxEntry pending;
    if (!autoseq_fetch_pending_tx(pending)) return "";
    if (mode) *mode = pending.mode;
    autoseq_on_tx_starting();
    autoseq_mark_sent(0);
    autoseq_tick(0, 0, 0);
    return pending.text;
}

static void test_touch_ft4_queues_ft4() {
    reset();
    check(autoseq_on_touch(line("CQ", "K1ABC", "FN42", RxMode::FT4, 0)) == AutoseqTouchResult::Queued, "FT4 tap queued");
    check(count("K1ABC", RxMode::FT4) == 1 && count("K1ABC", RxMode::FT8) == 0, "the entry is FT4's");
    RxMode m = RxMode::FT8;
    check(transmit(&m) == "K1ABC KB2SLO EM10" && m == RxMode::FT4, "its TX goes out on FT4");
}

static void test_decode_continues_own_mode_only() {
    reset();
    autoseq_on_touch(line("CQ", "K1ABC", "FN42", RxMode::FT4, 0));
    autoseq_on_touch(line("CQ", "K1ABC", "FN42", RxMode::FT8, 0));
    check(count("K1ABC", RxMode::FT4) == 1 && count("K1ABC", RxMode::FT8) == 1, "one station, two modes, two entries");
    decode(line("KB2SLO", "K1ABC", "-10", RxMode::FT8, 1));
    QsoContext ft8, ft4;
    count("K1ABC", RxMode::FT8, &ft8);
    count("K1ABC", RxMode::FT4, &ft4);
    check(ft8.state == AutoseqState::ROGER_REPORT, "the FT8 report advances the FT8 QSO");
    check(ft4.state == AutoseqState::REPLYING, "and leaves the FT4 QSO where it was");
}

static void test_touch_leaves_other_mode() {
    reset();
    decode(line("KB2SLO", "K1ABC", "FN42", RxMode::FT8, 1));
    check(count("K1ABC", RxMode::FT8) == 1, "K1ABC calls us on FT8");
    autoseq_on_touch(line("CQ", "K1ABC", "FN42", RxMode::FT4, 0));
    check(count("K1ABC", RxMode::FT8) == 1 && count("K1ABC", RxMode::FT4) == 1, "a tap on FT4 keeps the FT8 QSO");
}

static void test_parked_found_in_own_mode() {
    reset();
    autoseq_on_touch(line("CQ", "K1ABC", "FN42", RxMode::FT4, 0));
    decode(line("KB2SLO", "K1ABC", "-10", RxMode::FT4, 1));  // REPLYING -> ROGER_REPORT on FT4
    QsoContext c;
    count("K1ABC", RxMode::FT4, &c);
    check(autoseq_drop_by_entry_id(c.entry_id), "the FT4 QSO is parked");
    decode(line("KB2SLO", "K1ABC", "R-12", RxMode::FT8, 1));
    check(count("K1ABC", RxMode::FT8) == 0 && count("K1ABC", RxMode::FT4) == 0,
          "an FT8 R-report neither revives the FT4 QSO nor starts one");
    decode(line("KB2SLO", "K1ABC", "R-12", RxMode::FT4, 1));
    check(count("K1ABC", RxMode::FT4, &c) == 1 && c.state == AutoseqState::ROGER_REPORT, "the FT4 R-report revives it");
}

static void test_logged_with_mode() {
    reset();
    autoseq_on_touch(line("CQ", "K1ABC", "FN42", RxMode::FT4, 0));
    decode(line("KB2SLO", "K1ABC", "-10", RxMode::FT4, 1));
    decode(line("KB2SLO", "K1ABC", "RR73", RxMode::FT4, 1));
    transmit();  // our 73 logs it
    check(s_log_count == 1 && s_logged_mode == RxMode::FT4, "an FT4 QSO is logged as FT4");
}

static void test_cq_parity_from_ft8_only() {
    reset();
    autoseq_on_touch(line("CQ", "K1ABC", "FN42", RxMode::FT4, 0));  // FT4, TX parity 1
    check(autoseq_cq_parity(0, true) == 0, "an FT4 QSO does not set the FT8 CQ's parity");
    autoseq_on_touch(line("CQ", "K2DEF", "FN42", RxMode::FT8, 0));  // FT8, TX parity 1
    check(autoseq_cq_parity(0, true) == 1, "an FT8 QSO does");
}

static bool recent_ft8_only(const std::string&, RxMode mode) { return mode == RxMode::FT8; }

static void test_recent_per_mode() {
    reset();
    autoseq_set_recent_qso_callback(recent_ft8_only);
    decode(line("KB2SLO", "K1ABC", "FN42", RxMode::FT8, 1));
    check(count("K1ABC", RxMode::FT8) == 0, "worked on FT8 just now: no new FT8 QSO");
    decode(line("KB2SLO", "K1ABC", "FN42", RxMode::FT4, 1));
    check(count("K1ABC", RxMode::FT4) == 1, "but a new FT4 one");
    autoseq_set_recent_qso_callback(nullptr);
}

static void test_queue_event_carries_mode() {
    reset();
    autoseq_on_touch(line("CQ", "K1ABC", "FN42", RxMode::FT4, 0));
    autoseq_on_touch(line("CQ", "K2DEF", "FN42", RxMode::FT8, 0));
    QsoContext ft4, ft8;
    check(count("K1ABC", RxMode::FT4, &ft4) == 1 && count("K2DEF", RxMode::FT8, &ft8) == 1, "both queued");
    const uint32_t ft8_dial = 14074000u;
    const int32_t ft4_offset = 6000;  // 20 m: FT4 at 14.080 MHz
    porta_queue_entry_event_t q = queue_entry_event(ft4, ft8_dial, ft4_offset);
    check(q.mode == PORTA_QUEUE_MODE_FT4 && q.dial_hz == 14080000u, "an FT4 entry's event carries FT4 and its dial");
    check(q.offset_hz == 0, "no offset before its first transmission");
    q = queue_entry_event(ft8, ft8_dial, ft4_offset);
    check(q.mode == PORTA_QUEUE_MODE_FT8 && q.dial_hz == ft8_dial, "an FT8 entry's event carries FT8 and its dial");
    check(std::string(q.dxcall) == "K2DEF" && q.entry_id == ft8.entry_id, "and its call and id");

    check(autoseq_hold_tx_offset(ft4.entry_id, 1916), "the FT4 QSO holds an offset");
    count("K1ABC", RxMode::FT4, &ft4);
    check(queue_entry_event(ft4, ft8_dial, ft4_offset).offset_hz == 1916, "its event carries the held offset");
    check(queue_entry_event(ft4, 0, ft4_offset).dial_hz == 0, "an unknown dial stays unknown in FT4");

    q = queue_entry_gone_event(ft4.entry_id);
    check(q.state == PORTA_QUEUE_STATE_IDLE && q.mode == 0 && q.dial_hz == 0 && q.offset_hz == 0 && !q.dxcall[0],
          "a gone entry's event is IDLE and nothing else");
}

int main() {
    test_queue_event_carries_mode();
    test_touch_ft4_queues_ft4();
    test_decode_continues_own_mode_only();
    test_touch_leaves_other_mode();
    test_parked_found_in_own_mode();
    test_logged_with_mode();
    test_cq_parity_from_ft8_only();
    test_recent_per_mode();
    if (s_failed) return 1;
    printf("mode entries: OK\n");
    return 0;
}
