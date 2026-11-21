/*
 * An entry runs on the channel it was heard on, its dial and its mode (docs/architecture/autoseq.md, The queue;
 * epic u8mc, row bdy4).
 *
 * - A tap on an FT4 decode queues an entry on FT4's channel, and the TX autoseq hands the board says so.
 * - A decode continues the entry for its call on its own channel only: the same station on FT8 and on FT4 is
 *   two QSOs, as POTA and the awards count them, and so is the same station on two FT8 channels.
 * - A tap on one channel leaves a QSO with that station on another alone.
 * - A parked entry is found again only by a decode on its own channel.
 * - The QSO is logged with its entry's channel, its dial and mode.
 * - Call CQ calls on a channel: in its mode's slots, taking the parity of a QSO on that channel only, its tries
 *   counted as any CQ's, one CQ at a time on any channel; a station answering it starts on the CQ's offset only
 *   on the CQ's channel.
 * - Recently worked is asked on the decode's channel: a station just worked on FT8 can start a QSO on FT4.
 * - An entry's QUEUE_ENTRY event carries its channel and its held TX offset (queue_event.h).
 * - A band's plan (station_plan()): FT8 alone and FT8 with FT4 beside it keep FT8's dial, FT4 alone puts the dial
 *   at FT4's on any band, 15 m and 10 m included, and both where they do not fit fall back to FT8 alone.
 * - The station's channels (station_channel.h): the plan's, while the radio reports the plan's dial, none while
 *   the dial was moved by hand or refused, and the reason in the bar's words.
 */
#include <cstdio>
#include <string>

#include "autoseq.h"
#include "host_mocks.h"
#include "queue_event.h"
#include "station_channel.h"

static int s_failed = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        s_failed = 1;
    }
}

// 20 m: FT8 at 14.074 MHz, FT4 beside it at 14.080, and a second FT8 channel in the same window.
static const RxChannel kFt8{14074000u, RxMode::FT8};
static const RxChannel kFt4{14080000u, RxMode::FT4};
static const RxChannel kFt8b{14090000u, RxMode::FT8};

static RxChannel s_logged{};
static int s_log_count = 0;
static bool adif_cb(const std::string&, const std::string&, int, int, RxChannel channel) {
    s_logged = channel;
    ++s_log_count;
    return true;
}

static UiRxLine line(const char* f1, const char* dx, const char* f3, RxChannel channel, int slot) {
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
    m.channel = channel;
    return m;
}

static void decode(const UiRxLine& m) { autoseq_on_decodes(std::vector<UiRxLine>{m}); }

static void reset() {
    mock_clock_set(1000);
    autoseq_init();
    autoseq_set_station("KB2SLO", "EM10");
    autoseq_set_tries(3);
    autoseq_set_adif_callback(adif_cb);
    autoseq_set_recent_qso_callback(nullptr);
    s_log_count = 0;
}

static int count(const char* dx, RxChannel channel, QsoContext* out = nullptr) {
    int n = 0;
    QsoContext c;
    for (int i = 0; i < autoseq_active_count(); ++i) {
        if (autoseq_get_active_context(i, &c) && c.dxcall == dx && c.channel == channel) {
            ++n;
            if (out) *out = c;
        }
    }
    return n;
}

static std::string transmit(RxChannel* channel = nullptr, int offset_hz = 0) {
    AutoseqTxEntry pending;
    if (!autoseq_fetch_pending_tx(pending)) return "";
    if (channel) *channel = pending.channel;
    autoseq_on_tx_starting(offset_hz);
    autoseq_mark_sent(0);
    autoseq_tick(0, 0, 0);
    return pending.text;
}

