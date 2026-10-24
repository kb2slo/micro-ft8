/*
 * The heard-grid table (heard_grid.h) and the log's use of it
 * (autoseq_set_heard_grid_callback).
 *
 * The bug this pins: a QSO opened by tapping a CQ ran out of TX1 tries before
 * DX's report was decoded, so it was dropped, and the report started a fresh
 * QSO that logged no grid although the tapped CQ carried one. A tail-ended QSO
 * logged none either. Each now logs the grid DX last sent in any decode.
 */
#include <cstdio>
#include <string>

#include "autoseq.h"
#include "decode_fields.h"
#include "heard_grid.h"
#include "host_mocks.h"

static int s_failed = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        s_failed = 1;
    }
}

// A standard message, as every scenario here is unless it says otherwise.
static void note(const char* sender, const char* field3, int64_t now_ms) {
    heard_grid_note(sender, field3, true, now_ms);
}

static std::string lookup(const char* call, int64_t now_ms) {
    std::string grid;
    return heard_grid_lookup(call, now_ms, &grid) ? grid : "";
}

// ---- The table ----

static void test_records_a_cq_grid() {
    heard_grid_clear();
    note("LW6EMN", "GF03", 0);
    check(lookup("LW6EMN", 1000) == "GF03", "a CQ's grid is recorded");
    check(lookup("lw6emn", 1000) == "GF03", "lookup is case-insensitive");
    note("<PJ4/K1ABC>", "fk52", 0);
    check(lookup("PJ4/K1ABC", 1000) == "FK52", "<> stripped and grid upper-cased");
    check(lookup("W1AW", 1000).empty(), "an unheard call has no grid");
}

static void test_ignores_what_is_not_a_grid() {
    heard_grid_clear();
    const char* not_grids[] = {"RR73", "RRR", "73", "-13", "R-05", "+05", "", "FN3", "FN312",
                               "SA12", "ZZ12", "1B SCV"};
    for (const char* f3 : not_grids) note("K1ABC", f3, 0);
    check(lookup("K1ABC", 0).empty(), "only a Maidenhead square is recorded, not RR73");

    const char* not_calls[] = {"<...>", "CQ", "DX", "", "K1", "ABCDEFGHIJKL1", "K1-ABC"};
    for (const char* call : not_calls) note(call, "FN42", 0);
    check(lookup("<...>", 0).empty() && lookup("CQ", 0).empty() && lookup("DX", 0).empty(),
          "an unresolved hash or a non-call is not recorded");
}

// Free text can spell `CQ K1ABC FN42` too; only a standard message's grid is
// the sender's own.
static void test_only_standard_messages() {
    heard_grid_clear();
    heard_grid_note("K1ABC", "FN42", false, 0);
    check(lookup("K1ABC", 0).empty(), "a free-text or other non-standard message is not recorded");
    heard_grid_note("K1ABC", "FN42", true, 0);
    check(lookup("K1ABC", 0) == "FN42", "a standard message is");
}

// The CQ forms reach the table through the decode's own fields.
static void test_cq_with_a_modifier() {
    heard_grid_clear();
    const char* texts[] = {"CQ DX LW6EMN GF03", "CQ POTA K1ABC FN42", "CQ NA W1AW FN31",
                           "W9XYZ N0CALL EM12"};
    for (const char* text : texts) {
        RxDecodeEntry d = {};
        snprintf(d.text, sizeof(d.text), "%s", text);
        decode_fields_fill(&d);
        note(d.field2, d.field3, 0);
    }
    check(lookup("LW6EMN", 0) == "GF03", "CQ DX");
    check(lookup("K1ABC", 0) == "FN42", "CQ POTA");
    check(lookup("W1AW", 0) == "FN31", "CQ NA");
    check(lookup("N0CALL", 0) == "EM12", "a reply's grid is the sender's");
    check(lookup("W9XYZ", 0).empty(), "not the station it was sent to");
}

