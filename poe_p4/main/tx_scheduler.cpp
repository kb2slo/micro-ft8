#include "tx_scheduler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "amp_band_prime.h"
#include "callsign_hashtable.h"
#include "debug_log.h"
#include "decode_pipeline.h"
#include "decode_tx_state.h"
#include "esp_log.h"
#include "esp_random.h"
#include "ft8/constants.h"
#include "ft8/encode.h"
#include "ft8/message.h"
#include "porta_host.h"
#include "protocol.h"
#include "radio_control.h"
#include "radio_ta_format.h"
#include "rtc_core.h"
#include "tx_timing.h"
#include "swr_sweep.h"
#include "swr_trip.h"

// The protocol a transmission goes out in is its autoseq entry's (AutoseqTxEntry::mode), not a station-wide
// one: FT8, or FT4 beside it (docs/architecture/autoseq.md, The slot cycle).
static const ProtocolConfig *proto_of(RxMode mode) {
    return mode == RxMode::FT4 ? &kProtocolFT4 : &kProtocolFT8;
}

void log_rxtx_line(char dir, int snr, int offset_hz, const std::string &text, int repeat_counter);

static const char *TAG = "tx_sched";

static volatile bool s_qso_xmit = false;
static int s_target_slot_parity = 0;
static AutoseqTxEntry s_pending_tx;
static bool s_pending_tx_valid = false;
static int s_last_slot_parity = -1;

static uint8_t s_tx_tones[FT4_NN];
static int s_tx_tone_idx = 0;
static int s_tx_base_hz = 1500;
static int64_t s_tx_slot_start_ms = 0;
static int64_t s_tx_next_tone_time = 0;
// First tone is due this long after the slot boundary (radio_control_tone_lead_ms()).
static int s_tx_lead_ms = 0;
// After the last tone: TA0; has keyed down and RX; follows at this time.
static bool s_tx_keyed_down = false;
static int64_t s_tx_rx_at_ms = 0;
static int64_t s_tx_slot_idx = 0;
// The protocol of the transmission on the air, or of the last one.
static const ProtocolConfig *s_tx_proto = &kProtocolFT8;
static bool s_tx_cancel_requested = false;
// The end of the slot a cancel landed in; the trigger will not start a TX before it. A time, not a slot
// index, so it holds whichever mode is armed next.
static int64_t s_tx_cancel_until_ms = 0;
// The slot whose skip the TX HUD shows as Not keyed, and why (tx_scheduler_hud_not_keyed_reason()).
static int64_t s_not_keyed_until_ms = 0;
static std::string s_not_keyed_reason;
// The end of the slot tx_start() skipped; the trigger will not try again before it.
static int64_t s_tx_skip_until_ms = 0;
static int s_tx_last_ta_int = -1;
static int s_tx_last_ta_frac = -1;

// SWR protection (docs/architecture/radio.md, SWR protection trips on a QMX). Whether the
// radio answers SR; is learned at each CAT connect.
enum class SrProbe { Idle, Waiting, Yes, No };
static SrProbe s_sr_probe = SrProbe::Idle;
static bool s_sr_was_ready = false;
static int64_t s_sr_probe_deadline_ms = 0;
static constexpr int64_t kSrProbeMs = 1000;
static bool s_sr_readback_pending = false;
static SwrTrip s_swr_trip;
static bool s_swr_operator_start_pending = false;
static bool s_swr_operator_clear_anyway = false;
// Slot a trip or a Tune trip ended a transmission in; the TX HUD shows it for the rest of it.
static int64_t s_swr_abort_until_ms = 0;
static constexpr const char *kSwrReason = "SWR protection";

static int resolve_tx_offset(const AutoseqTxEntry &e) {
    const int src = porta_p4_station_offset_src();
    const int cursor = porta_p4_station_offset_hz();
    if (src == 1) {  // CURSOR
        return cursor > 0 ? cursor : 1500;
    }
    if (src == 2 && e.offset_hz > 0 && e.text.rfind("CQ ", 0) != 0) {  // RX
        return e.offset_hz;
    }
    return 500 + (int)(esp_random() % 2001);
}

