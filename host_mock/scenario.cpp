#include "scenario.h"

#include "autoseq.h"
#include "host_mocks.h"
#include "porta_proto.h"

#include <cstdio>
#include <cstring>
#include <ctime>

static Scenario* s_current = nullptr;

static const uint32_t kEpochBase = 1700000000u;
static const uint32_t kDialHz = 14074000u;

static void push_raw(Scenario& s, const char* row)
{
    if (row && row[0]) {
        s.events.emplace_back(row);
        s.next_seq++;
    }
}

static void stamp(Scenario& s)
{
    if (s.wall_slots) {
        const uint32_t now = (uint32_t)time(nullptr);
        s.epoch_secs = now;
        s.slot_idx = now / 15u;
    }
}

static void emit_log_row(Scenario& s, const char* text)
{
    char row[1024];
    const size_t n = porta_event_json_log(row, sizeof(row), s.next_seq, s.epoch_secs, text);
    if (n > 0) {
        push_raw(s, row);
    }
}

static void emit_hud_row(Scenario& s, bool active)
{
    porta_tx_hud_event_t hud;
    memset(&hud, 0, sizeof(hud));
    hud.epoch_secs = s.epoch_secs;
    hud.active = active;
    hud.power_dw = -1;
    hud.swr_c = -1;
    hud.battery_pct = -1;
    hud.offset_hz = s.hud_hz;
    snprintf(hud.text, sizeof(hud.text), "%s", s.hud_text.c_str());
    char row[1024];
    const size_t n = porta_event_json_tx_hud(row, sizeof(row), s.next_seq, &hud);
    if (n > 0) {
        push_raw(s, row);
    }
}

static void emit_decode(Scenario& s, const UiRxLine& rx)
{
    porta_decode_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.epoch_secs = s.epoch_secs;
    ev.decode_id = s.next_decode_id++;
    snprintf(ev.text, sizeof(ev.text), "%s", rx.text.c_str());
    ev.snr = (int8_t)(rx.snr < -128 ? -128 : (rx.snr > 127 ? 127 : rx.snr));
    ev.offset_hz = (uint16_t)(rx.offset_hz < 0 ? 0 : (rx.offset_hz > 65535 ? 65535 : rx.offset_hz));
    ev.is_cq = rx.is_cq;
    ev.is_to_me = rx.is_to_me;
    ev.is_recent_qso = rx.is_recent_qso;
    ev.slot_idx = s.slot_idx;
    ev.slot_len_ds = 150;
    ev.dial_hz = s.dial_hz ? s.dial_hz : kDialHz;
    char row[1024];
    const size_t n = porta_event_json_decode(row, sizeof(row), s.next_seq, &ev);
    if (n > 0) {
        push_raw(s, row);
    }
    s.decodes[ev.decode_id] = rx;
}

static std::string time_on_for(int period)
{
    const int secs = period * 15;
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", (secs / 3600) % 24, (secs % 3600) / 60);
    return buf;
}

static bool on_adif(const std::string& dxcall, const std::string& dxgrid,
                    int rst_sent, int rst_rcvd)
{
    if (!s_current) {
        return true;
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "%s %s rst_sent=%+d rst_rcvd=%+d",
             dxcall.c_str(), dxgrid.c_str(), rst_sent, rst_rcvd);
    printf(">>> ADIF log: %s\n", buf);
    if (!s_current->period_log.empty()) {
        s_current->period_log += " | ";
    }
    s_current->period_log += buf;

    SimLogEntry row;
    row.time_on = time_on_for(s_current->period);
    row.call = dxcall;
    row.grid = dxgrid;
    row.rst_sent = rst_sent;
    row.rst_rcvd = rst_rcvd;
    row.station_call = s_current->station_call;
    row.my_grid = s_current->station_grid;
    s_current->log.push_back(row);
    emit_log_row(*s_current, buf);
    return true;
}

static bool on_cabrillo(const std::string& dxcall, const std::string& exchange)
{
    if (!s_current) {
        return true;
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "%s exchange=%s", dxcall.c_str(), exchange.c_str());
    printf(">>> Cabrillo FD log: %s\n", buf);
    if (!s_current->period_cabrillo.empty()) {
        s_current->period_cabrillo += " | ";
    }
    s_current->period_cabrillo += buf;
    return true;
}

static void apply_period_slot(UiRxLine& rx, int slot_parity)
{
    if (rx.slot_id == 0 && slot_parity != 0) {
        rx.slot_id = slot_parity;
    }
}

