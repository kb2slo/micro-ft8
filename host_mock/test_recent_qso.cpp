/*
 * A recently logged call is refused a new QSO, but messages for a QSO still in
 * the queue always reach it (autoseq_set_recent_qso_callback).
 *
 * The bug this pins: the decode pipeline used to drop every decode from a call
 * logged in the last 10 minutes before autoseq saw it. A QSO is logged when we
 * send RR73, so the station's 73 never arrived, and ROGERS resent RR73 on every
 * retry. Here the "logged" set is filled by the ADIF callback, as the firmware's
 * dedupe list is, and registered as the recent-QSO check.
 *
 * Also pins parked-entry expiry (AUTOSEQ_INACTIVE_EXPIRY_MS): a parked entry
 * catches DX's retries inside the window, and past it a call from that station
 * is a fresh QSO with a fresh report that is logged again.
 */
#include <cstdio>
#include <set>
#include <string>

#include "autoseq.h"
#include "host_mocks.h"

static std::set<std::string> s_logged;
static int s_log_count = 0;

static bool adif_cb(const std::string& dxcall, const std::string&, int, int) {
    s_logged.insert(dxcall);
    ++s_log_count;
    return true;
}

static bool recent_cb(const std::string& dxcall_norm) {
    return s_logged.count(dxcall_norm) != 0;
}

static int s_failed = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        s_failed = 1;
    }
}

static UiRxLine to_me(const char* dx, const char* f3) {
    UiRxLine m;
    m.field1 = "KB2SLO";
    m.field2 = dx;
    m.field3 = f3;
    m.text = std::string("KB2SLO ") + dx + " " + f3;
    m.snr = -8;
    m.offset_hz = 1200;
    m.slot_id = 1;
    m.is_to_me = true;
    return m;
}

static void decode(const UiRxLine& m) {
    autoseq_on_decodes(std::vector<UiRxLine>{m});
}

// One of our TX slots: send queue[0] if there is anything, then tick.
// Returns the text sent, or "" if nothing was queued.
static std::string transmit() {
    AutoseqTxEntry pending;
    if (!autoseq_fetch_pending_tx(pending)) return "";
    autoseq_on_tx_starting();
    autoseq_mark_sent(0);
    autoseq_tick(0, 0, 0);
    return pending.text;
}

static void reset() {
    mock_clock_set(1000);  // parked entries stamp this; 0 reads as "not parked"
    autoseq_init();
    autoseq_set_station("KB2SLO", "FN30");
    autoseq_set_max_retry(5);
    autoseq_set_adif_callback(adif_cb);
    autoseq_set_recent_qso_callback(recent_cb);
    s_logged.clear();
    s_log_count = 0;
}

// We CQ, K1ABC answers, we end with RR73, K1ABC sends 73: one RR73, then done.
static void test_rr73_once_then_73_closes() {
    reset();
    autoseq_start_cq(0);
    check(transmit() == "CQ KB2SLO FN30", "CQ goes out");

    decode(to_me("K1ABC", "FN42"));
    check(transmit() == "K1ABC KB2SLO -8", "report to the caller");

    decode(to_me("K1ABC", "R-10"));
    check(transmit() == "K1ABC KB2SLO RR73", "RR73 after their R-report");
    check(s_log_count == 1 && recent_cb("K1ABC"), "RR73 logs the QSO");

    decode(to_me("K1ABC", "73"));
    check(autoseq_active_count() == 0, "their 73 ends the QSO despite the log");
    check(transmit().empty(), "no second RR73");

    autoseq_start_cq(0);
    check(transmit() == "CQ KB2SLO FN30", "the beacon CQ resumes at once");
}

// They missed our RR73 and resend R+report: RR73 again, fresh retries.
static void test_resent_r_report_gets_rr73() {
    reset();
    autoseq_start_cq(0);
    transmit();
    decode(to_me("K1ABC", "FN42"));
    transmit();
    decode(to_me("K1ABC", "R-10"));
    check(transmit() == "K1ABC KB2SLO RR73", "first RR73 logs");

    decode(to_me("K1ABC", "R-10"));
    QsoContext head;
    check(autoseq_get_active_context(0, &head) && head.state == AutoseqState::ROGERS &&
              head.retry_counter == 0,
          "resent R-report reaches the logged QSO and resets its retries");
    check(transmit() == "K1ABC KB2SLO RR73", "RR73 again");
    check(s_log_count == 1, "logged once");
}

