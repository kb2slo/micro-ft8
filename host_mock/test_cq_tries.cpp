/*
 * Tries on every queue entry (docs/architecture/autoseq.md, Tries).
 *
 * - Call CQ queues one CQ with the station default's tries. Each CQ sent uses
 *   one, an answer gives none back, and at its count the CQ ends.
 * - Dropping the CQ (the CQ row's x) removes it; QSOs are left alone.
 * - An entry's own count survives its state changes and a change of the
 *   default; 0 returns it to the default.
 * - Lowering a count to the tries already sent ends the entry now, or after
 *   the try on the air.
 * - Call CQ picks the period an active QSO uses, otherwise the slot under way
 *   when it can still be keyed.
 */
#include <cstdio>
#include <string>

#include "autoseq.h"
#include "host_mocks.h"

static int s_failed = 0;
static void check(bool ok, const char* what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        s_failed = 1;
    }
}

static bool adif_cb(const std::string&, const std::string&, int, int) { return true; }

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

static std::string transmit()
{
    AutoseqTxEntry pending;
    if (!autoseq_fetch_pending_tx(pending)) return "";
    autoseq_on_tx_starting();
    autoseq_mark_sent(0);
    autoseq_tick(0, 0, 0);
    return pending.text;
}

static void reset(int tries)
{
    mock_clock_set(1000);  // parked entries stamp this; 0 reads as "not parked"
    autoseq_init();
    autoseq_set_station("KB2SLO", "FN30");
    autoseq_set_tries(tries);
    autoseq_set_adif_callback(adif_cb);
    autoseq_set_recent_qso_callback(nullptr);
}

static bool find(const char* dx, QsoContext* out)
{
    for (int i = 0; i < autoseq_active_count(); ++i) {
        if (autoseq_get_active_context(i, out) && out->dxcall == dx) return true;
    }
    return false;
}

static const char* const kCq = "CQ KB2SLO FN30";

// The row's Done-when: on a dead band a CQ of 4 tries sends 4 and ends.
static void test_cq_of_4_sends_4_and_ends()
{
    reset(4);
    check(autoseq_call_cq(0), "Call CQ queues a CQ");
    QsoContext cq;
    check(find("CQ", &cq) && cq.try_limit == 4 && cq.tries_sent == 0 && cq.slot_id == 0,
          "with the default's 4 tries, on the parity asked for");
    for (int i = 1; i <= 4; ++i) {
        check(transmit() == kCq, "a CQ goes out");
        if (i < 4) {
            check(find("CQ", &cq) && cq.tries_sent == i, "each CQ sent uses one try");
        }
    }
    check(autoseq_cq_entry_id() == 0 && autoseq_active_count() == 0, "the fourth ends the CQ");
    check(transmit().empty(), "and nothing more goes out");
}

static void test_second_call_keeps_the_queued_cq()
{
    reset(6);
    autoseq_call_cq(1);
    const uint16_t id = autoseq_cq_entry_id();
    transmit();
    check(!autoseq_call_cq(0), "a second Call CQ is refused");
    QsoContext cq;
    check(find("CQ", &cq) && cq.entry_id == id && cq.tries_sent == 1 && cq.slot_id == 1,
          "the queued CQ keeps its count and parity");
}

// An answer gives none back: the QSO goes first, then the CQ resumes where it was.
static void test_answer_gives_no_try_back()
{
    reset(3);
    autoseq_call_cq(0);
    check(transmit() == kCq, "CQ 1 of 3");
    decode(to_me("K1ABC", "FN42"));
    check(transmit() == "K1ABC KB2SLO -8", "the answer is worked first");
    QsoContext cq;
    check(find("CQ", &cq) && cq.tries_sent == 1, "the CQ still has 1 of 3 sent");
    decode(to_me("K1ABC", "R-10"));
    check(transmit() == "K1ABC KB2SLO RR73", "RR73 ends the QSO");
    check(transmit() == kCq, "CQ 2 of 3");
    check(transmit() == kCq, "CQ 3 of 3");
    check(autoseq_cq_entry_id() == 0, "three CQs in all, answer or not");
}