static void fail_period(Scenario& s, const char* what, const std::string& expect,
                        const std::string& got)
{
    fprintf(stderr, "period %d %s: expected [%s] got [%s]\n",
            s.period, what, expect.c_str(), got.c_str());
    s.failures++;
}

static const char* beacon_name(int mode)
{
    switch (mode) {
        case 0: return "OFF";
        case 1: return "EVEN";
        case 2: return "ODD";
        default: return "?";
    }
}

void scenario_begin(Scenario& s, const TestData& data)
{
    s = Scenario();
    s.data = data;
    s.my_tx_parity = data.config.tx_on_even ? 0 : 1;
    s.station_call = data.config.my_callsign;
    s.station_grid = data.config.my_grid;
    s.dial_hz = kDialHz;
    s_current = &s;

    autoseq_init();
    autoseq_set_station(s.station_call, s.station_grid);
    autoseq_set_adif_callback(on_adif);
    autoseq_set_cabrillo_fd_callback(on_cabrillo);
    autoseq_set_max_retry(data.config.max_retry);
    autoseq_set_skip_tx1(data.config.skip_tx1);

    if (!data.config.cq_type.empty()) {
        AutoseqCqType cq = AutoseqCqType::CQ;
        if (data.config.cq_type == "FD") cq = AutoseqCqType::FD;
        else if (data.config.cq_type == "SOTA") cq = AutoseqCqType::SOTA;
        else if (data.config.cq_type == "POTA") cq = AutoseqCqType::POTA;
        else if (data.config.cq_type == "QRP") cq = AutoseqCqType::QRP;
        else if (data.config.cq_type == "FREETEXT") cq = AutoseqCqType::FREETEXT;
        autoseq_set_cq_type(cq, data.config.free_text);
    }

    printf("Station: %s / %s\n", s.station_call.c_str(), s.station_grid.c_str());
    printf("TX_ON_EVEN: %s\n", data.config.tx_on_even ? "true" : "false");
    if (!data.config.cq_type.empty()) {
        printf("CQ_TYPE: %s  FREE_TEXT: %s\n",
               data.config.cq_type.c_str(), data.config.free_text.c_str());
    }
    printf("Periods: %d\n\n", (int)data.periods.size());
}

void scenario_emit_status(Scenario& s)
{
    stamp(s);
    porta_slot_state_event_t slot;
    memset(&slot, 0, sizeof(slot));
    slot.epoch_secs = s.epoch_secs;
    slot.slot_parity = s.last_parity;
    slot.beacon_mode = (uint8_t)s.beacon;
    slot.resolved_offset_hz = s.last_offset;
    char row[1024];
    size_t n = porta_event_json_slot(row, sizeof(row), s.next_seq, &slot);
    if (n > 0) {
        push_raw(s, row);
    }

    std::vector<QsoContext> ctxs;
    autoseq_get_active_contexts(ctxs);
    std::vector<uint16_t> ids;
    for (const QsoContext& ctx : ctxs) {
        if (ctx.state == AutoseqState::IDLE || ctx.entry_id == 0) {
            continue;
        }
        ids.push_back(ctx.entry_id);
        porta_queue_entry_event_t q;
        memset(&q, 0, sizeof(q));
        q.epoch_secs = s.epoch_secs;
        q.entry_id = ctx.entry_id;
        q.state = (uint8_t)ctx.state;
        q.retry_count = (uint8_t)ctx.retry_counter;
        q.retry_limit = (uint8_t)ctx.retry_limit;
        snprintf(q.dxcall, sizeof(q.dxcall), "%s", ctx.dxcall.c_str());
        n = porta_event_json_queue(row, sizeof(row), s.next_seq, &q);
        if (n > 0) {
            push_raw(s, row);
        }
    }
    for (uint16_t id : s.last_ids) {
        bool still = false;
        for (uint16_t cur : ids) {
            if (cur == id) {
                still = true;
                break;
            }
        }
        if (still) {
            continue;
        }
        porta_queue_entry_event_t q;
        memset(&q, 0, sizeof(q));
        q.epoch_secs = s.epoch_secs;
        q.entry_id = id;
        q.state = (uint8_t)AutoseqState::IDLE;
        n = porta_event_json_queue(row, sizeof(row), s.next_seq, &q);
        if (n > 0) {
            push_raw(s, row);
        }
    }
    s.last_ids = ids;
}