void arm_pending_tx(const AutoseqTxEntry &pending) {
    // A prior TX_CANCEL (e.g. Operate band change) must not abort this new
    // arm — field-hit B68: first beacon TX died at tone 0, second succeeded.
    // A TX still on air keeps its pending cancel.
    if (!decode_tx_state_get_tx_active()) {
        s_tx_cancel_requested = false;
    }
    s_qso_xmit = true;
    s_target_slot_parity = pending.slot_id & 1;
    s_pending_tx = pending;
    s_pending_tx.offset_hz = resolve_tx_offset(s_pending_tx);
    s_pending_tx_valid = true;
}

void tx_scheduler_arm_from_autoseq(void) {
    if (decode_tx_state_get_tx_active()) {
        return;
    }
    AutoseqTxEntry pending;
    if (autoseq_fetch_pending_tx(pending)) {
        arm_pending_tx(pending);
        ESP_LOGI(TAG, "TX ready: %s parity=%d", pending.text.c_str(), s_target_slot_parity);
    }
}

void tx_scheduler_rearm(void) {
    // A TX on the air finishes as armed; its tick re-arms from the queue.
    if (decode_tx_state_get_tx_active()) {
        return;
    }
    AutoseqTxEntry pending;
    if (autoseq_fetch_pending_tx(pending)) {
        arm_pending_tx(pending);
    } else {
        s_qso_xmit = false;
        s_pending_tx_valid = false;
    }
}

bool tx_scheduler_call_cq(void) {
    tx_scheduler_swr_operator_start(false);
    const int64_t now_ms = rtc_now_ms();
    const int slot_period = kProtocolFT8.slot_time_ms;  // the CQ is FT8's
    const int64_t slot_idx = now_ms / slot_period;
    const int slot_ms = (int)(now_ms % slot_period);
    // The slot under way can still be keyed inside the trigger's window
    // (check_slot_boundary()), unless the trigger refuses it.
    const bool keyable = slot_ms < (slot_period * 4 / 15) && !decode_tx_state_get_tx_active() &&
                         now_ms >= s_tx_cancel_until_ms && now_ms >= s_tx_skip_until_ms;
    if (!autoseq_call_cq(autoseq_cq_parity((int)(slot_idx & 1), keyable))) {
        return false;
    }
    tx_scheduler_rearm();
    return true;
}

int tx_scheduler_target_parity(void) {
    return s_target_slot_parity;
}

void tx_scheduler_cancel(void) {
    // Autoseq still holds the entry and the next tick re-arms it. Without
    // this it restarted in the same slot as a late start whenever the cancel
    // came in its first 4/15 -- on the old band, so a band change found a TX
    // on air and could not retune.
    {
        // The rest of the slot it landed in: the transmission's own, or the armed entry's.
        const ProtocolConfig *p = decode_tx_state_get_tx_active() ? s_tx_proto
                                  : s_pending_tx_valid               ? proto_of(s_pending_tx.mode)
                                                                     : &kProtocolFT8;
        const int64_t now_ms = rtc_now_ms();
        s_tx_cancel_until_ms = (now_ms / p->slot_time_ms + 1) * p->slot_time_ms;
    }
    s_qso_xmit = false;
    if (!decode_tx_state_get_tx_active()) {
        // Idle/armed: drop the pending TX now. Do not leave
        // s_tx_cancel_requested sticky for a later arm (band-change cancel
        // then beacon Even was the B68 repro).
        s_pending_tx_valid = false;
        s_tx_cancel_requested = false;
    } else {
        s_tx_cancel_requested = true;
    }
    // End the radio TX now, not on tx_tick()'s next call noticing
    // s_tx_cancel_requested.
    if (radio_control_ready()) {
        radio_control_end_tx();
    }
}

// The operator's least TX;-to-RF gap. A radio with no lead (one that takes the symbols itself, through
// begin_cpfsk_tx) has nothing to hold back.
static int tx_min_delay_ms(void) {
    return radio_control_tone_lead_ms() > 0 ? porta_p4_station_tx_min_delay_ms() : 0;
}

static void tx_send_ta(float tone_hz) {
    int ta_int = 0;
    int ta_frac = 0;
    radio_ta_parts(tone_hz, &ta_int, &ta_frac);
    if (ta_int == s_tx_last_ta_int && ta_frac == s_tx_last_ta_frac) {
        return;
    }
    if (radio_control_set_tone_hz(tone_hz) == ESP_OK) {
        s_tx_last_ta_int = ta_int;
        s_tx_last_ta_frac = ta_frac;
    }
}

