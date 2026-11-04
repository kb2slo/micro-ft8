#pragma once

// How the HL2's receive stream goes through components/iq_frontend (RFC 0014 §3.4, S1). Header only; the P4's
// hl2_transport.cpp uses it, and host_test_hl2_frontend pins it.
//
// The HL2's receiver NCO sits on the dial, so FT8's audio band, 0 to 3 kHz above the dial, is 0 to 3 kHz above the
// LO, and 1,500 Hz of audio is 1,500 Hz above it. The HL2's I/Q is taken as standard: I then Q, positive
// frequencies above the NCO. If the band came out mirrored, nothing would decode, and negate_q is what flips it.
// No IF and no analog mixer, so no image to correct and no trim.

#include "iq_frontend.h"

static const iq_frontend_config_t kHl2RxFrontendConfig = {
    48000,   // input_rate_hz: one receiver at 48 kHz
    6000,    // output_rate_hz: the pipeline's
    1500.0,  // center_hz: 1,500 Hz of audio is 1,500 Hz above the NCO
    0.0,     // if_trim_hz
    false,   // swap_iq
    false,   // negate_q
};
