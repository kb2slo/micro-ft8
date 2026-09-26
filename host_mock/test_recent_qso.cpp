/*
 * A recently logged call is refused a new QSO, but messages for a QSO still in
 * the queue always reach it (autoseq_set_recent_qso_callback).
 *
 * The bug this pins: the decode pipeline used to drop every decode from a call
 * logged in the last 10 minutes before autoseq saw it. A QSO is logged when we
 * send RR73, so the station's 73 never arrived, and ROGERS resent RR73 on every
 * retry. Here the "logged" set is filled by the ADIF callback, as the firmware's
 * dedupe list is, and registered as the recent-QSO check.
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

int main() {
    test_rr73_once_then_73_closes();
    test_resent_r_report_gets_rr73();
    test_late_rr73_reactivates_parked_signoff();
    test_recent_call_cannot_start_new_qso();
    if (s_failed) return 1;
    printf("PASS: recent-QSO filter only blocks new QSOs\n");
    return 0;
}