static void test_touch_ft4_queues_ft4() {
    reset();
    check(autoseq_on_touch(line("CQ", "K1ABC", "FN42", kFt4, 0)) == AutoseqTouchResult::Queued, "FT4 tap queued");
    check(count("K1ABC", kFt4) == 1 && count("K1ABC", kFt8) == 0, "the entry is on FT4's channel");
    RxChannel c;
    check(transmit(&c) == "K1ABC KB2SLO EM10" && c == kFt4, "its TX goes out on FT4's channel");
}

static void test_decode_continues_own_channel_only() {
    reset();
    autoseq_on_touch(line("CQ", "K1ABC", "FN42", kFt4, 0));
    autoseq_on_touch(line("CQ", "K1ABC", "FN42", kFt8, 0));
    autoseq_on_touch(line("CQ", "K1ABC", "FN42", kFt8b, 0));
    check(count("K1ABC", kFt4) == 1 && count("K1ABC", kFt8) == 1 && count("K1ABC", kFt8b) == 1,
          "one station, three channels, three entries");
    decode(line("KB2SLO", "K1ABC", "-10", kFt8, 1));
    QsoContext ft8, ft4, ft8b;
    count("K1ABC", kFt8, &ft8);
    count("K1ABC", kFt4, &ft4);
    count("K1ABC", kFt8b, &ft8b);
    check(ft8.state == AutoseqState::ROGER_REPORT, "the report on 14.074 advances that QSO");
    check(ft4.state == AutoseqState::REPLYING, "and leaves the FT4 QSO where it was");
    check(ft8b.state == AutoseqState::REPLYING, "and the FT8 QSO on 14.090 too");
}

static void test_touch_leaves_other_channel() {
    reset();
    decode(line("KB2SLO", "K1ABC", "FN42", kFt8, 1));
    check(count("K1ABC", kFt8) == 1, "K1ABC calls us on FT8");
    autoseq_on_touch(line("CQ", "K1ABC", "FN42", kFt4, 0));
    check(count("K1ABC", kFt8) == 1 && count("K1ABC", kFt4) == 1, "a tap on FT4 keeps the FT8 QSO");
}

static void test_parked_found_on_own_channel() {
    reset();
    autoseq_on_touch(line("CQ", "K1ABC", "FN42", kFt4, 0));
    decode(line("KB2SLO", "K1ABC", "-10", kFt4, 1));  // REPLYING -> ROGER_REPORT on FT4
    QsoContext c;
    count("K1ABC", kFt4, &c);
    check(autoseq_drop_by_entry_id(c.entry_id), "the FT4 QSO is parked");
    decode(line("KB2SLO", "K1ABC", "R-12", kFt8, 1));
    check(count("K1ABC", kFt8) == 0 && count("K1ABC", kFt4) == 0,
          "an FT8 R-report neither revives the FT4 QSO nor starts one");
    decode(line("KB2SLO", "K1ABC", "R-12", kFt4, 1));
    check(count("K1ABC", kFt4, &c) == 1 && c.state == AutoseqState::ROGER_REPORT, "the FT4 R-report revives it");
}

static void test_logged_with_channel() {
    reset();
    autoseq_on_touch(line("CQ", "K1ABC", "FN42", kFt4, 0));
    decode(line("KB2SLO", "K1ABC", "-10", kFt4, 1));
    decode(line("KB2SLO", "K1ABC", "RR73", kFt4, 1));
    transmit();  // our 73 logs it
    check(s_log_count == 1 && s_logged == kFt4, "an FT4 QSO is logged on FT4's channel, its dial and mode");
}

static void test_cq_parity_from_its_channel_only() {
    reset();
    autoseq_on_touch(line("CQ", "K1ABC", "FN42", kFt4, 0));  // FT4, TX parity 1
    check(autoseq_cq_parity(kFt8, 0, true) == 0, "an FT4 QSO does not set an FT8 CQ's parity");
    check(autoseq_cq_parity(kFt4, 0, true) == 1, "it sets an FT4 CQ's");
    autoseq_on_touch(line("CQ", "K2DEF", "FN42", kFt8b, 0));  // FT8 on 14.090, TX parity 1
    check(autoseq_cq_parity(kFt8, 0, true) == 0, "nor does an FT8 QSO on another channel");
    autoseq_on_touch(line("CQ", "K3GHI", "FN42", kFt8, 0));  // FT8 on 14.074, TX parity 1
    check(autoseq_cq_parity(kFt8, 0, true) == 1, "an FT8 QSO on the CQ's channel does");
}

