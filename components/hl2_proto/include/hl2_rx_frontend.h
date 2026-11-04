#pragma once

// How the HL2's receive stream goes through components/iq_frontend (decode.md, The HL2's channel; row mg9c). Header only; the P4's
// hl2_transport.cpp uses it, and host_test_hl2_frontend pins it.
//
// The HL2's receiver NCO sits on the dial, so FT8's audio band, 0 to 3 kHz above the dial, is 0 to 3 kHz above the
// LO, and 1,500 Hz of audio is 1,500 Hz above it. The HL2's Q is the negative of the standard convention, so a
// signal above the NCO arrives as I = cos, Q = -sin: with Q taken as sent, 40 m found 256 candidates a slot and
// decoded none; with it negated, 9 to 30 a slot (bench, gateware 74.2, 2026-11-03).
// No IF and no analog mixer, so no image to correct and no trim.

#include "iq_frontend.h"

static const iq_frontend_config_t kHl2RxFrontendConfig = {
    48000,   // input_rate_hz: one receiver at 48 kHz
    6000,    // output_rate_hz: the pipeline's
    1500.0,  // center_hz: 1,500 Hz of audio is 1,500 Hz above the NCO
    0.0,     // if_trim_hz
    false,   // swap_iq
    true,    // negate_q: the HL2's Q (above)
};