// We answered their CQ and sent 73; a late RR73 still gets another 73.
static void test_late_rr73_reactivates_parked_signoff() {
    reset();
    UiRxLine cq;
    cq.field1 = "CQ";
    cq.field2 = "W1AW";
    cq.field3 = "FN31";
    cq.snr = -5;
    cq.offset_hz = 900;
    cq.slot_id = 1;
    cq.is_cq = true;
    check(autoseq_on_touch(cq) == AutoseqTouchResult::Queued, "reply to W1AW's CQ");
    check(transmit() == "W1AW KB2SLO FN30", "TX1 to W1AW");

    decode(to_me("W1AW", "-12"));
    check(transmit() == "W1AW KB2SLO R-5", "R-report to W1AW");

    decode(to_me("W1AW", "RR73"));
    check(transmit() == "W1AW KB2SLO 73", "73 logs and parks");
    check(recent_cb("W1AW") && autoseq_active_count() == 0, "parked after 73");

    decode(to_me("W1AW", "RR73"));
    check(transmit() == "W1AW KB2SLO 73", "late RR73 still answered with 73");
    check(s_log_count == 1, "logged once");
}

// After the QSO is gone from the queue, the same call is refused a new one.
static void test_recent_call_cannot_start_new_qso() {
    reset();
    autoseq_start_cq(0);
    transmit();
    decode(to_me("K1ABC", "FN42"));
    transmit();
    decode(to_me("K1ABC", "R-10"));
    transmit();
    decode(to_me("K1ABC", "73"));
    check(autoseq_active_count() == 0, "QSO done");

    decode(to_me("K1ABC", "FN42"));
    check(autoseq_active_count() == 0, "a recent call cannot start a second QSO");

    decode(to_me("N0CALL", "EM12"));
    check(autoseq_active_count() == 1, "another caller still can");

    s_logged.clear();  // the 10-minute window has passed
    decode(to_me("K1ABC", "FN42"));
    check(autoseq_active_count() == 2, "K1ABC can call again once the window passes");
}

// Answer dx's CQ and finish with our 73, which parks the entry.
static void work_to_our_73(const char* dx, const char* grid) {
    UiRxLine cq;
    cq.field1 = "CQ";
    cq.field2 = dx;
    cq.field3 = grid;
    cq.snr = -5;
    cq.offset_hz = 900;
    cq.slot_id = 1;
    cq.is_cq = true;
    autoseq_on_touch(cq);
    transmit();
    decode(to_me(dx, "-12"));
    transmit();
    decode(to_me(dx, "RR73"));
    transmit();
}

static void work_w1aw_to_our_73() { work_to_our_73("W1AW", "FN31"); }

// Inside the window a parked entry still catches DX's retries.
static void test_parked_entry_lives_through_the_window() {
    reset();
    work_w1aw_to_our_73();
    mock_clock_advance(AUTOSEQ_INACTIVE_EXPIRY_MS - 60 * 1000);
    decode(to_me("W1AW", "RR73"));
    check(transmit() == "W1AW KB2SLO 73", "late RR73 inside the window gets 73");
}

// Past the window the parked entry is gone: a new call is a new QSO, with a
// fresh report, and it is logged again. Without expiry the old SIGNOFF entry
// reactivated, answered the grid with 73, and never logged.
static void test_parked_entry_expires() {
    reset();
    work_w1aw_to_our_73();
    check(s_log_count == 1, "first QSO logged");

    mock_clock_advance(AUTOSEQ_INACTIVE_EXPIRY_MS + 1000);
    s_logged.clear();  // the recently-worked window has passed too

    UiRxLine again = to_me("W1AW", "FN31");
    again.snr = -15;
    decode(again);
    check(transmit() == "W1AW KB2SLO -15", "a new QSO with a fresh report, not a 73");
    decode(to_me("W1AW", "R-09"));
    check(transmit() == "W1AW KB2SLO RR73", "RR73 on the new QSO");
    check(s_log_count == 2, "the new QSO is logged");
}

// Expiry drops only the stale entry; a newer parked one survives.
static void test_expiry_keeps_newer_parked_entries() {
    reset();
    work_w1aw_to_our_73();                                       // parked at t0
    mock_clock_advance(AUTOSEQ_INACTIVE_EXPIRY_MS / 2);
    work_to_our_73("N0CALL", "EM12");                            // parked at t0 + W/2
    mock_clock_advance(AUTOSEQ_INACTIVE_EXPIRY_MS / 2 + 1000);   // W1AW past W, N0CALL not

    decode(to_me("N0CALL", "RR73"));
    check(transmit() == "N0CALL KB2SLO 73", "the newer parked entry survived");

    s_logged.clear();
    decode(to_me("W1AW", "FN31"));
    check(transmit() == "W1AW KB2SLO -8", "the stale one expired: W1AW starts fresh");
}

int main() {
    test_rr73_once_then_73_closes();
    test_resent_r_report_gets_rr73();
    test_late_rr73_reactivates_parked_signoff();
    test_recent_call_cannot_start_new_qso();
    test_parked_entry_lives_through_the_window();
    test_parked_entry_expires();
    test_expiry_keeps_newer_parked_entries();
    if (s_failed) return 1;
    printf("PASS: recent-QSO filter only blocks new QSOs; parked entries expire\n");
    return 0;
}