// Started: the radio is keyed and TX is active. Skipped: the slot could not be
// keyed; nothing was recorded, so it is not counted and the entry stays armed.
// EncodeFailed: nothing was keyed, but the try is charged (a message that can
// never encode must not loop forever).
enum class TxStart { Started, Skipped, EncodeFailed };

static TxStart tx_start(int skip_tones, std::string &skip_reason) {
    if (decode_tx_state_get_tx_active()) {
        skip_reason = "TX already active";
        return TxStart::Skipped;
    }
    if (amp_band_prime_busy()) {
        skip_reason = "amplifier band prime running";
        return TxStart::Skipped;
    }
    if (swr_sweep_busy()) {
        // An SWR sweep has the radio (docs/architecture/radio.md, The SWR sweep). It refuses to start over a queued QSO or
        // CQ, but the operator can queue one after; that slot is skipped.
        skip_reason = "SWR sweep running";
        return TxStart::Skipped;
    }
    if (s_swr_trip.stopped) {
        // Two transmissions in a row were stopped by the radio's lock; it stays set until the
        // operator calls CQ, replies or presses Tune (tx_scheduler_swr_operator_start()).
        skip_reason = kSwrReason;
        return TxStart::Skipped;
    }
    if (!rtc_is_set()) {
        // Until a browser sets the clock the slot phase is uptime's (docs/architecture/time.md),
        // so a transmission would land in no one's period. The entry stays armed and goes out
        // in its next slot once the clock is set.
        skip_reason = "clock not set";
        return TxStart::Skipped;
    }
    if (!s_pending_tx_valid || s_pending_tx.text.empty()) {
        skip_reason = "no pending TX";
        return TxStart::Skipped;
    }

    const int64_t now_ms = rtc_now_ms();
    const ProtocolConfig *proto = proto_of(s_pending_tx.mode);
    const int slot_period = proto->slot_time_ms;
    s_tx_slot_idx = now_ms / slot_period;

    ESP_LOGI(TAG, "tx_start: TX=%s offset=%d skip=%d slot=%lld", s_pending_tx.text.c_str(),
             s_pending_tx.offset_hz, skip_tones, (long long)s_tx_slot_idx);

    ftx_message_t msg;
    const ftx_message_rc_t rc =
        ftx_message_encode(&msg, &hash_if, s_pending_tx.text.c_str());
    if (rc != FTX_MESSAGE_RC_OK) {
        ESP_LOGE(TAG, "Encode failed for TX");
        autoseq_on_tx_starting();
        return TxStart::EncodeFailed;
    }
    if (proto->protocol_id == FTX_PROTOCOL_FT4) {
        ft4_encode(msg.payload, s_tx_tones);
    } else {
        ft8_encode(msg.payload, s_tx_tones);
    }

    // FT4 beside FT8 goes out at FT8's dial plus FT4's offset plus its audio offset as a far TA tone, or, below
    // the dial (40 m), from FT4's own dial by a retune (tx_timing.h, tx_plan()).
    const float tones_span_hz = proto->tone_spacing * (proto->protocol_id == FTX_PROTOCOL_FT4 ? 3 : 7);
    const TxPlan plan = tx_plan(s_pending_tx.offset_hz,
                                proto->protocol_id == FTX_PROTOCOL_FT4 ? porta_p4_station_ft4_dial_offset_hz() : 0,
                                tones_span_hz);
    const int tx_base_hz = plan.base_hz;
    if (!tx_tones_in_range(tx_base_hz, tx_base_hz + tones_span_hz)) {
        // A wild offset: never keyed. Not counted, so the entry waits to be cancelled rather than spending tries.
        skip_reason = "TX tone " + std::to_string(tx_base_hz) + " Hz out of range";
        return TxStart::Skipped;
    }
    const int tone_idx =
        (skip_tones >= proto->total_symbols) ? proto->total_symbols : skip_tones;

    // Key the radio before autoseq is told: autoseq_on_tx_starting() records the
    // entry as transmitting and logs a QSO on RR73 or 73, so it must not run
    // for a slot that cannot go out.
    if (!radio_control_ready()) {
        skip_reason = "CAT not ready";
        return TxStart::Skipped;
    }
    const int rx_dial_hz = porta_p4_station_freq_hz();
    const esp_err_t err = radio_control_begin_tx(rx_dial_hz + (int)plan.dial_shift_hz, tx_base_hz, rx_dial_hz);
    if (err != ESP_OK) {
        radio_control_end_tx();
        skip_reason = std::string("radio TX begin failed: ") + esp_err_to_name(err);
        return TxStart::Skipped;
    }
    const int64_t keyed_ms = rtc_now_ms();  // after TX; returned, so the gap is not cut short
    const int remaining = proto->total_symbols - tone_idx;
    const size_t n = remaining > 0 ? (size_t)remaining : 0;
    const esp_err_t cpfsk = radio_control_begin_cpfsk_tx(
        static_cast<float>(tx_base_hz), n ? s_tx_tones + tone_idx : nullptr, n,
        proto->tone_spacing, proto->samples_per_symbol);
    if (cpfsk != ESP_OK && cpfsk != ESP_ERR_NOT_SUPPORTED) {
        radio_control_end_tx();
        skip_reason = std::string("CPFSK start failed: ") + esp_err_to_name(cpfsk);
        return TxStart::Skipped;
    }

    autoseq_on_tx_starting();

    s_tx_base_hz = tx_base_hz;
    s_tx_slot_start_ms = (now_ms / slot_period) * slot_period;
    s_tx_tone_idx = tone_idx;
    s_tx_proto = proto;
    const int sym_ms = (int)roundf(proto->symbol_period * 1000.0f);
    s_tx_lead_ms = radio_control_tone_lead_ms();
    // The first TA waits the minimum TX delay after TX; however late this TX
    // is armed (tx_timing.h). Later tones are slot-anchored again.
    s_tx_next_tone_time =
        std::max(tx_tone_time_ms(s_tx_slot_start_ms, s_tx_tone_idx, sym_ms, s_tx_lead_ms),
                 tx_first_rf_ms(keyed_ms, tx_min_delay_ms()));
    s_tx_last_ta_int = -1;
    s_tx_last_ta_frac = -1;
    s_tx_keyed_down = false;

    if (skip_tones > 0) {
        ESP_LOGI(TAG, "Skipping first %d tones (late start)", skip_tones);
    }
    // No TA here: TX; keyed PTT with no RF, and the first tx_tick() TA starts
    // the RF s_tx_lead_ms after the slot boundary.

    decode_tx_state_set_tx_active(true);
    // Deaf by overlap: the FT8 slot this transmission is in, and for FT4 its own period too, which its
    // decode would otherwise start after the tones end (decode_tx_state.h).
    if (proto->protocol_id == FTX_PROTOCOL_FT4) {
        decode_tx_state_set_tx_ft4_period_idx(s_tx_slot_idx);
        decode_tx_state_set_tx_slot_idx(tx_ft8_slot_of_ft4_period(s_tx_slot_idx));
    } else {
        decode_tx_state_set_tx_slot_idx(s_tx_slot_idx);
    }
    return TxStart::Started;
}