static void test_ft4_cq() {
    reset();
    RxChannel c;
    check(!autoseq_cq_channel(&c), "no CQ, no CQ channel");
    check(autoseq_call_cq(kFt4, 1), "Call CQ on FT4's channel");
    check(autoseq_cq_channel(&c) && c == kFt4, "the CQ is on FT4's channel");
    check(!autoseq_call_cq(kFt8, 0), "a second CQ is refused, on another channel too");
    check(autoseq_cq_channel(&c) && c == kFt4, "and the first is left as it is");
    QsoContext cq;
    for (int i = 0; i < autoseq_active_count(); ++i) {
        if (autoseq_get_active_context(i, &cq) && cq.state == AutoseqState::CALLING) break;
    }
    check(cq.slot_id == 1 && cq.try_limit == 3, "on its parity, with the station default's tries");
    check(transmit(&c, 1700) == "CQ KB2SLO EM10" && c == kFt4, "it goes out on FT4's channel");
    check(transmit(&c, 1800) == "CQ KB2SLO EM10" && c == kFt4, "again");
    check(transmit(&c, 1900) == "CQ KB2SLO EM10" && c == kFt4, "and a third time");
    check(!autoseq_cq_channel(nullptr) && autoseq_active_count() == 0, "its tries used, it ends, as an FT8 CQ does");
}

static void test_cq_offset_on_its_channel() {
    reset();
    autoseq_call_cq(kFt4, 1);
    transmit(nullptr, 1700);  // the CQ goes out on 1700 Hz
    const int64_t now = 1000;
    check(autoseq_remembered_tx_offset("K1ABC", kFt4, true, now) == 1700,
          "a station calling us on the CQ's channel starts where it heard the CQ");
    check(autoseq_remembered_tx_offset("K1ABC", kFt8, true, now) == 0, "one calling on another channel does not");
    check(autoseq_remembered_tx_offset("K1ABC", kFt4, false, now) == 0, "nor one we call ourselves");
}

static void test_ft8_cq_unchanged() {
    reset();
    check(autoseq_call_cq(kFt8, 0), "Call CQ on FT8's channel");
    RxChannel c;
    check(transmit(&c, 1500) == "CQ KB2SLO EM10" && c == kFt8, "it goes out on FT8's channel");
    decode(line("KB2SLO", "K1ABC", "FN42", kFt8, 1));
    QsoContext q;
    check(count("K1ABC", kFt8, &q) == 1 && q.state == AutoseqState::REPORT && q.tx_offset_hz == 1500,
          "an answer starts an FT8 QSO on the CQ's offset");
}

static bool recent_ft8_only(const std::string&, RxChannel channel) { return channel.mode == RxMode::FT8; }

static void test_recent_per_channel() {
    reset();
    autoseq_set_recent_qso_callback(recent_ft8_only);
    decode(line("KB2SLO", "K1ABC", "FN42", kFt8, 1));
    check(count("K1ABC", kFt8) == 0, "worked on FT8 just now: no new FT8 QSO");
    decode(line("KB2SLO", "K1ABC", "FN42", kFt4, 1));
    check(count("K1ABC", kFt4) == 1, "but a new FT4 one");
}

