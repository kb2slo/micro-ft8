#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Busy percent of one core over a window. idle_delta is how long that core's
// idle task ran; elapsed is the FreeRTOS run-time counter over the same
// window. Both are that counter (a uint32 subtract, so a wrap is fine).
// Returns 0..100. A zero elapsed, or an idle delta past elapsed, is 0.
int decode_core_busy_pct(uint32_t idle_delta, uint32_t elapsed);

#ifdef __cplusplus
}
#endif