static bool swr_lock_known(void) {
    return s_sr_probe == SrProbe::Yes;
}

// True when the latest SR reply says the lock is set.
static bool swr_lock_set(void) {
    const radio_swr_lock_t *lock = radio_control_swr_lock();
    bool locked = false;
    return swr_lock_known() && lock && lock->reply(&locked) && locked;
}

static void swr_lock_clear(const char *why) {
    const radio_swr_lock_t *lock = radio_control_swr_lock();
    if (!lock) {
        return;
    }
    const esp_err_t err = lock->clear();
    s_sr_readback_pending = err == ESP_OK;
    ESP_LOGI(TAG, "SWR protection lock cleared (%s): %s", why, esp_err_to_name(err));
}

// The operator called CQ, replied or pressed Tune (Tune: clear_anyway, since a Tune trip leaves
// the lock set). Out of the stopped state, and the lock cleared when it was set.
void tx_scheduler_swr_operator_start(bool clear_anyway) {
    if (!swr_lock_known() || decode_tx_state_get_tx_active()) {
        return;
    }
    if (amp_band_prime_busy()) {
        s_swr_operator_start_pending = true;
        s_swr_operator_clear_anyway = s_swr_operator_clear_anyway || clear_anyway;
        return;
    }
    if (swr_trip_operator_start(s_swr_trip) || clear_anyway) {
        swr_lock_clear("operator start");
    }
}

bool tx_scheduler_swr_tripped(void) {
    return swr_lock_set();
}