static void test_queue_event_carries_channel() {
    reset();
    autoseq_on_touch(line("CQ", "K1ABC", "FN42", kFt4, 0));
    autoseq_on_touch(line("CQ", "K2DEF", "FN42", kFt8, 0));
    QsoContext ft4, ft8;
    check(count("K1ABC", kFt4, &ft4) == 1 && count("K2DEF", kFt8, &ft8) == 1, "both queued");
    porta_queue_entry_event_t q = queue_entry_event(ft4);
    check(q.mode == PORTA_QUEUE_MODE_FT4 && q.dial_hz == 14080000u, "an FT4 entry's event carries FT4 and its dial");
    check(q.offset_hz == 0, "no offset before its first transmission");
    q = queue_entry_event(ft8);
    check(q.mode == PORTA_QUEUE_MODE_FT8 && q.dial_hz == 14074000u, "an FT8 entry's event carries FT8 and its dial");
    check(std::string(q.dxcall) == "K2DEF" && q.entry_id == ft8.entry_id, "and its call and id");

    check(autoseq_hold_tx_offset(ft4.entry_id, 1916), "the FT4 QSO holds an offset");
    count("K1ABC", kFt4, &ft4);
    check(queue_entry_event(ft4).offset_hz == 1916, "its event carries the held offset");

    q = queue_entry_gone_event(ft4.entry_id);
    check(q.state == PORTA_QUEUE_STATE_IDLE && q.mode == 0 && q.dial_hz == 0 && q.offset_hz == 0 && !q.dxcall[0],
          "a gone entry's event is IDLE and nothing else");
}

static StationDials dials_on(uint32_t dial, int listen, int32_t ft4_offset) {
    StationDials d;
    d.plan.radio_dial_hz = dial;
    d.plan.listen = listen;
    d.plan.ft4_offset_hz = ft4_offset;
    d.reported_dial_hz = dial;
    return d;
}

static void test_station_plan() {
    // 20 m: FT4's dial 6 kHz above FT8's, both in a QMX's window.
    StationPlan p = station_plan(14074000u, 6000, kListenFt8, true);
    check(p.radio_dial_hz == 14074000u && p.listen == kListenFt8 && p.ft4_offset_hz == 0, "FT8 alone: FT8's dial");
    p = station_plan(14074000u, 6000, kListenBoth, true);
    check(p.radio_dial_hz == 14074000u && p.listen == kListenBoth && p.ft4_offset_hz == 6000,
          "both where they fit: FT8's dial, FT4 6 kHz beside it");
    p = station_plan(14074000u, 6000, kListenFt4, true);
    check(p.radio_dial_hz == 14080000u && p.listen == kListenFt4 && p.ft4_offset_hz == 0,
          "FT4 alone: the dial at FT4's, no offset");
    // 15 m and 10 m: 66 and 106 kHz apart, outside a QMX's window; FT4 alone still works there.
    p = station_plan(21074000u, 66000, kListenFt4, false);
    check(p.radio_dial_hz == 21140000u && p.listen == kListenFt4, "FT4 alone on 15 m: the dial at 21.140");
    p = station_plan(28074000u, 106000, kListenFt4, false);
    check(p.radio_dial_hz == 28180000u && p.listen == kListenFt4, "FT4 alone on 10 m: the dial at 28.180");
    p = station_plan(21074000u, 66000, kListenBoth, false);
    check(p.radio_dial_hz == 21074000u && p.listen == kListenFt8 && p.ft4_offset_hz == 0,
          "both where they do not fit falls back to FT8 alone");
    // 40 m: FT4 26.5 kHz below FT8.
    p = station_plan(7074000u, -26500, kListenFt4, true);
    check(p.radio_dial_hz == 7047500u, "FT4 alone on 40 m: the dial at 7.0475");
    p = station_plan(0, 6000, kListenFt4, true);
    check(p.radio_dial_hz == 0, "no dial set: none");
}

