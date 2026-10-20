#pragma once

#include <stdint.h>

// When a CAT-tone TX (QMX) puts its tones on the air, relative to the slot.
// One home for the firmware (poe_p4/main/tx_scheduler.cpp) and the host model
// that tests it (tests/tx_e2e/tx_state_machine.cpp).
//
// TX; is sent at the slot boundary and keys PTT with no RF; RF starts on the
// first TA (docs/architecture/radio.md). Tone n then goes out at
// slot_start + kTxToneLeadMs + n * symbol: WSJT-X's own 0.5 s start, and the
// gap an amplifier keyed from the radio's PTT needs to switch before RF.
// The first TA is also never sooner than the operator's minimum TX delay after TX;
// returned (Station.txt tx_min_delay_ms, default 50 ms), however late the TX is
// armed: the amplifier needs that gap after PTT, not after the slot, or the radio's
// SWR protection can trip on the relay's switching transient. It is separate from the
// 0.5 s start, which is the convention and applies to every TX.
// After the last tone, TA0; keys down, kTxKeyUpTailMs later RX; releases PTT.

constexpr int kTxToneLeadMs = 500;
constexpr int kTxKeyUpTailMs = 50;

// When tone `tone_idx` is due.
inline int64_t tx_tone_time_ms(int64_t slot_start_ms, int tone_idx, int sym_ms, int lead_ms) {
    return slot_start_ms + lead_ms + (int64_t)tone_idx * sym_ms;
}

// The first tone of a TX keyed `slot_ms` into its slot. RF cannot start before
// slot_ms + min_delay_ms into the slot, and the tone sounding then is
// (slot_ms + min_delay_ms - lead_ms) / sym_ms. Keyed on the boundary this skips
// nothing; a TX keyed within lead_ms - min_delay_ms of it still skips nothing.
inline int tx_skip_tones(int slot_ms, int sym_ms, int lead_ms, int min_delay_ms) {
    const int first_rf_ms = slot_ms + min_delay_ms;
    return first_rf_ms <= lead_ms ? 0 : (first_rf_ms - lead_ms) / sym_ms;
}

// The earliest the first TA may go out, given when TX; returned.
inline int64_t tx_first_rf_ms(int64_t keyed_ms, int min_delay_ms) {
    return keyed_ms + min_delay_ms;
}
