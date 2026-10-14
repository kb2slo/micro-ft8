#include "tx_scheduler.h"

#include <cmath>
#include <cstring>
#include <string>

#include "callsign_hashtable.h"
#include "debug_log.h"
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
#include "swr_sweep.h"

extern const ProtocolConfig *g_protocol;

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
static int64_t s_tx_slot_idx = 0;
static bool s_tx_cancel_requested = false;
// Slot a cancel landed in; the trigger will not start a TX in it.
static int64_t s_tx_cancel_slot_idx = -1;
// Slot tx_start() skipped; the trigger will not try again in it.
static int64_t s_tx_skip_slot_idx = -1;
static int s_tx_last_ta_int = -1;
static int s_tx_last_ta_frac = -1;

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
    const int64_t now_ms = rtc_now_ms();
    const int slot_period = g_protocol->slot_time_ms;
    const int64_t slot_idx = now_ms / slot_period;
    const int slot_ms = (int)(now_ms % slot_period);
    // The slot under way can still be keyed inside the trigger's window
    // (check_slot_boundary()), unless the trigger refuses it.
    const bool keyable = slot_ms < (slot_period * 4 / 15) && !decode_tx_state_get_tx_active() &&
                         slot_idx != s_tx_cancel_slot_idx && slot_idx != s_tx_skip_slot_idx;
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
    s_tx_cancel_slot_idx = rtc_now_ms() / g_protocol->slot_time_ms;
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
    if (swr_sweep_busy()) {
        // An SWR sweep has the radio (docs/architecture/radio.md, The SWR sweep). It refuses to start over a queued QSO or
        // CQ, but the operator can queue one after; that slot is skipped.
        skip_reason = "SWR sweep running";
        return TxStart::Skipped;
    }
    if (!s_pending_tx_valid || s_pending_tx.text.empty()) {
        skip_reason = "no pending TX";
        return TxStart::Skipped;
    }

    const int64_t now_ms = rtc_now_ms();
    const int slot_period = g_protocol->slot_time_ms;
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
    if (g_protocol->protocol_id == FTX_PROTOCOL_FT4) {
        ft4_encode(msg.payload, s_tx_tones);
    } else {
        ft8_encode(msg.payload, s_tx_tones);
    }

    const int tx_base_hz = s_pending_tx.offset_hz;
    const int tone_idx =
        (skip_tones >= g_protocol->total_symbols) ? g_protocol->total_symbols : skip_tones;

    // Key the radio before autoseq is told: autoseq_on_tx_starting() records the
    // entry as transmitting and logs a QSO on RR73 or 73, so it must not run
    // for a slot that cannot go out.
    if (!radio_control_ready()) {
        skip_reason = "CAT not ready";
        return TxStart::Skipped;
    }
    const esp_err_t err = radio_control_begin_tx(porta_p4_station_freq_hz(), tx_base_hz);
    if (err != ESP_OK) {
        radio_control_end_tx();
        skip_reason = std::string("radio TX begin failed: ") + esp_err_to_name(err);
        return TxStart::Skipped;
    }
    const int remaining = g_protocol->total_symbols - tone_idx;
    const size_t n = remaining > 0 ? (size_t)remaining : 0;
    const esp_err_t cpfsk = radio_control_begin_cpfsk_tx(
        static_cast<float>(tx_base_hz), n ? s_tx_tones + tone_idx : nullptr, n,
        g_protocol->tone_spacing, g_protocol->samples_per_symbol);
    if (cpfsk != ESP_OK && cpfsk != ESP_ERR_NOT_SUPPORTED) {
        radio_control_end_tx();
        skip_reason = std::string("CPFSK start failed: ") + esp_err_to_name(cpfsk);
        return TxStart::Skipped;
    }

    autoseq_on_tx_starting();

    s_tx_base_hz = tx_base_hz;
    s_tx_slot_start_ms = (now_ms / slot_period) * slot_period;
    s_tx_tone_idx = tone_idx;
    const int sym_ms = (int)roundf(g_protocol->symbol_period * 1000.0f);
    s_tx_next_tone_time = s_tx_slot_start_ms + s_tx_tone_idx * sym_ms;
    s_tx_last_ta_int = -1;
    s_tx_last_ta_frac = -1;

    if (skip_tones > 0) {
        ESP_LOGI(TAG, "Skipping first %d tones (late start)", skip_tones);
    }

    if (s_tx_tone_idx < g_protocol->total_symbols) {
        const float tone_hz =
            s_tx_base_hz + g_protocol->tone_spacing * s_tx_tones[s_tx_tone_idx];
        tx_send_ta(tone_hz);
    }

    decode_tx_state_set_tx_active(true);
    decode_tx_state_set_tx_slot_idx(s_tx_slot_idx);
    return TxStart::Started;
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
        decode_tx_state_set_was_txing(false);
        return;
    }

    if (now_ms < s_tx_next_tone_time) {
        return;
    }

    if (s_tx_tone_idx >= g_protocol->total_symbols) {
        ESP_LOGI(TAG, "tx_tick: TX complete, all %d tones sent", g_protocol->total_symbols);
        radio_control_end_tx();
        autoseq_mark_sent(s_tx_slot_idx);
        decode_tx_state_set_tx_active(false);
        s_pending_tx_valid = false;
        s_tx_cancel_requested = false;
        return;
    }

    const float tone_hz = s_tx_base_hz + g_protocol->tone_spacing * s_tx_tones[s_tx_tone_idx];
    tx_send_ta(tone_hz);
    s_tx_tone_idx++;
    s_tx_next_tone_time =
        s_tx_slot_start_ms +
        s_tx_tone_idx * (int)roundf(g_protocol->symbol_period * 1000.0f);
}

static void check_slot_boundary(void) {
    const int64_t now_ms = rtc_now_ms();
    const int slot_period = g_protocol->slot_time_ms;
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
    // Not while a decode runs: it mutates the queue on the other task, and
    // the trigger below waits for it anyway.
    if (s_qso_xmit && s_pending_tx_valid && !decode_tx_state_get_tx_active() &&
        !decode_tx_state_get_decode_in_progress() &&
        autoseq_head_changed_or_refresh(&s_pending_tx)) {
        s_qso_xmit = false;
        s_pending_tx_valid = false;
        tx_scheduler_arm_from_autoseq();
    }

    if (s_qso_xmit && s_target_slot_parity == slot_parity && slot_idx != s_tx_cancel_slot_idx &&
        slot_idx != s_tx_skip_slot_idx &&
        slot_ms < (g_protocol->slot_time_ms * 4 / 15) && !decode_tx_state_get_tx_active() &&
        !decode_tx_state_get_decode_in_progress() &&
        decode_tx_state_get_decode_applied_slot_idx() >= slot_idx - 1) {

        ESP_LOGI(TAG, "TX trigger: starting TX in slot %lld (parity %d)", (long long)slot_idx,
                 slot_parity);

        const int sym_ms = (int)roundf(g_protocol->symbol_period * 1000.0f);
        const int skip_tones = slot_ms / sym_ms;
        if (skip_tones < g_protocol->total_symbols) {
            if (s_pending_tx_valid && !s_pending_tx.text.empty()) {
                std::string skip_reason;
                const TxStart started = tx_start(skip_tones, skip_reason);
                if (started == TxStart::Skipped) {
                    // Armed still, so the entry goes out in its next slot; the
                    // guard stops another attempt in this one.
                    s_tx_skip_slot_idx = slot_idx;
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