static void test_station_channels() {
    StationDials d = dials_on(14074000u, kListenFt8, 0);
    RxChannel c;
    check(station_channel(d, RxMode::FT8, &c) && c == kFt8, "FT8's channel is the dial set");
    check(!station_channel(d, RxMode::FT4, &c), "FT4's is not listened to while FT8 is alone");
    d = dials_on(14074000u, kListenBoth, 6000);
    check(station_channel(d, RxMode::FT4, &c) && c == kFt4, "beside FT8, FT4's is FT8's dial plus its offset");
    check(station_listens(d, kFt8) && station_listens(d, kFt4), "both listened to");
    check(!station_listens(d, kFt8b), "a channel the station does not run is not");
    check(!station_listens(d, RxChannel{14080000u, RxMode::FT8}), "nor FT8 on FT4's dial");
    d = dials_on(14080000u, kListenFt4, 0);
    check(station_channel(d, RxMode::FT4, &c) && c == kFt4, "FT4 alone: its channel is the radio's dial");
    check(!station_listens(d, kFt8), "and FT8's is not listened to");
    d = dials_on(21140000u, kListenFt4, 0);
    check(station_listens(d, RxChannel{21140000u, RxMode::FT4}), "FT4 alone on 15 m is a channel like any other");
    char buf[40];
    d = dials_on(14074000u, kListenBoth, 6000);
    check(station_off_dial_refusal(d, buf, sizeof buf) == nullptr, "on its dial, nothing refuses");

    d.reported_dial_hz = 14074500u;  // the knob turned
    check(!station_on_dial(d) && !station_listens(d, kFt8) && !station_listens(d, kFt4),
          "a dial moved by hand belongs to no channel");
    const char* why = station_off_dial_refusal(d, buf, sizeof buf);
    check(why && std::string(why) == "Radio not on 14.074 MHz", "and says why in the bar's words");
    d = dials_on(14080000u, kListenFt4, 0);
    d.reported_dial_hz = 14074000u;  // a radio that did not move to FT4's dial
    why = station_off_dial_refusal(d, buf, sizeof buf);
    check(why && std::string(why) == "Radio not on 14.08 MHz", "FT4 alone names FT4's dial");
    d.reported_dial_hz = 0;
    check(!station_on_dial(d), "a dial not yet reported is not the station's");

    d = dials_on(0, kListenFt8, 0);
    check(!station_on_dial(d) && station_off_dial_refusal(d, buf, sizeof buf) == nullptr,
          "with no dial set there is no channel, and no dial to name");

    // The dial in MHz with its trailing zeros dropped.
    const struct {
        uint32_t hz;
        const char* text;
    } dials[] = {{7047500u, "Radio not on 7.0475 MHz"}, {50313000u, "Radio not on 50.313 MHz"},
                 {3573000u, "Radio not on 3.573 MHz"}, {14000000u, "Radio not on 14 MHz"},
                 {10136000u, "Radio not on 10.136 MHz"}};
    for (const auto& t : dials) {
        StationDials e = dials_on(t.hz, kListenFt8, 0);
        e.reported_dial_hz = t.hz + 1;
        why = station_off_dial_refusal(e, buf, sizeof buf);
        check(why && std::string(why) == t.text, t.text);
    }
    check(queue_mode_dial_hz(RxMode::FT4, 0, 6000) == 0, "an unknown dial stays unknown in FT4");
    check(queue_mode_dial_hz(RxMode::FT4, 7074000u, -26500) == 7047500u, "40 m's FT4 dial is below FT8's");
}

int main() {
    test_station_plan();
    test_station_channels();
    test_queue_event_carries_channel();
    test_touch_ft4_queues_ft4();
    test_decode_continues_own_channel_only();
    test_touch_leaves_other_channel();
    test_parked_found_on_own_channel();
    test_logged_with_channel();
    test_cq_parity_from_its_channel_only();
    test_ft4_cq();
    test_cq_offset_on_its_channel();
    test_ft8_cq_unchanged();
    test_recent_per_channel();
    if (s_failed) return 1;
    printf("channel entries: OK\n");
    return 0;
}
