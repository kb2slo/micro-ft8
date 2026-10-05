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

// The slot a block's audio was captured in. now_ms is the soft clock as the block
// completes; queued is the samples captured after it and not yet processed. After a
// decode runs past a boundary, the queue is the last slot's tail, which belongs to
// that slot and not to the one the clock is already in.
int64_t ft8_block_slot_idx(int64_t now_ms, int64_t queued, int slot_ms, int sample_rate);

#ifdef __cplusplus
}
#endif