static void call_n(int n, char* out) {
    snprintf(out, 12, "K%dAB", n);
}

static void test_one_entry_per_call() {
    heard_grid_clear();
    char call[12];
    for (int i = 0; i < HEARD_GRID_CAPACITY; ++i) {
        call_n(i, call);
        note(call, "FN42", 1000 + i);
    }
    // K0AB moved: its entry takes the new grid rather than a second slot.
    note("K0AB", "EM12", 5000);
    check(lookup("K0AB", 5000) == "EM12", "a call heard again takes the new grid");
    call_n(1, call);
    check(lookup(call, 5000) == "FN42", "updating in place evicted nothing");
}

static void test_full_table_drops_the_call_heard_longest_ago() {
    heard_grid_clear();
    char call[12];
    for (int i = 0; i < HEARD_GRID_CAPACITY; ++i) {
        call_n(i, call);
        note(call, "FN42", 1000 + i);
    }
    note("K0AB", "FN42", 9000);  // heard again: now the newest
    note("W1AW", "FN31", 9001);  // one past capacity
    check(lookup("W1AW", 9001) == "FN31", "the new call is recorded");
    check(lookup("K0AB", 9001) == "FN42", "a call heard again is not the one dropped");
    call_n(1, call);
    check(lookup(call, 9001).empty(), "the call heard longest ago is dropped");
    call_n(2, call);
    check(lookup(call, 9001) == "FN42", "only one call is dropped");
}

static void test_expires_four_hours_after_last_heard() {
    heard_grid_clear();
    note("LW6EMN", "GF03", 1000);
    check(lookup("LW6EMN", 1000 + HEARD_GRID_EXPIRY_MS - 1) == "GF03", "kept just inside 4 h");
    check(lookup("LW6EMN", 1000 + HEARD_GRID_EXPIRY_MS).empty(), "gone at 4 h");

    note("LW6EMN", "GF03", 1000 + HEARD_GRID_EXPIRY_MS);
    check(lookup("LW6EMN", 1000 + 2 * HEARD_GRID_EXPIRY_MS - 1) == "GF03",
          "hearing it again restarts the 4 h");
}

// ---- The log ----

static std::string s_logged_grid;
static int s_log_count = 0;

static bool adif_cb(const std::string&, const std::string& grid, int, int) {
    s_logged_grid = grid;
    ++s_log_count;
    return true;
}

static bool heard_cb(const std::string& dxcall_norm, std::string* grid_out) {
    return heard_grid_lookup(dxcall_norm.c_str(), mock_clock_get(), grid_out);
}

static UiRxLine msg(const char* f1, const char* f2, const char* f3, int snr = -8) {
    UiRxLine m;
    m.field1 = f1;
    m.field2 = f2;
    m.field3 = f3;
    m.text = std::string(f1) + " " + f2 + " " + f3;
    m.snr = snr;
    m.offset_hz = 1200;
    m.slot_id = 1;
    m.is_cq = (m.field1 == "CQ");
    m.is_to_me = (m.field1 == "KB2SLO");
    return m;
}

// Every decode is offered to the table, as decode_pipeline does, and the
// ones addressed to us go to autoseq.
static void decode(const UiRxLine& m) {
    note(m.field2.c_str(), m.field3.c_str(), mock_clock_get());
    if (m.is_to_me) autoseq_on_decodes(std::vector<UiRxLine>{m});
}

static std::string transmit() {
    AutoseqTxEntry pending;
    if (!autoseq_fetch_pending_tx(pending)) return "";
    autoseq_on_tx_starting();
    autoseq_mark_sent(0);
    autoseq_tick(0, 0, 0);
    mock_clock_advance(30000);
    return pending.text;
}

static void reset() {
    mock_clock_set(1000);
    heard_grid_clear();
    autoseq_init();
    autoseq_set_station("KB2SLO", "EM10");
    autoseq_set_tries(2);
    autoseq_set_adif_callback(adif_cb);
    autoseq_set_heard_grid_callback(heard_cb);
    s_logged_grid.clear();
    s_log_count = 0;
}

