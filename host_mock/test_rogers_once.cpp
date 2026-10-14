/*
 * ROGERS sends RR73 once, then parks (autoseq_tick()). The QSO is logged with
 * that RR73 and DX's 73 is optional, so a blind resend almost always went to a
 * station that already had it: in the field ROGERS used up all its retries,
 * about 2.5 minutes of RR73s. A DX who missed it resends R+report, which reaches
 * the parked entry and gets RR73 again.
 *
 * Pins: one RR73 to a silent DX; a resent R+report, at once or minutes later
 * inside the parked window, gets RR73 each time and logs nothing new; a failed
 * log write keeps RR73 going, which retries the write, up to the setting; the
 * earlier states still resend up to the Station.txt setting.
 */
#include <cstdio>
#include <string>

#include "autoseq.h"
#include "host_mocks.h"

static int s_log_count = 0;
static bool s_log_fails = false;

static bool adif_cb(const std::string&, const std::string&, int, int) {
    if (s_log_fails) return false;
    ++s_log_count;
    return true;
}

static bool always_recent(const std::string&) {
    return true;
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
    autoseq_set_tries(AUTOSEQ_TRIES);
    autoseq_set_adif_callback(adif_cb);
    autoseq_set_recent_qso_callback(nullptr);
    s_log_count = 0;
    s_log_fails = false;
}

// One CQ that ends after it is sent: the CQ these tests open a QSO with.
static void one_cq() {
    autoseq_call_cq(0);
    autoseq_set_entry_tries(autoseq_cq_entry_id(), 1);
}

static const char* const kRr73 = "K1ABC KB2SLO RR73";

// We CQ, K1ABC answers with a grid, and their R-report gets our first RR73.
static void work_to_first_rr73() {
    one_cq();
    transmit();
    decode(to_me("K1ABC", "FN42"));
    transmit();
    decode(to_me("K1ABC", "R-10"));
    check(transmit() == kRr73, "first RR73");
}

static void test_silent_dx_gets_one_rr73() {
    reset();
    work_to_first_rr73();
    check(s_log_count == 1, "the first RR73 logs");
    check(autoseq_active_count() == 0, "the entry parks right after it");
    for (int i = 0; i < AUTOSEQ_TRIES; ++i) {
        check(transmit().empty(), "no RR73 resent to a silent DX");
    }
    one_cq();
    check(transmit() == "CQ KB2SLO FN30", "a CQ is not held behind it");
}

// DX missed our RR73 and says so with R+report: RR73 again, once each time.
static void test_resent_r_report_gets_rr73_each_time() {
    reset();
    work_to_first_rr73();
    for (int i = 0; i < 3; ++i) {
        decode(to_me("K1ABC", "R-10"));
        QsoContext head;
        check(autoseq_get_active_context(0, &head) && head.state == AutoseqState::ROGERS &&
                  head.tries_sent == 0,
              "a resent R-report reactivates the parked entry with fresh tries");
        check(transmit() == kRr73, "and gets RR73");
        check(autoseq_active_count() == 0, "which parks it again");
    }
    check(s_log_count == 1, "logged once");
}

// Minutes later, inside the parked window, and with the call already "recent":
// the parked entry is found before the recently-worked check.
static void test_late_r_report_inside_window_reactivates() {
    reset();
    work_to_first_rr73();
    autoseq_set_recent_qso_callback(always_recent);  // logged at that RR73
    mock_clock_advance(AUTOSEQ_INACTIVE_EXPIRY_MS - 60 * 1000);
    decode(to_me("K1ABC", "R-10"));
    check(transmit() == kRr73, "a late R-report inside the window gets RR73");
    check(s_log_count == 1, "and logs nothing new");
}

// A failed write leaves the entry unlogged, and each RR73 retries it.
static void test_failed_log_write_keeps_rr73_going() {
    reset();
    s_log_fails = true;
    work_to_first_rr73();
    check(s_log_count == 0, "the write failed");
    check(transmit() == kRr73, "unlogged: RR73 again, which retries the write");
    s_log_fails = false;
    check(transmit() == kRr73, "the next RR73's write succeeds");
    check(s_log_count == 1, "logged once");
    check(transmit().empty(), "logged: parked, no more RR73");
}

static void test_failed_log_write_resends_up_to_the_setting() {
    reset();
    autoseq_set_tries(3);
    s_log_fails = true;
    work_to_first_rr73();
    check(transmit() == kRr73, "try 2");
    check(transmit() == kRr73, "try 3");
    check(transmit().empty(), "the setting's 3 tries used, parked unlogged");
}

// Counts how many times `text` goes out before something else (or nothing).
static int count_sends(const char* text) {
    int n = 0;
    while (transmit() == text) ++n;
    return n;
}

static void test_earlier_states_keep_the_setting() {
    reset();
    one_cq();
    transmit();
    decode(to_me("K1ABC", "FN42"));
    check(count_sends("K1ABC KB2SLO -8") == AUTOSEQ_TRIES,
          "an unanswered report goes out the default's tries");

    reset();
    autoseq_set_tries(4);
    one_cq();
    transmit();
    decode(to_me("K1ABC", "-12"));
    check(count_sends("K1ABC KB2SLO R-8") == 4,
          "an unanswered R-report goes out the setting's tries");
}

int main() {
    test_silent_dx_gets_one_rr73();
    test_resent_r_report_gets_rr73_each_time();
    test_late_r_report_inside_window_reactivates();
    test_failed_log_write_keeps_rr73_going();
    test_failed_log_write_resends_up_to_the_setting();
    test_earlier_states_keep_the_setting();
    if (s_failed) return 1;
    printf("PASS: ROGERS sends RR73 once; a resent R-report gets another\n");
    return 0;
}