void scenario_emit_hud(Scenario& s)
{
    if (!s.hud_active) {
        return;
    }
    stamp(s);
    emit_hud_row(s, true);
}

void scenario_clear_hud(Scenario& s)
{
    if (!s.hud_active) {
        return;
    }
    stamp(s);
    emit_hud_row(s, false);
    s.hud_active = false;
}

void scenario_emit_usb(Scenario& s, bool present)
{
    stamp(s);
    if (!s.wall_slots && s.epoch_secs == 0) {
        s.epoch_secs = kEpochBase;
    }
    porta_usb_status_event_t usb;
    memset(&usb, 0, sizeof(usb));
    usb.epoch_secs = s.epoch_secs;
    usb.present = present;
    usb.kind = 0;
    usb.vid = 0x0483;
    usb.pid = 0xA34C;
    char row[256];
    const size_t n = porta_event_json_usb(row, sizeof(row), s.next_seq, &usb);
    if (n > 0) {
        push_raw(s, row);
    }
}

void scenario_set_beacon(Scenario& s, int mode)
{
    s_current = &s;
    s.beacon = mode;
    if (mode == 0) {
        autoseq_cancel_cq();
    }
    stamp(s);
    scenario_emit_status(s);
}

void scenario_set_station(Scenario& s, const std::string& call, const std::string& grid)
{
    s_current = &s;
    s.station_call = call;
    s.station_grid = grid;
    autoseq_set_station(call, grid);
}

void scenario_set_cq_type(Scenario& s, int type, const std::string& free_text)
{
    s_current = &s;
    AutoseqCqType cq = AutoseqCqType::CQ;
    switch (type) {
        case 1: cq = AutoseqCqType::SOTA; break;
        case 2: cq = AutoseqCqType::POTA; break;
        case 3: cq = AutoseqCqType::QRP; break;
        case 4: cq = AutoseqCqType::FD; break;
        case 5: cq = AutoseqCqType::FREETEXT; break;
        default: cq = AutoseqCqType::CQ; break;
    }
    autoseq_set_cq_type(cq, free_text);
}

void scenario_set_max_retry(Scenario& s, int retry)
{
    s_current = &s;
    autoseq_set_max_retry(retry);
}

bool scenario_reply(Scenario& s, uint32_t decode_id)
{
    s_current = &s;
    auto it = s.decodes.find(decode_id);
    if (it == s.decodes.end()) {
        return false;
    }
    autoseq_on_touch(it->second);
    stamp(s);
    scenario_emit_status(s);
    return true;
}

bool scenario_cancel_entry(Scenario& s, uint16_t entry_id)
{
    s_current = &s;
    const bool ok = autoseq_drop_by_entry_id(entry_id);
    stamp(s);
    scenario_emit_status(s);
    return ok;
}

void scenario_cancel_tx(Scenario& s)
{
    s_current = &s;
    autoseq_cancel_cq();
    stamp(s);
    if (s.hud_active) {
        emit_hud_row(s, false);
        s.hud_active = false;
    }
    scenario_emit_status(s);
}

