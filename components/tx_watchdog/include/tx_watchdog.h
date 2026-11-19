#pragma once

#include <stdint.h>

// The TX watchdog (docs/architecture/radio.md, The TX watchdog): what bounds a transmitter that tries do not.
// Header only so host_mock tests it; the radio is poe_p4/main/tx_scheduler.cpp's, the queue autoseq's.

// What has the radio keyed (radio_control_keyed()).
enum class TxWatchdogKey {
    Message,  // radio_control_begin_tx() to radio_control_end_tx(): one FT8 or FT4 transmission
    Tune,     // radio_control_set_tune(true) to (false)
};

// The longest a message keeps the radio keyed: an FT8 slot. Its tones are 12.64 s, after a lead of under a
// second and before a tail of tens of ms; FT4's are 5 s. Longer means nothing will key it down.
constexpr int64_t kTxWatchdogMessageMs = 15000;
// The longest a Tune carrier runs: time for an antenna tuner to match, not for a carrier left on.
constexpr int64_t kTxWatchdogTuneMs = 30000;
// The most transmissions one QSO makes, every step and every reactivation counted. Tries bound each step, but
// a station that keeps answering keeps changing the step or reviving the entry. A QSO at the default tries
// sends at most 17 (4 + 6 + 6 + 1); 40 is 20 minutes of every other slot.
constexpr int kTxWatchdogQsoTransmissions = 40;

inline int64_t tx_watchdog_cap_ms(TxWatchdogKey key) {
    return key == TxWatchdogKey::Tune ? kTxWatchdogTuneMs : kTxWatchdogMessageMs;
}

// The radio has been keyed by `key` since `keyed_since_ms`: true once that is past its cap. Both times on a
// clock no one sets (esp_timer's), so a clock set during a transmission cannot trip it or hide one.
inline bool tx_watchdog_key_over(TxWatchdogKey key, int64_t keyed_since_ms, int64_t now_ms) {
    return now_ms - keyed_since_ms > tx_watchdog_cap_ms(key);
}

// A QSO has made `transmissions` over its life: true when it may make no more.
inline bool tx_watchdog_qso_over(int transmissions) {
    return transmissions >= kTxWatchdogQsoTransmissions;
}
