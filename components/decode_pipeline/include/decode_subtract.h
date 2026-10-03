#pragma once

// Remove a decoded signal from the waterfall and search again.
// How the fraction and the pass count were chosen: docs/architecture/decode.md.

#include "ft8/decode.h"

#ifdef __cplusplus
extern "C" {
#endif

// Two searches fit the air guard. A third does not.
#define DECODE_SUBTRACT_PASSES 2
// Also lower the neighbouring time subdivision. At time_osr 2 that is the other half of the symbol.
#define DECODE_SUBTRACT_NEIGHBORS 1
// Half the estimated SNR. A full subtraction digs below the noise.
#define DECODE_SUBTRACT_FRACTION 0.5f
// About one freq_osr=2 bin. The same signal's other subdivision is not decoded twice.
// A station one tone (6.25 Hz) away still is.
#define DECODE_SUBTRACT_FREQ_SKIP_HZ 4.0f

int decode_subtract_already(const float* seen_hz, int seen, float freq_hz);

// Re-encode `payload` and lower those tones. `neighbors` 0 touches the candidate's
// time_sub only. 1 also clears the adjacent time subdivisions. `fraction` is the
// share of the estimated SNR removed, clamped into the stored 0.5 dB codes.
void decode_subtract_message(ftx_waterfall_t* wf, const ftx_candidate_t* candidate,
                             const uint8_t* payload, int neighbors, float fraction);

#ifdef __cplusplus
}
#endif
