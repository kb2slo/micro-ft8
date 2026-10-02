#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Samples short of a full slot. slots_crossed is how many slot periods the
// boundary log is closing; sample_rate is the rate cfg->read returns (6 kHz).
// A full FT8 slot is 90,000 samples. Extra samples are not a deficit.
int64_t ft8_slot_sample_deficit(int64_t samples_got, int64_t slots_crossed,
                                int slot_ms, int sample_rate);

#ifdef __cplusplus
}
#endif
