#include "tx_scheduler.h"

#include <cmath>
#include <cstring>
#include <string>

#include "callsign_hashtable.h"
#include "decode_tx_state.h"
#include "esp_log.h"
#include "esp_random.h"
#include "ft8/constants.h"
#include "ft8/encode.h"
#include "ft8/message.h"
#include "porta_p4_host.h"
#include "protocol.h"
#include "radio_control.h"
#include "radio_ta_format.h"
#include "rtc_core.h"
#include "swr_sweep.h"

extern const ProtocolConfig *g_protocol;
extern volatile bool g_tx_view_dirty;

void log_rxtx_line(char dir, int snr, int offset_hz, const std::string &text, int repeat_counter);

static const char *TAG = "tx_sched";

enum class BeaconMode : uint8_t { OFF = 0, EVEN = 1, ODD = 2 };

static BeaconMode s_beacon = BeaconMode::OFF;
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
static bool s_tx_cat_ok = false;
static bool s_tx_cancel_requested = false;
// Slot a cancel landed in; the trigger will not start a TX in it.
static int64_t s_tx_cancel_slot_idx = -1;
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

static void enqueue_beacon_cq(void) {
    const int target_parity = (s_beacon == BeaconMode::EVEN) ? 0 : 1;
    autoseq_start_cq(target_parity);
    g_tx_view_dirty = true;
}

void tx_scheduler_arm_from_autoseq_or_beacon(void) {
    if (decode_tx_state_get_tx_active()) {
        return;
    }
    AutoseqTxEntry pending;
    if (autoseq_fetch_pending_tx(pending)) {
        arm_pending_tx(pending);
        ESP_LOGI(TAG, "TX ready: %s parity=%d", pending.text.c_str(), s_target_slot_parity);
        return;
    }
    if (s_beacon == BeaconMode::OFF) {
        return;
    }
    enqueue_beacon_cq();
    if (autoseq_fetch_pending_tx(pending)) {
        arm_pending_tx(pending);
        ESP_LOGI(TAG, "Beacon CQ ready: %s parity=%d", pending.text.c_str(), s_target_slot_parity);
    }
}

// Same three cases as the ADV's apply_beacon_mode(): Off re-arms whatever
// autoseq still has queued rather than dropping it, and Even <-> Odd drops the
// old-parity CQ before queueing the new one.
void tx_scheduler_set_beacon(uint8_t mode) {
    const BeaconMode next = (BeaconMode)mode;
    if (mode > 2 || s_beacon == next) {
        return;
    }
    const bool was_off = (s_beacon == BeaconMode::OFF);
    const bool tx_active = decode_tx_state_get_tx_active();
    s_beacon = next;
    ESP_LOGI(TAG, "beacon mode=%u", (unsigned)mode);
    AutoseqTxEntry pending;
    if (s_beacon == BeaconMode::OFF) {
        autoseq_cancel_cq(tx_active);
        if (!tx_active) {
            if (autoseq_fetch_pending_tx(pending)) {
                arm_pending_tx(pending);
            } else {
                s_qso_xmit = false;
                s_pending_tx_valid = false;
            }
        }
    } else if (was_off) {
        enqueue_beacon_cq();
        if (autoseq_fetch_pending_tx(pending)) {
            arm_pending_tx(pending);
        }
    } else {
        autoseq_cancel_cq(tx_active);
        enqueue_beacon_cq();
        if (!tx_active && autoseq_fetch_pending_tx(pending)) {
            arm_pending_tx(pending);
        }
    }
}

