#pragma once

#include <stdint.h>
#include "ft8/constants.h"

// ---------------------------------------------------------------------------
// Runtime protocol configuration for Micro-FT8.
//
// A ProtocolConfig is one mode's constants. g_protocol is the station's mode,
// the one its own channel receives: FT8, and nothing reassigns it. FT4 is
// received beside it on a channel of its own, which carries &kProtocolFT4 in
// its config (docs/architecture/decode.md, Channels and FT4), and a queue
// entry transmits in the mode of the decode it came from
// (docs/architecture/autoseq.md, The queue).
// ---------------------------------------------------------------------------

struct ProtocolConfig {
    ftx_protocol_t protocol_id;  ///< FTX_PROTOCOL_FT8 or FTX_PROTOCOL_FT4
    const char*    name;         ///< "FT8" or "FT4"
    int            slot_time_ms; ///< Slot period in ms (15000 FT8, 7500 FT4)
    float          symbol_period;///< Symbol duration in seconds
    int            total_symbols;///< Total channel symbols (FT8_NN / FT4_NN)
    float          tone_spacing; ///< Tone spacing in Hz (6.25 FT8, 20.8333 FT4)
    uint32_t       samples_per_symbol; ///< Exact samples at 48 kHz UAC OUT
};

inline constexpr ProtocolConfig kProtocolFT8 = {
    .protocol_id   = FTX_PROTOCOL_FT8,
    .name          = "FT8",
    .slot_time_ms  = 15000,
    .symbol_period = FT8_SYMBOL_PERIOD,  // 0.160f
    .total_symbols = FT8_NN,
    .tone_spacing  = 6.25f,
    .samples_per_symbol = 7680,
};

inline constexpr ProtocolConfig kProtocolFT4 = {
    .protocol_id   = FTX_PROTOCOL_FT4,
    .name          = "FT4",
    .slot_time_ms  = 7500,
    .symbol_period = FT4_SYMBOL_PERIOD,  // 0.048f
    .total_symbols = FT4_NN,
    .tone_spacing  = 20.8333f,
    .samples_per_symbol = 2304,
};

// g_protocol — the station's mode. Defined in poe_p4/main/main.cpp as
// &kProtocolFT8.
extern const ProtocolConfig* g_protocol;
