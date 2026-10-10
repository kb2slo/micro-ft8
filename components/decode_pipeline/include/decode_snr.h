#pragma once

// Decode SNR on WSJT-X's scale: the signal against the noise in 2500 Hz, floored at -24 as jt9 is.
// Method, the fitted constant, and how far it sits from jt9: docs/architecture/decode.md (SNR).

#include <stdint.h>

#include "common/monitor.h"
#include "ft8/decode.h"

#ifdef __cplusplus
extern "C" {
#endif

// jt9 reports nothing lower.
#define DECODE_SNR_FLOOR_DB -24
// The most a report can carry: WSJT-X reads a sent +55 as -46 (ft8code, WSJT-X 3.0.1).
#define DECODE_SNR_CEILING_DB 49
// Fitted on the synthetic grid in tools/decode_recall (FT8, time_osr 2, freq_osr 2, 6 kHz). report.py fails
// CI when the grid stops reading its injected SNR, so a change to the method or the waterfall must refit it.
#define DECODE_SNR_FT8_OFFSET_DB -26.1f
// The noise at a tone is the 10th percentile of the per-frequency slot means within this distance.
#define DECODE_SNR_BASELINE_HZ 400.0f
#define DECODE_SNR_BASELINE_PERCENT 10
// Frequency columns kept per slot: 864 at f 200-2900 Hz and freq_osr 2.
#define DECODE_SNR_MAX_COLUMNS 1024
// Analysis frame length in subblocks: freq_osr * time_osr, 4 in the firmware.
#define DECODE_SNR_MAX_FRAME_SUBBLOCKS 16

typedef struct {
    const float* block_gain;  // the audio pipeline's AGC gain per waterfall block; not owned
    int frame_subblocks;      // 0 when the slot could not be prepared
    float frame_weight[DECODE_SNR_MAX_FRAME_SUBBLOCKS];  // Hann^2 share of each subblock in the frame
    int num_columns;          // num_bins * freq_osr, in frequency order: bin * freq_osr + freq_sub
    int16_t column_cdb[DECODE_SNR_MAX_COLUMNS];  // slot-mean power per column, AGC removed, 0.01 dB
} decode_snr_slot_t;

// Once per slot, before the first subtraction. block_gain[b] is the gain the audio pipeline applied to
// waterfall block b (1.0 with no AGC); it must hold monitor->wf.num_blocks entries and outlive the slot.
// Returns 0 if this waterfall's shape is beyond the limits above; every SNR is then the floor.
int decode_snr_prepare(decode_snr_slot_t* slot, const monitor_t* monitor, const float* block_gain);

// SNR of one decoded message, in whole dB, from DECODE_SNR_FLOOR_DB to DECODE_SNR_CEILING_DB. Reads the
// waterfall as it is now, so call it before this message is subtracted.
int decode_snr_message(const decode_snr_slot_t* slot, const monitor_t* monitor,
                       const ftx_candidate_t* candidate, const uint8_t* payload);

#ifdef __cplusplus
}
#endif