bool scenario_step(Scenario& s)
{
    if (s.period >= (int)s.data.periods.size()) {
        return false;
    }
    s_current = &s;
    s.period_log.clear();
    s.period_cabrillo.clear();

    if (s.wall_slots) {
        stamp(s);
    } else {
        s.epoch_secs = kEpochBase + (uint32_t)s.period * 15u;
        s.slot_idx = (uint32_t)s.period + 1u;
    }

    if (s.hud_active) {
        emit_hud_row(s, false);
        s.hud_active = false;
    }

    const int p = s.period;
    const int slot_parity = p & 1;
    const int64_t period_base_ms = (int64_t)p * 15000;
    mock_clock_set(period_base_ms);
    printf("\n========== Period %d ==========\n", p);

    TestPeriod& tp = s.data.periods[p];
    if (tp.has_beacon_change) {
        const int old_beacon = s.beacon;
        s.beacon = tp.beacon_change.beacon_on;
        if (s.beacon == 0) {
            autoseq_cancel_cq();
        }
        printf("[Period %d @ %.1fs] Beacon: %s -> %s\n",
               p, tp.beacon_change.time_offset,
               beacon_name(old_beacon), beacon_name(s.beacon));
    }

    std::vector<UiRxLine> to_me;
    for (const UiRxLine& src : tp.messages) {
        UiRxLine rx = src;
        apply_period_slot(rx, slot_parity);
        printf("[Period %d] RX: %s %s %s  snr=%d slot=%d\n",
               p, rx.field1.c_str(), rx.field2.c_str(), rx.field3.c_str(),
               rx.snr, rx.slot_id);
        emit_decode(s, rx);
        if (rx.is_to_me) {
            to_me.push_back(rx);
        }
    }
    if (!to_me.empty()) {
        autoseq_on_decodes(to_me);
    }

    if (tp.has_touch_event) {
        const int idx = tp.touch_event.message_index;
        if (idx >= 0 && idx < (int)tp.messages.size()) {
            UiRxLine rx = tp.messages[idx];
            apply_period_slot(rx, slot_parity);
            printf("[Period %d @ %.1fs] Touch: message %d (%s)\n",
                   p, tp.touch_event.time_offset, idx, rx.text.c_str());
            autoseq_on_touch(rx);
        }
    }

    if (tp.has_freetext_event) {
        const int fallback_parity = (int)(((int64_t)p + 1) & 1);
        const bool ok = autoseq_schedule_freetext(tp.freetext_event.text, fallback_parity);
        printf("[Period %d @ %.1fs] FreeText: %s (scheduled=%d)\n",
               p, tp.freetext_event.time_offset,
               tp.freetext_event.text.c_str(), ok ? 1 : 0);
    }

    AutoseqTxEntry pending;
    bool has_tx = autoseq_fetch_pending_tx(pending);
    if (!has_tx && s.beacon != 0 && slot_parity == s.my_tx_parity) {
        autoseq_start_cq(s.my_tx_parity);
        has_tx = autoseq_fetch_pending_tx(pending);
    }

    std::string actual_tx;
    if (has_tx) {
        actual_tx = pending.text;
        printf("[Period %d] TX: %s  (slot=%d offset=%dHz)\n",
               p, pending.text.c_str(), pending.slot_id, pending.offset_hz);
        autoseq_on_tx_starting();
        autoseq_mark_sent(p);
        const int ms_to_boundary = 15000 - (int)(mock_clock_get() - period_base_ms);
        autoseq_tick(p, slot_parity, ms_to_boundary);
        s.hud_text = pending.text;
        s.hud_hz = (uint16_t)(pending.offset_hz < 0 ? 0 : pending.offset_hz);
        s.hud_active = true;
        s.last_offset = s.hud_hz;
        emit_hud_row(s, true);
    } else {
        printf("[Period %d] (no TX)\n", p);
        s.last_offset = 0;
    }

    s.last_parity = (uint8_t)slot_parity;
    scenario_emit_status(s);

    std::vector<std::string> states;
    autoseq_get_qso_states(states);
    if (!states.empty()) {
        printf("[Period %d] Queue (%d):", p, (int)states.size());
        for (const auto& st : states) {
            printf("  [%s]", st.c_str());
        }
        printf("\n");
    }

    if (s.check_expect) {
        if (!tp.has_expect_tx) {
            fprintf(stderr, "period %d: missing expect.tx\n", p);
            s.failures++;
        } else if (tp.expect_tx != actual_tx) {
            fail_period(s, "TX", tp.expect_tx, actual_tx);
        }
        const std::string expect_log = tp.has_expect_log ? tp.expect_log : "";
        if (expect_log != s.period_log) {
            fail_period(s, "log", expect_log, s.period_log);
        }
        const std::string expect_cab = tp.has_expect_cabrillo ? tp.expect_cabrillo : "";
        if (expect_cab != s.period_cabrillo) {
            fail_period(s, "cabrillo", expect_cab, s.period_cabrillo);
        }
    }

    s.period++;
    return true;
}

void scenario_emit_sweep(Scenario& s, uint8_t kind, uint8_t range_index, uint32_t freq_hz, int swr_c,
                         uint8_t reason)
{
    stamp(s);
    if (!s.wall_slots && s.epoch_secs == 0) {
        s.epoch_secs = kEpochBase;
    }
    porta_sweep_event_t e;
    memset(&e, 0, sizeof(e));
    e.epoch_secs = s.epoch_secs;
    e.kind = kind;
    e.range_index = range_index;
    e.freq_hz = freq_hz;
    e.swr_c = (int16_t)swr_c;
    e.reason = reason;
    char row[160];
    const size_t n = porta_event_json_sweep(row, sizeof(row), s.next_seq, &e);
    if (n > 0) {
        push_raw(s, row);
    }
}
