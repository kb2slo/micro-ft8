/*
 * The TX offset (docs/architecture/autoseq.md, TX offset): tx_offset_resolve()
 * and the offset a QSO's entry holds.
 *
 * - A QSO rolls once and keeps that offset for every later transmission,
 *   through its steps and a park and reactivation.
 * - An unanswered CQ rolls every time and holds nothing.
 * - Cursor and RX sources are as set, and hold nothing.
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

static void send()
{
    autoseq_on_tx_starting();
    autoseq_mark_sent(0);
    autoseq_tick(0, 0, 0);
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

    // A station answers: its QSO rolls once, and the CQ behind it still rolls.
    decode(to_me("K1ABC", "FN42"));
    check(arm(50) == 550 && arm(60) == 550, "the QSO that answered holds its own roll");
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
    if (s_failed) return 1;
    printf("PASS: a QSO holds its TX offset; a CQ rolls every time\n");
    return 0;
}
