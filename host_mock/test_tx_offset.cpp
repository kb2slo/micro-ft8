/*
 * The TX offset (docs/architecture/autoseq.md, TX offset): tx_offset_resolve()
 * and the offset a QSO's entry holds.
 *
 * - A QSO rolls once and keeps that offset for every later transmission,
 *   through its steps and a park and reactivation.
 * - An unanswered CQ rolls every time and holds nothing.
 * - Cursor and RX sources are as set, and hold nothing.
 * - A fresh entry starts where its station last heard us, for 10 minutes: a
 *   late answer to an opening call that used its tries, and a caller to our
 *   CQ (bench, 2026-11-14: W9KXI, and KG9OH). A station we call ourselves
 *   never heard our CQ, and rolls its own.
 */
#include <cstdio>
#include <string>
#include <vector>

#include "autoseq.h"
#include "host_mocks.h"
#include "tx_offset.h"

static int s_failed = 0;
static void check(bool ok, const char* what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        s_failed = 1;
    }
}

static bool adif_cb(const std::string&, const std::string&, int, int, RxMode) { return true; }

static UiRxLine to_me(const char* dx, const char* f3)
{
    UiRxLine m;
    m.field1 = "KB2SLO";
    m.field2 = dx;
    m.field3 = f3;
    m.snr = -8;
    m.offset_hz = 1200;
    m.slot_id = 1;
    m.is_to_me = true;
    return m;
}

static void decode(const UiRxLine& m) { autoseq_on_decodes(std::vector<UiRxLine>{m}); }

static void reset()
{
    mock_clock_set(1000);
    autoseq_init();
    autoseq_set_station("KB2SLO", "FN30");
    autoseq_set_tries(2);
    autoseq_set_adif_callback(adif_cb);
    autoseq_set_recent_qso_callback(nullptr);
}

// What the main board does at each arm (resolve_tx_offset(), tx_scheduler.cpp).
static int arm(uint32_t random, int src = kTxOffsetSrcRandom, int cursor_hz = 0)
{
    AutoseqTxEntry e;
    if (!autoseq_fetch_pending_tx(e)) return -1;
    TxOffsetIn in;
    in.src = src;
    in.cursor_hz = cursor_hz;
    in.rx_offset_hz = e.offset_hz;
    in.is_cq = e.text.rfind("CQ ", 0) == 0;
    in.held_hz = e.held_offset_hz;
    in.random = random;
    const TxOffsetOut out = tx_offset_resolve(in);
    if (out.hold) autoseq_hold_tx_offset(e.entry_id, out.offset_hz);
    return out.offset_hz;
}

// One transmission on `offset_hz`, the offset it was armed on (tx_start() passes it).
static void send(int offset_hz = 0)
{
    autoseq_on_tx_starting(offset_hz);
    autoseq_mark_sent(0);
    autoseq_tick(0, 0, 0);
}

static UiRxLine cq_from(const char* dx)
{
    UiRxLine m;
    m.field1 = "CQ";
    m.field2 = dx;
    m.field3 = "EN52";
    m.snr = -8;
    m.offset_hz = 1200;
    m.slot_id = 1;
    return m;
}

static void test_the_policy()
{
    TxOffsetIn in;
    in.random = 0;
    check(tx_offset_resolve(in).offset_hz == 500, "the lowest roll is 500 Hz");
    in.random = 2000;
    check(tx_offset_resolve(in).offset_hz == 2500, "the highest is 2500 Hz");
    in.random = 2001;
    check(tx_offset_resolve(in).offset_hz == 500, "and it wraps");
    check(tx_offset_resolve(in).hold, "a QSO's first roll is held");

    in.held_hz = 1234;
    check(tx_offset_resolve(in).offset_hz == 1234 && !tx_offset_resolve(in).hold,
          "a held offset is used as it is");
    in.is_cq = true;
    check(tx_offset_resolve(in).offset_hz == 500 && !tx_offset_resolve(in).hold,
          "a CQ rolls whatever is held, and holds nothing");

    TxOffsetIn cur;
    cur.src = kTxOffsetSrcCursor;
    cur.cursor_hz = 900;
    cur.held_hz = 1234;
    check(tx_offset_resolve(cur).offset_hz == 900 && !tx_offset_resolve(cur).hold,
          "the cursor wins over a held offset");
    cur.cursor_hz = 0;
    check(tx_offset_resolve(cur).offset_hz == 1500, "an unset cursor is 1500 Hz");

    TxOffsetIn rx;
    rx.src = kTxOffsetSrcRx;
    rx.rx_offset_hz = 1700;
    check(tx_offset_resolve(rx).offset_hz == 1700 && !tx_offset_resolve(rx).hold, "RX: DX's offset");
    rx.is_cq = true;
    rx.random = 10;
    check(tx_offset_resolve(rx).offset_hz == 510, "RX, for a CQ: a roll");
    rx.is_cq = false;
    rx.rx_offset_hz = 0;
    check(tx_offset_resolve(rx).offset_hz == 510 && tx_offset_resolve(rx).hold,
          "RX with no offset heard: a roll, held like any QSO's");
}

static void test_a_qso_keeps_its_offset()
{
    reset();
    decode(to_me("K1ABC", "FN42"));  // a caller: we send our report
    const int first = arm(100);
    check(first == 600, "the first transmission rolls");
    check(arm(999) == 600, "a re-arm in the same slot does not roll again");
    send();
    check(arm(777) == 600, "the report's second try is on the same offset");

    decode(to_me("K1ABC", "R-10"));  // the next step: RR73
    check(arm(1500) == 600, "and so is the RR73, a new step");
    send();  // logged, so the entry parks

    check(autoseq_active_count() == 0, "the QSO is parked after its RR73");
    decode(to_me("K1ABC", "R-10"));  // DX missed it and repeats: reactivated
    check(arm(42) == 600, "a reactivated QSO is still on its offset");
}