void tx_scheduler_swr_abort_note(void) {
    const int64_t now_ms = rtc_now_ms();
    s_swr_abort_until_ms = (now_ms / s_tx_proto->slot_time_ms + 1) * s_tx_proto->slot_time_ms;
}

const char *tx_scheduler_hud_not_keyed_reason(void) {
    return rtc_now_ms() < s_not_keyed_until_ms
               ? s_not_keyed_reason.c_str()
               : nullptr;
}

const char *tx_scheduler_hud_abort_reason(void) {
    return rtc_now_ms() < s_swr_abort_until_ms
               ? kSwrReason
               : nullptr;
}

// The radio's lock ended this transmission. Handled like a TX cancel (docs/architecture/autoseq.md,
// The slot cycle): not counted, no try charged, the entry re-armed for its next slot, which the
// trigger skips this one. A second trip in a row leaves the lock set and stops the station.
static void tx_swr_tripped(void) {
    radio_control_end_tx();
    autoseq_on_tx_cancelled();
    decode_tx_state_set_tx_active(false);
    s_pending_tx_valid = false;
    s_tx_cancel_requested = false;
    s_tx_keyed_down = false;
    decode_tx_state_set_was_txing(false);
    s_tx_cancel_until_ms = s_tx_slot_start_ms + s_tx_proto->slot_time_ms;
    s_swr_abort_until_ms = s_tx_cancel_until_ms;

    if (swr_trip_on_trip(s_swr_trip) == SwrTripAction::Retry) {
        ESP_LOGW(TAG, "TX tripped the radio's SWR protection at tone %d: lock cleared, entry re-armed",
                 s_tx_tone_idx);
        debug_log_line("TX slot " + std::to_string((long long)s_tx_slot_idx) +
                       " tripped SWR protection: lock cleared, not counted");
        swr_lock_clear("trip");
        return;
    }
    ESP_LOGE(TAG, "TX tripped the radio's SWR protection twice in a row: lock left set, station stopped");
    debug_log_line("TX tripped SWR protection twice in a row: stopped, CQ ended and QSOs cancelled");
    s_qso_xmit = false;
    std::vector<QsoContext> active;
    autoseq_get_active_contexts(active);
    for (const QsoContext &q : active) {
        autoseq_drop_by_entry_id(q.entry_id);
    }
}

// Learns at each CAT connect whether the radio answers SR; (QMX 1_04_004 and later), and
// whether it was already locked. Waits out a transmission, which clears the replies.
static void swr_lock_tick(void) {
    if (!radio_control_ready()) {
        s_sr_was_ready = false;
        s_sr_probe = SrProbe::Idle;
        s_sr_readback_pending = false;
        s_swr_operator_start_pending = false;
        s_swr_operator_clear_anyway = false;
        return;
    }
    const radio_swr_lock_t *lock = radio_control_swr_lock();
    if (!lock) {
        return;
    }
    if (!s_sr_was_ready) {
        s_sr_was_ready = true;
        s_sr_probe = SrProbe::Idle;
        lock->poll_with_meters(false);
    }
    if (!porta_p4_station_amp_swr_guard()) {
        // Off (the default): no probe, no SR; in the poll, trips go unseen as before.
        if (s_sr_probe != SrProbe::Idle) {
            lock->poll_with_meters(false);
            s_swr_trip = SwrTrip{};
            s_sr_readback_pending = false;
            s_sr_probe = SrProbe::Idle;
        }
        s_swr_operator_start_pending = false;
        s_swr_operator_clear_anyway = false;
        return;
    }
    if (decode_tx_state_get_tx_active() || amp_band_prime_busy() || swr_sweep_busy()) {
        if (s_sr_probe == SrProbe::Waiting) {
            s_sr_probe = SrProbe::Idle;
        }
        return;
    }
    if (s_swr_operator_start_pending) {
        const bool clear_anyway = s_swr_operator_clear_anyway;
        s_swr_operator_start_pending = false;
        s_swr_operator_clear_anyway = false;
        if (swr_trip_operator_start(s_swr_trip) || clear_anyway) {
            swr_lock_clear("deferred operator start");
        }
        return;
    }
    const int64_t now_ms = rtc_now_ms();
    bool locked = false;
    switch (s_sr_probe) {
    case SrProbe::Idle:
        if (lock->query() == ESP_OK) {
            s_sr_probe = SrProbe::Waiting;
            s_sr_probe_deadline_ms = now_ms + kSrProbeMs;
        }
        break;
    case SrProbe::Waiting:
        if (lock->reply(&locked)) {
            s_sr_probe = SrProbe::Yes;
            lock->poll_with_meters(true);
            ESP_LOGI(TAG, "SR; answered: SWR protection lock is %s", locked ? "SET" : "clear");
            s_swr_trip = SwrTrip{};  // a new connection starts clean
            if (locked) {
                swr_trip_on_locked_before_tx(s_swr_trip);
                debug_log_line("SWR protection lock set before we keyed: stopped until CQ, reply or Tune");
            }
        } else if (now_ms >= s_sr_probe_deadline_ms) {
            s_sr_probe = SrProbe::No;
            ESP_LOGW(TAG, "SR; unanswered: SWR protection is not handled on this connection "
                          "(QMX firmware before 1_04_004?)");
            debug_log_line("SR; unanswered: SWR protection not handled (QMX firmware before 1_04_004?)");
        }
        break;
    case SrProbe::Yes:
        if (s_sr_readback_pending && lock->reply(&locked)) {
            s_sr_readback_pending = false;
            if (locked) {
                ESP_LOGW(TAG, "SR0; did not clear the SWR protection lock");
                debug_log_line("SR0; did not clear the SWR protection lock");
            }
        }
        break;
    case SrProbe::No:
        break;
    }
}