// The CQ row's x.
static void test_drop_ends_the_cq_and_leaves_qsos()
{
    reset(6);
    autoseq_call_cq(0);
    decode(to_me("K1ABC", "FN42"));
    check(autoseq_active_count() == 2, "a QSO and the CQ");
    check(autoseq_drop_by_entry_id(autoseq_cq_entry_id()), "x on the CQ row");
    QsoContext head;
    check(autoseq_active_count() == 1 && autoseq_get_active_context(0, &head) &&
              head.dxcall == "K1ABC",
          "the QSO is left");
    check(autoseq_cq_entry_id() == 0, "no CQ queued");
}

// The row's Done-when: raise one queued station to 12 mid-QSO, then the
// default to 3: it stays at 12, the others follow the default.
static void test_own_count_survives_the_default()
{
    reset(6);
    decode(to_me("K1ABC", "FN42"));
    decode(to_me("N0CALL", "EM12"));
    QsoContext k, n;
    check(find("K1ABC", &k), "K1ABC queued");
    check(autoseq_set_entry_tries(k.entry_id, 12) == AutoseqTriesResult::Ok, "K1ABC to 12");
    autoseq_set_tries(3);
    check(find("K1ABC", &k) && k.try_limit == 12 && k.tries_custom == 12, "K1ABC stays at 12");
    check(find("N0CALL", &n) && n.try_limit == 3 && n.tries_custom == 0, "N0CALL follows to 3");

    decode(to_me("K1ABC", "R-10"));  // a new step
    check(find("K1ABC", &k) && k.state == AutoseqState::ROGERS && k.try_limit == 12 &&
              k.tries_sent == 0,
          "its own count carries into the next step, starting over");

    check(autoseq_set_entry_tries(k.entry_id, 0) == AutoseqTriesResult::Ok, "Use default");
    check(find("K1ABC", &k) && k.try_limit == 3 && k.tries_custom == 0, "back on the default");
}

static void test_cq_count_changes_in_place()
{
    reset(6);
    autoseq_call_cq(0);
    const uint16_t id = autoseq_cq_entry_id();
    check(autoseq_set_entry_tries(id, 99) == AutoseqTriesResult::Ok, "a CQ runs to 99");
    check(autoseq_set_entry_tries(id, 100) == AutoseqTriesResult::OutOfRange, "not 100");
    autoseq_set_tries(2);
    QsoContext cq;
    check(find("CQ", &cq) && cq.try_limit == 99, "the CQ's own count survives the default");
    check(autoseq_set_entry_tries(id, 0) == AutoseqTriesResult::Ok && find("CQ", &cq) &&
              cq.try_limit == 2,
          "and Use default follows it again");
}

static void test_ranges_and_unknown_entries()
{
    reset(6);
    decode(to_me("K1ABC", "FN42"));
    QsoContext k;
    find("K1ABC", &k);
    check(autoseq_set_entry_tries(k.entry_id, 20) == AutoseqTriesResult::Ok, "a QSO runs to 20");
    check(autoseq_set_entry_tries(k.entry_id, 21) == AutoseqTriesResult::OutOfRange, "not 21");
    check(autoseq_set_entry_tries(k.entry_id, -1) == AutoseqTriesResult::OutOfRange, "not -1");
    check(autoseq_set_entry_tries(0, 5) == AutoseqTriesResult::UnknownEntry, "0 is no entry");
    check(autoseq_set_entry_tries(9999, 5) == AutoseqTriesResult::UnknownEntry, "nor is 9999");

    autoseq_set_tries(0);
    check(find("K1ABC", &k) && k.try_limit == 20, "own count untouched by a bad default");
    autoseq_set_entry_tries(k.entry_id, 0);
    check(find("K1ABC", &k) && k.try_limit == 1, "the default is at least 1");
    reset(50);
    decode(to_me("K1ABC", "FN42"));
    check(find("K1ABC", &k) && k.try_limit == AUTOSEQ_TRIES_MAX_QSO, "and at most 20");
}