static void test_two_qsos_hold_their_own()
{
    reset();
    decode(to_me("K1ABC", "FN42"));
    const int a = arm(100);
    decode(to_me("N0CALL", "FN31"));
    // Whichever is at the head, each entry answers with its own offset.
    AutoseqTxEntry e;
    autoseq_fetch_pending_tx(e);
    const int head = arm(300);
    if (e.dxcall == "K1ABC") {
        check(head == a, "K1ABC kept its offset when another QSO joined");
    } else {
        check(head == 800, "the second QSO rolled its own");
    }
    QsoContext x;
    int held = 0;
    for (int i = 0; i < autoseq_active_count(); ++i) {
        if (autoseq_get_active_context(i, &x) && x.tx_offset_hz > 0) ++held;
    }
    check(held >= 1, "an entry holds its offset on its context");
}

static void test_a_cq_rolls_every_time()
{
    reset();
    autoseq_call_cq(0);
    check(arm(1) == 501, "a CQ rolls");
    send();
    check(arm(2) == 502, "and rolls again for the next one");
    check(!autoseq_hold_tx_offset(autoseq_cq_entry_id(), 1000), "a CQ cannot hold an offset");
    check(!autoseq_hold_tx_offset(9999, 1000), "nor can an unknown entry");

    // Nothing sent yet, so no station heard either roll: one that calls now rolls its own, and holds it.
    decode(to_me("K1ABC", "FN42"));
    check(arm(50) == 550 && arm(60) == 550, "a caller before any CQ went out holds its own roll");
}

// KG9OH, 2026-11-14: our CQ went out on 2185 Hz and the report to KG9OH on 996 Hz. A station that answers our CQ
// listens where it heard it.
static void test_a_caller_to_our_cq_is_answered_on_the_cqs_offset()
{
    reset();
    autoseq_call_cq(0);
    const int cq = arm(1685);
    check(cq == 2185, "the CQ rolls");
    send(cq);
    decode(to_me("KG9OH", "EN61"));
    check(arm(496) == 2185, "the report to its caller goes out on the CQ's offset");
    send(2185);
    decode(to_me("KG9OH", "R-12"));
    check(arm(7) == 2185, "and so does the RR73");

    // A station we tap ourselves never heard our CQ: its QSO rolls its own.
    reset();
    autoseq_call_cq(0);
    send(arm(1685));
    autoseq_on_touch(cq_from("N0CALL"));
    check(arm(300) == 800, "a station we call rolls its own, our CQ's notwithstanding");
}

// W9KXI, 2026-11-14: three calls on 1916 Hz used the opening call's tries and the entry was dropped; W9KXI then
// answered, and its fresh entry rolled 910 Hz for the R-4 and the 73.
static void test_a_late_answer_keeps_the_offset_it_was_called_on()
{
    reset();
    autoseq_on_touch(cq_from("W9KXI"));
    const int called = arm(1416);
    check(called == 1916, "the opening call rolls");
    send(called);
    check(arm(9) == 1916, "and holds it");
    send(1916);  // two tries at autoseq_set_tries(2): the opening call is used up and dropped
    check(autoseq_active_count() == 0, "the unanswered opening call is dropped");

    decode(to_me("W9KXI", "-04"));  // the late answer: its report
    check(arm(410) == 1916, "the fresh entry answers on the offset W9KXI was called on");
}

// The memory is the parked QSO's 10 minutes: past them, a fresh entry rolls, the CQ's caller included.
static void test_the_memory_lasts_ten_minutes()
{
    reset();
    autoseq_on_touch(cq_from("W9KXI"));
    send(arm(1416));
    send(1916);
    autoseq_call_cq(0);
    send(arm(1685));
    mock_clock_advance(AUTOSEQ_INACTIVE_EXPIRY_MS);
    decode(to_me("W9KXI", "-04"));
    check(arm(410) == 910, "ten minutes on, a late answer rolls its own");
    decode(to_me("KG9OH", "EN61"));
    QsoContext k;
    bool rolled = false;
    for (int i = 0; i < autoseq_active_count(); ++i) {
        if (autoseq_get_active_context(i, &k) && k.dxcall == "KG9OH") rolled = k.tx_offset_hz == 0;
    }
    check(rolled, "and so does a caller to a CQ sent ten minutes ago");
}

static void test_a_new_qso_with_the_same_station_rolls_again()
{
    reset();
    decode(to_me("K1ABC", "FN42"));
    check(arm(100) == 600, "the first QSO");
    send();
    send();  // tries used: the entry ends, parked
    reset();  // the parked entry gone, as after its 10 minutes
    decode(to_me("K1ABC", "FN42"));
    check(arm(200) == 700, "a fresh entry for the same call rolls its own");
}

int main()
{
    test_the_policy();
    test_a_qso_keeps_its_offset();
    test_two_qsos_hold_their_own();
    test_a_cq_rolls_every_time();
    test_a_new_qso_with_the_same_station_rolls_again();
    test_a_caller_to_our_cq_is_answered_on_the_cqs_offset();
    test_a_late_answer_keeps_the_offset_it_was_called_on();
    test_the_memory_lasts_ten_minutes();
    if (s_failed) return 1;
    printf("PASS: a QSO holds its TX offset; a fresh entry starts where its station heard us; a CQ rolls every time\n");
    return 0;
}