static void tx_tick(void) {
    if (!decode_tx_state_get_tx_active()) {
        return;
    }

    const int64_t now_ms = rtc_now_ms();

    if (s_tx_cancel_requested) {
        ESP_LOGI(TAG, "tx_tick: TX cancelled at tone %d", s_tx_tone_idx);
        radio_control_end_tx();
        autoseq_on_tx_cancelled();
        decode_tx_state_set_tx_active(false);
        s_pending_tx_valid = false;
        s_tx_cancel_requested = false;
        s_tx_keyed_down = false;
        decode_tx_state_set_was_txing(false);
        return;
    }

    if (swr_lock_set()) {
        tx_swr_tripped();
        return;
    }

    if (now_ms < s_tx_next_tone_time) {
        return;
    }

    if (!s_tx_keyed_down && s_tx_tone_idx >= s_tx_proto->total_symbols) {
        // The last tone has run its symbol. Key down, then release PTT after
        // the tail, so an amplifier on the radio's PTT sees no RF at release.
        // A radio with no key-up goes straight to RX;.
        s_tx_keyed_down = true;
        s_tx_rx_at_ms = now_ms + (radio_control_key_up() == ESP_OK ? kTxKeyUpTailMs : 0);
    }
    if (s_tx_keyed_down) {
        if (now_ms < s_tx_rx_at_ms) {
            return;
        }
        ESP_LOGI(TAG, "tx_tick: TX complete, all %d tones sent", s_tx_proto->total_symbols);
        radio_control_end_tx();
        autoseq_mark_sent(s_tx_slot_idx);
        swr_trip_on_clean(s_swr_trip);
        decode_tx_state_set_tx_active(false);
        s_pending_tx_valid = false;
        s_tx_cancel_requested = false;
        s_tx_keyed_down = false;
        return;
    }

    const float tone_hz = s_tx_base_hz + s_tx_proto->tone_spacing * s_tx_tones[s_tx_tone_idx];
    tx_send_ta(tone_hz);
    s_tx_tone_idx++;
    s_tx_next_tone_time =
        tx_tone_time_ms(s_tx_slot_start_ms, s_tx_tone_idx,
                        (int)roundf(s_tx_proto->symbol_period * 1000.0f), s_tx_lead_ms);
}

