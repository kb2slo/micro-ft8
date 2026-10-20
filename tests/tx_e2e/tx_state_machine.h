#ifndef _TX_E2E_STATE_MACHINE_H_
#define _TX_E2E_STATE_MACHINE_H_

#include <stdint.h>
#include <string>
#include <vector>
#include "../../components/ft8_lib/vendor/ft8/constants.h"
#include "tx_timing.h"

#ifdef __cplusplus
extern "C" {
#endif

// Configuration for TX state machine
struct TxConfig {
    const char* protocol_name;   // "FT8" or "FT4"
    ftx_protocol_t protocol;
    float base_hz;
    int64_t slot_start_ms;       // Simulated wall clock at slot boundary
    int skip_tones;              // How many tones to skip before TX
    const char* text;            // Message to transmit
    int64_t cancel_ms = -1;      // >= 0: tx_scheduler_cancel() lands this far into the slot
    int min_delay_ms = 50;       // Station tx_min_delay_ms: least TX;-to-first-RF gap
    int64_t arm_ms = 0;          // How far into the slot the TX is keyed (late start)
};

#ifdef __cplusplus
}

// CAT command event
struct CatEvent {
    int64_t time_ms;
    enum Kind { TX_BEGIN, TA, KEY_UP, TX_END } kind;
    float tone_hz;  // Valid for TA
};

// Run TX state machine, return CAT event stream. Mirrors poe_p4/main/tx_scheduler.cpp
// for a CAT-tone radio: TX_BEGIN (TX;) at arm_ms into the slot with no TA, tone n at
// tx_tone_time_ms(), KEY_UP (TA0;) when the last symbol has run, TX_END (RX;)
// kTxKeyUpTailMs later.
std::vector<CatEvent> run_tx(const TxConfig& cfg, int64_t loop_delay_ms = 2);

// Verify that tick-loop TA events are slot-anchored within tolerance.
// Returns "" on success, or an error string on the first violation.
// sym_ms: symbol period in milliseconds (FT8=160, FT4=48)
std::string verify_event_timing(const std::vector<CatEvent>& events,
                                const TxConfig& cfg,
                                int sym_ms, int64_t loop_delay_ms);

// Synthesize PCM from CAT event stream (phase-continuous FSK)
std::vector<float> events_to_pcm(const std::vector<CatEvent>& events,
                                 int sample_rate, float amplitude);

#endif

#endif // _TX_E2E_STATE_MACHINE_H_