uint8_t tx_scheduler_beacon_mode(void) {
    return (uint8_t)s_beacon;
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
    // End the radio TX and mark the view dirty now, the same as the ADV's
    // request_tx_cancel() already does -- this used to defer both to
    // tx_tick()'s next call noticing s_tx_cancel_requested, a real drift
    // from the ADV found while scoping row j4xh.
    if (radio_control_ready()) {
        radio_control_end_tx();
    }
    g_tx_view_dirty = true;
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

static void tx_start(int skip_tones) {
    if (decode_tx_state_get_tx_active()) {
        return;
    }
    if (swr_sweep_busy()) {
        // An SWR sweep has the radio (docs/architecture/radio.md, The SWR sweep). It refuses to start over a queued QSO or the
        // beacon, but the operator can arm one after; that slot is skipped.
        return;
    }
    if (!s_pending_tx_valid || s_pending_tx.text.empty()) {
        ESP_LOGW(TAG, "tx_start: no pending TX");
        return;
    }

    const int64_t now_ms = rtc_now_ms();
    const int slot_period = g_protocol->slot_time_ms;
    s_tx_slot_idx = now_ms / slot_period;

    ESP_LOGI(TAG, "tx_start: TX=%s offset=%d skip=%d slot=%lld", s_pending_tx.text.c_str(),
             s_pending_tx.offset_hz, skip_tones, (long long)s_tx_slot_idx);

    autoseq_on_tx_starting();

    ftx_message_t msg;
    const ftx_message_rc_t rc =
        ftx_message_encode(&msg, &hash_if, s_pending_tx.text.c_str());
    if (rc != FTX_MESSAGE_RC_OK) {
        ESP_LOGE(TAG, "Encode failed for TX");
        return;
    }
    if (g_protocol->protocol_id == FTX_PROTOCOL_FT4) {
        ft4_encode(msg.payload, s_tx_tones);
    } else {
        ft8_encode(msg.payload, s_tx_tones);
    }

    s_tx_base_hz = s_pending_tx.offset_hz;
    s_tx_slot_start_ms = (now_ms / slot_period) * slot_period;
    s_tx_tone_idx =
        (skip_tones >= g_protocol->total_symbols) ? g_protocol->total_symbols : skip_tones;
    const int sym_ms = (int)roundf(g_protocol->symbol_period * 1000.0f);
    s_tx_next_tone_time = s_tx_slot_start_ms + s_tx_tone_idx * sym_ms;
    s_tx_last_ta_int = -1;
    s_tx_last_ta_frac = -1;

    s_tx_cat_ok = radio_control_ready();
    if (s_tx_cat_ok) {
        const int freq_hz = porta_p4_station_freq_hz();
        const esp_err_t err = radio_control_begin_tx(freq_hz, s_tx_base_hz);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "tx_start: radio TX begin failed: %s", esp_err_to_name(err));
            s_tx_cat_ok = false;
        }
    }

    if (s_tx_cat_ok) {
        const int remaining = g_protocol->total_symbols - s_tx_tone_idx;
        const size_t n = remaining > 0 ? (size_t)remaining : 0;
        const esp_err_t cpfsk = radio_control_begin_cpfsk_tx(
            static_cast<float>(s_tx_base_hz), n ? s_tx_tones + s_tx_tone_idx : nullptr, n,
            g_protocol->tone_spacing, g_protocol->samples_per_symbol);
        if (cpfsk != ESP_OK && cpfsk != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "tx_start: CPFSK start failed: %s", esp_err_to_name(cpfsk));
            radio_control_end_tx();
            s_tx_cat_ok = false;
            return;
        }
    }

    if (skip_tones > 0) {
        ESP_LOGI(TAG, "Skipping first %d tones (late start)", skip_tones);
    }

    if (s_tx_cat_ok && s_tx_tone_idx < g_protocol->total_symbols) {
        const float tone_hz =
            s_tx_base_hz + g_protocol->tone_spacing * s_tx_tones[s_tx_tone_idx];
        tx_send_ta(tone_hz);
    }

    decode_tx_state_set_tx_active(true);
}

static void tx_tick(void) {
    if (!decode_tx_state_get_tx_active()) {
        return;
    }

    const int64_t now_ms = rtc_now_ms();

    if (s_tx_cancel_requested) {
        ESP_LOGI(TAG, "tx_tick: TX cancelled at tone %d", s_tx_tone_idx);
        if (s_tx_cat_ok) {
            radio_control_end_tx();
        }
        decode_tx_state_set_tx_active(false);
        s_pending_tx_valid = false;
        s_tx_cancel_requested = false;
        decode_tx_state_set_was_txing(false);
        g_tx_view_dirty = true;
        return;
    }

    if (now_ms < s_tx_next_tone_time) {
        return;
    }

    if (s_tx_tone_idx >= g_protocol->total_symbols) {
        ESP_LOGI(TAG, "tx_tick: TX complete, all %d tones sent", g_protocol->total_symbols);
        if (s_tx_cat_ok) {
            radio_control_end_tx();
        }
        autoseq_mark_sent(s_tx_slot_idx);
        decode_tx_state_set_tx_active(false);
        s_pending_tx_valid = false;
        s_tx_cancel_requested = false;
        g_tx_view_dirty = true;
        return;
    }

    const float tone_hz = s_tx_base_hz + g_protocol->tone_spacing * s_tx_tones[s_tx_tone_idx];
    if (s_tx_cat_ok) {
        tx_send_ta(tone_hz);
    }
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
        g_tx_view_dirty = true;
    }

    if (!decode_tx_state_get_was_txing() && !decode_tx_state_get_tx_active() &&
        !(s_qso_xmit && s_pending_tx_valid)) {
        tx_scheduler_arm_from_autoseq_or_beacon();
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
        tx_scheduler_arm_from_autoseq_or_beacon();
    }

    if (s_qso_xmit && s_target_slot_parity == slot_parity && slot_idx != s_tx_cancel_slot_idx &&
        slot_ms < (g_protocol->slot_time_ms * 4 / 15) && !decode_tx_state_get_tx_active() &&
        !decode_tx_state_get_decode_in_progress() &&
        decode_tx_state_get_decode_applied_slot_idx() >= slot_idx - 1) {

        ESP_LOGI(TAG, "TX trigger: starting TX in slot %lld (parity %d)", (long long)slot_idx,
                 slot_parity);

        const int sym_ms = (int)roundf(g_protocol->symbol_period * 1000.0f);
        const int skip_tones = slot_ms / sym_ms;
        if (skip_tones < g_protocol->total_symbols) {
            if (s_pending_tx_valid && !s_pending_tx.text.empty()) {
                s_qso_xmit = false;
                decode_tx_state_set_was_txing(true);
                log_rxtx_line('T', 0, s_pending_tx.offset_hz, s_pending_tx.text,
                              s_pending_tx.repeat_counter);
                tx_start(skip_tones);
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