// The field report: we tapped LW6EMN's CQ, our TX1 ran out of tries, and
// their report then started a fresh QSO. It logs the CQ's grid.
static void test_tapped_cq_outlives_its_dropped_entry() {
    reset();
    const UiRxLine cq = msg("CQ", "LW6EMN", "GF03", -7);
    decode(cq);
    check(autoseq_on_touch(cq) == AutoseqTouchResult::Queued, "tap LW6EMN's CQ");
    check(transmit() == "LW6EMN KB2SLO EM10", "TX1");
    check(transmit() == "LW6EMN KB2SLO EM10", "last TX1");
    check(autoseq_active_count() == 0, "tries used: the entry is dropped");

    decode(msg("KB2SLO", "LW6EMN", "-13", -16));
    check(transmit() == "LW6EMN KB2SLO R-16", "their report starts a fresh QSO");
    decode(msg("KB2SLO", "LW6EMN", "RR73", -16));
    check(transmit() == "LW6EMN KB2SLO 73", "73");
    check(s_log_count == 1 && s_logged_grid == "GF03", "logged with the tapped CQ's grid");
}

// Tail-ending someone else's RR73: the QSO never carries a grid.
static void test_tail_ended_qso_logs_the_heard_grid() {
    reset();
    decode(msg("CQ", "K1ABC", "FN42"));
    mock_clock_advance(60 * 60 * 1000);  // heard an hour ago
    const UiRxLine rr73 = msg("W9XYZ", "K1ABC", "RR73");
    decode(rr73);
    autoseq_on_touch(rr73);
    check(transmit() == "K1ABC KB2SLO EM10", "TX1 to the tail-ended station");
    decode(msg("KB2SLO", "K1ABC", "-10"));
    transmit();
    decode(msg("KB2SLO", "K1ABC", "RR73"));
    transmit();
    check(s_log_count == 1 && s_logged_grid == "FN42", "logged with the grid heard earlier");
}

// A grid sent in the QSO itself beats one heard elsewhere.
static void test_qso_grid_wins() {
    reset();
    decode(msg("CQ", "K1ABC", "FN42"));
    autoseq_call_cq(0);
    autoseq_set_entry_tries(autoseq_cq_entry_id(), 1);
    transmit();
    decode(msg("KB2SLO", "K1ABC", "FN43"));  // answers our CQ from a new square
    transmit();
    decode(msg("KB2SLO", "K1ABC", "R-10"));
    transmit();
    check(s_log_count == 1 && s_logged_grid == "FN43", "the QSO's own grid is logged");
}

// Nothing heard, or heard too long ago: no grid, as before.
static void test_no_heard_grid_logs_none() {
    reset();
    decode(msg("CQ", "K1ABC", "FN42"));
    mock_clock_advance(HEARD_GRID_EXPIRY_MS);
    const UiRxLine rr73 = msg("W9XYZ", "K1ABC", "RR73");
    autoseq_on_touch(rr73);
    transmit();
    decode(msg("KB2SLO", "K1ABC", "-10"));
    transmit();
    decode(msg("KB2SLO", "K1ABC", "RR73"));
    transmit();
    check(s_log_count == 1 && s_logged_grid.empty(), "an expired grid is not logged");
}

int main() {
    test_records_a_cq_grid();
    test_ignores_what_is_not_a_grid();
    test_only_standard_messages();
    test_cq_with_a_modifier();
    test_one_entry_per_call();
    test_full_table_drops_the_call_heard_longest_ago();
    test_expires_four_hours_after_last_heard();
    test_tapped_cq_outlives_its_dropped_entry();
    test_tail_ended_qso_logs_the_heard_grid();
    test_qso_grid_wins();
    test_no_heard_grid_logs_none();
    if (s_failed) return 1;
    printf("PASS: heard grids fill a QSO's empty log grid; one entry per call, 128, 4 h\n");
    return 0;
}
