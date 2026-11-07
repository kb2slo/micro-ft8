#pragma once

// Where a channel sits in the QMX's 48 kHz I/Q window, and whether it fits (docs/architecture/radio.md,
// The QMX's receive output; docs/architecture/decode.md, FT4). The LO is 12 kHz below the dial, so the
// station's FT8 channel, audio 1,500 Hz at the dial plus 1,500 Hz, is 13,500 Hz above the LO; a channel
// whose dial is offset Hz from FT8's is that much further. Pure, so the host test reads it.

#include <stdint.h>

// The station's FT8 channel against the LO (iq_frontend.h, center_hz).
static constexpr double kQmxIqFt8CenterHz = 13500.0;

// How far from the LO a channel's audio may reach, either side: FT4 decoded without thinning to 21.0 kHz
// above it on the 2026-11-02 corpus, and the radio's ADC filter ends near 21.8 kHz.
static constexpr double kQmxIqWindowEdgeHz = 21000.0;

// The channel's audio spans 1,500 Hz either side of its centre (the decoder searches 200 to 2,900 Hz).
static constexpr double kQmxIqChannelHalfHz = 1500.0;

// The centre for a channel whose dial is dial_offset_hz from FT8's.
double qmx_iq_channel_center_hz(int32_t dial_offset_hz);

// Null if that channel fits the window; else why not, for the radio bar and the log.
const char* qmx_iq_channel_refusal(int32_t dial_offset_hz);
