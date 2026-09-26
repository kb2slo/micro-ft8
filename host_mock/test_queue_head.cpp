/*
 * The tick after a TX advances the entry that was transmitted, and a parked
 * entry comes back able to transmit whatever DX sent.
 *
 * - Dropping the on-air entry (Port A QUEUE_CANCEL, the web app's row x) used
 *   to leave the tick to advance whatever was queue[0] next: a QSO that never
 *   transmitted lost a retry.
 * - autoseq_head_entry_id()/AutoseqTxEntry::entry_id let a board check that
 *   its armed TX is still autoseq's head before keying, and
 *   autoseq_transmitting_entry_id() tells it that a cancelled entry is the one
 *   on the air, so the cancel stops the transmission too.
 * - A parked entry reactivated by a message autoseq cannot classify (no third
 *   field, say) came back with next_tx = TX_NONE: empty text at queue[0], and
 *   nothing transmitted, the beacon included, until it was removed.
 */
#include <cstdio>
#include <string>

#include "autoseq.h"
#include "host_mocks.h"

static int s_failed = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        s_failed = 1;
    }
}

static bool adif_cb(const std::string&, const std::string&, int, int) { return true; }

static UiRxLine to_me(const char* dx, const char* f3) {
    UiRxLine m;
    m.field1 = "KB2SLO";
    m.field2 = dx;
    m.field3 = f3;
    m.text = std::string("KB2SLO ") + dx + (f3[0] ? " " : "") + f3;
    m.snr = -8;
    m.offset_hz = 1200;
    m.slot_id = 1;
    m.is_to_me = true;
    return m;
}

static void decode(const UiRxLine& m) { autoseq_on_decodes(std::vector<UiRxLine>{m}); }

static std::string transmit() {
    AutoseqTxEntry pending;
    if (!autoseq_fetch_pending_tx(pending)) return "";
    autoseq_on_tx_starting();
    autoseq_mark_sent(0);
    autoseq_tick(0, 0, 0);
    return pending.text;
}

static void reset() {
    mock_clock_set(1000);
    autoseq_init();
    autoseq_set_station("KB2SLO", "FN30");
    autoseq_set_max_retry(5);
    autoseq_set_adif_callback(adif_cb);
}

static bool find(const char* dx, QsoContext* out) {
    for (int i = 0; i < autoseq_active_count(); ++i) {
        if (autoseq_get_active_context(i, out) && out->dxcall == dx) return true;
    }
    return false;
}

static void test_head_id_matches_fetch() {
    reset();
    check(autoseq_head_entry_id() == 0, "empty queue has no head id");
    decode(to_me("K1ABC", "FN42"));
    AutoseqTxEntry pending;
    check(autoseq_fetch_pending_tx(pending), "K1ABC queued");
    QsoContext head;
    autoseq_get_active_context(0, &head);
    check(pending.entry_id != 0 && pending.entry_id == head.entry_id &&
              autoseq_head_entry_id() == head.entry_id,
          "fetch and head id name queue[0]");
}

// Drop the entry on the air; the tick after that TX must not touch the next one.
static void test_tick_skips_dropped_on_air_entry() {
    reset();
    decode(to_me("K1ABC", "FN42"));
    decode(to_me("N0CALL", "EM12"));
    AutoseqTxEntry pending;
    autoseq_fetch_pending_tx(pending);
    autoseq_on_tx_starting();                    // pending goes on the air
    check(autoseq_drop_by_entry_id(pending.entry_id), "drop the on-air entry");
    const char* other = (pending.dxcall == "K1ABC") ? "N0CALL" : "K1ABC";
    QsoContext before;
    check(find(other, &before), "the other QSO is now the head");
    autoseq_mark_sent(0);
    autoseq_tick(0, 0, 0);
    QsoContext after;
    check(find(other, &after) && after.retry_counter == before.retry_counter &&
              after.state == before.state,
          "the tick left the QSO that never transmitted alone");
}

// A retry still counts on the entry that did transmit, even if it moved.
static void test_tick_follows_moved_entry() {
    reset();
    decode(to_me("K1ABC", "FN42"));              // REPORT
    AutoseqTxEntry pending;
    autoseq_fetch_pending_tx(pending);
    autoseq_on_tx_starting();
    decode(to_me("N0CALL", "-10"));              // ROGER_REPORT sorts above K1ABC
    QsoContext head;
    autoseq_get_active_context(0, &head);
    check(head.dxcall == "N0CALL", "N0CALL moved ahead mid-TX");
    autoseq_mark_sent(0);
    autoseq_tick(0, 0, 0);
    QsoContext k, n;
    check(find("K1ABC", &k) && k.retry_counter == 1, "K1ABC's retry counted");
    check(find("N0CALL", &n) && n.retry_counter == 0, "N0CALL untouched");
}

// Parked SIGNOFF, reactivated by a message with no third field.
static void test_reactivation_by_unclassified_message_can_transmit() {
    reset();
    UiRxLine cq;
    cq.field1 = "CQ";
    cq.field2 = "W1AW";
    cq.field3 = "FN31";
    cq.snr = -5;
    cq.slot_id = 1;
    cq.is_cq = true;
    autoseq_on_touch(cq);
    transmit();
    decode(to_me("W1AW", "-12"));
    transmit();
    decode(to_me("W1AW", "RR73"));
    check(transmit() == "W1AW KB2SLO 73", "73 parks W1AW");

    decode(to_me("W1AW", ""));
    check(transmit() == "W1AW KB2SLO 73", "reactivated entry sends its 73");
    autoseq_start_cq(0);
    check(transmit() == "CQ KB2SLO FN30", "and the beacon is not blocked");
}

// Parked REPORT (retries exhausted), reactivated the same way.
static void test_reactivated_report_resends_report() {
    reset();
    decode(to_me("K1ABC", "FN42"));
    for (int i = 0; i < 6; ++i) transmit();      // first TX + 5 retries, then parked
    check(autoseq_active_count() == 0, "K1ABC parked after its retries");
    decode(to_me("K1ABC", ""));
    check(transmit() == "K1ABC KB2SLO -8", "reactivated REPORT resends the report");
}

// The board stops the TX when the cancelled entry is the one on the air.
static void test_transmitting_id_names_the_on_air_entry() {
    reset();
    check(autoseq_transmitting_entry_id() == 0, "nothing on the air yet");
    decode(to_me("K1ABC", "FN42"));
    decode(to_me("N0CALL", "EM12"));
    AutoseqTxEntry pending;
    autoseq_fetch_pending_tx(pending);
    autoseq_on_tx_starting();
    check(autoseq_transmitting_entry_id() == pending.entry_id, "names the entry that started");
    QsoContext other;
    autoseq_get_active_context(1, &other);
    check(other.entry_id != autoseq_transmitting_entry_id(), "a queued entry is not on the air");
    autoseq_mark_sent(0);
    autoseq_tick(0, 0, 0);
    check(autoseq_transmitting_entry_id() == 0, "cleared by the tick after the TX");
}

int main() {
    test_head_id_matches_fetch();
    test_transmitting_id_names_the_on_air_entry();
    test_tick_skips_dropped_on_air_entry();
    test_tick_follows_moved_entry();
    test_reactivation_by_unclassified_message_can_transmit();
    test_reactivated_report_resends_report();
    if (s_failed) return 1;
    printf("PASS: tick follows the transmitted entry; reactivation can transmit\n");
    return 0;
}