// Lowered to the tries already sent, off the air: ends now. A QSO that has
// exchanged reports parks; a CQ, or a grid-only reply, is dropped.
static void test_lowering_ends_now_off_the_air()
{
    reset(6);
    decode(to_me("K1ABC", "FN42"));  // REPORT: we send our report
    transmit();
    transmit();
    QsoContext k;
    check(find("K1ABC", &k) && k.tries_sent == 2, "two reports sent");
    autoseq_set_entry_tries(k.entry_id, 2);
    check(!find("K1ABC", &k) && autoseq_active_count() == 0, "set to 2: ended at once");
    decode(to_me("K1ABC", "R-10"));
    check(find("K1ABC", &k) && k.state == AutoseqState::ROGERS, "parked, so a late R-report finds it");

    reset(6);
    autoseq_call_cq(0);
    transmit();
    transmit();
    transmit();
    autoseq_set_tries(3);
    check(autoseq_cq_entry_id() == 0, "a CQ with 3 sent ends when the default drops to 3");
}

// Lowered while that entry is on the air: the try in flight counts and ends it.
static void test_lowering_on_the_air_ends_after_the_try()
{
    reset(6);
    autoseq_call_cq(0);
    transmit();
    AutoseqTxEntry pending;
    autoseq_fetch_pending_tx(pending);
    autoseq_on_tx_starting();  // CQ 2 on the air
    autoseq_set_entry_tries(pending.entry_id, 1);
    check(autoseq_cq_entry_id() == pending.entry_id, "not dropped from under the TX");
    autoseq_mark_sent(0);
    autoseq_tick(0, 0, 0);
    check(autoseq_cq_entry_id() == 0, "its tick ends it");

    // A cancelled TX is not counted and has no tick: the entry is off the air.
    reset(6);
    autoseq_call_cq(0);
    transmit();
    autoseq_fetch_pending_tx(pending);
    autoseq_on_tx_starting();
    autoseq_on_tx_cancelled();
    check(autoseq_transmitting_entry_id() == 0, "nothing on the air after a cancel");
    autoseq_set_entry_tries(pending.entry_id, 1);
    check(autoseq_cq_entry_id() == 0, "so lowering it ends it at once");
}

static void test_call_cq_parity()
{
    reset(6);
    check(autoseq_cq_parity(0, true) == 0, "idle: the slot under way, while it can be keyed");
    check(autoseq_cq_parity(0, false) == 1, "otherwise the next");
    check(autoseq_cq_parity(1, false) == 0, "either way round");
    decode(to_me("K1ABC", "FN42"));  // heard on odd, we send on even
    check(autoseq_cq_parity(1, true) == 0, "an active QSO's parity wins");
}

static void test_flip_cq_parity()
{
    reset(6);
    autoseq_call_cq(0);
    const uint16_t id = autoseq_cq_entry_id();
    check(autoseq_set_cq_parity(id, 1) == AutoseqParityResult::Ok, "flip the CQ");
    QsoContext cq;
    check(find("CQ", &cq) && cq.slot_id == 1 && cq.entry_id == id, "same CQ, odd now");
    decode(to_me("K1ABC", "FN42"));
    QsoContext k;
    find("K1ABC", &k);
    check(autoseq_set_cq_parity(k.entry_id, 1) == AutoseqParityResult::NotCq,
          "a QSO's parity is not ours to flip");
    check(autoseq_set_cq_parity(9999, 1) == AutoseqParityResult::UnknownEntry, "unknown entry");
}

int main()
{
    test_cq_of_4_sends_4_and_ends();
    test_second_call_keeps_the_queued_cq();
    test_answer_gives_no_try_back();
    test_drop_ends_the_cq_and_leaves_qsos();
    test_own_count_survives_the_default();
    test_cq_count_changes_in_place();
    test_ranges_and_unknown_entries();
    test_lowering_ends_now_off_the_air();
    test_lowering_on_the_air_ends_after_the_try();
    test_call_cq_parity();
    test_flip_cq_parity();
    if (s_failed) return 1;
    printf("PASS: CQ tries, an entry's own tries, Call CQ parity\n");
    return 0;
}