static void check_slot_boundary(void) {
    const int64_t now_ms = rtc_now_ms();
    // The armed entry's own slots: FT4's 7.5 s periods for an FT4 one, FT8's otherwise.
    const ProtocolConfig *proto = (s_qso_xmit && s_pending_tx_valid) ? proto_of(s_pending_tx.mode) : &kProtocolFT8;
    const bool ft4 = proto->protocol_id == FTX_PROTOCOL_FT4;
    const int slot_period = proto->slot_time_ms;
    const int64_t slot_idx = now_ms / slot_period;
    const int slot_ms = (int)(now_ms % slot_period);
    const int slot_parity = (int)(slot_idx & 1);

    if (slot_parity != s_last_slot_parity) {
        s_last_slot_parity = slot_parity;
    }

    if (decode_tx_state_get_was_txing() && !decode_tx_state_get_tx_active()) {
        ESP_LOGI(TAG, "TX completed, calling tick (slot %lld, parity %d)", (long long)slot_idx,
                 slot_parity);
        autoseq_tick(slot_idx, slot_parity, 0);
        decode_tx_state_set_was_txing(false);
    }

    if (!decode_tx_state_get_was_txing() && !decode_tx_state_get_tx_active() &&
        !(s_qso_xmit && s_pending_tx_valid)) {
        tx_scheduler_arm_from_autoseq();
    }

    // The armed TX must still be autoseq's head. A queue change that did not
    // re-arm -- a Port A QUEUE_CANCEL landing between the decode and this slot
    // -- would otherwise key the cancelled message once. Same entry but
    // changed text (a live cq_type switch) is refreshed in place rather than
    // re-armed, so a switch made after this slot armed still lands in it.
    if (s_qso_xmit && s_pending_tx_valid && !decode_tx_state_get_tx_active() &&
        autoseq_head_changed_or_refresh(&s_pending_tx)) {
        s_qso_xmit = false;
        s_pending_tx_valid = false;
        tx_scheduler_arm_from_autoseq();
    }

    // The previous slot's decode is applied once the decode task has marked it
    // and this loop has taken its result (decode_pipeline_apply()). Read in this
    // order: the decode task publishes before it marks (decode_pipeline.cpp).
    // FT4 is gated by its own channel's previous period, as FT8 by the station's previous slot.
    const int64_t applied = ft4 ? decode_tx_state_get_ft4_applied_period_idx()
                                : decode_tx_state_get_decode_applied_slot_idx();
    if (s_qso_xmit && s_target_slot_parity == slot_parity && now_ms >= s_tx_cancel_until_ms &&
        now_ms >= s_tx_skip_until_ms &&
        slot_ms < (slot_period * 4 / 15) && !decode_tx_state_get_tx_active() &&
        !amp_band_prime_busy() &&
        !decode_tx_state_get_decode_in_progress() &&
        applied >= slot_idx - 1 &&
        !decode_pipeline_result_pending(ft4)) {

        ESP_LOGI(TAG, "TX trigger: starting TX in slot %lld (parity %d)", (long long)slot_idx,
                 slot_parity);

        const int sym_ms = (int)roundf(proto->symbol_period * 1000.0f);
        const int skip_tones =
            tx_skip_tones(slot_ms, sym_ms, radio_control_tone_lead_ms(), tx_min_delay_ms());
        if (skip_tones < proto->total_symbols) {
            if (s_pending_tx_valid && !s_pending_tx.text.empty()) {
                std::string skip_reason;
                const TxStart started = tx_start(skip_tones, skip_reason);
                if (started == TxStart::Skipped) {
                    // Armed still, so the entry goes out in its next slot; the
                    // guard stops another attempt in this one.
                    s_tx_skip_until_ms = (slot_idx + 1) * slot_period;
                    s_not_keyed_until_ms = s_tx_skip_until_ms;
                    s_not_keyed_reason = skip_reason;
                    ESP_LOGW(TAG, "TX slot %lld skipped, not counted: %s", (long long)slot_idx,
                             skip_reason.c_str());
                    debug_log_line("TX slot " + std::to_string((long long)slot_idx) +
                                   " skipped, not counted: " + skip_reason);
                } else {
                    s_qso_xmit = false;
                    decode_tx_state_set_was_txing(true);
                    if (started == TxStart::Started) {
                        log_rxtx_line('T', 0, s_pending_tx.offset_hz, s_pending_tx.text,
                                      s_pending_tx.repeat_counter);
                    }
                }
            }
        }
    }
}

void tx_scheduler_tick(void) {
    swr_lock_tick();
    check_slot_boundary();
    tx_tick();
}

bool tx_scheduler_hud_active(void) {
    return decode_tx_state_get_tx_active();
}

bool tx_scheduler_hud_snapshot(const char **text_out, int *offset_hz_out) {
    if (!s_pending_tx_valid || s_pending_tx.text.empty()) {
        return false;
    }
    if (text_out) {
        *text_out = s_pending_tx.text.c_str();
    }
    if (offset_hz_out) {
        *offset_hz_out = s_pending_tx.offset_hz;
    }
    return true;
}
