#pragma once

// Which candidates a later search may skip. A decode reads only its own candidate's
// tone bins (ft8_extract_likelihood), and a subtraction lowers only the decoded
// signal's tone bins, at every freq_sub and its neighbouring time_sub. So a candidate
// that already failed, with no subtraction on any of its bins, reads the same cells
// and fails again. Skipping it changes no output (docs/architecture/decode.md).

#include <stdint.h>

#include "decode_limits.h"
#include "ft8/decode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ftx_candidate_t failed[DECODE_MAX_CANDIDATES];
    int n_failed;
    int16_t subtracted_bin[DECODE_MAX_CANDIDATES];
    int n_subtracted;
    // More subtractions than the list holds: one unrecorded could touch any bin.
    int subtracted_overflow;
} decode_retry_t;

void decode_retry_reset(decode_retry_t* retry);
void decode_retry_note_failed(decode_retry_t* retry, const ftx_candidate_t* candidate);
void decode_retry_note_subtracted(decode_retry_t* retry, const ftx_candidate_t* candidate);

// 1 when `candidate` failed at this exact position and no subtraction overlaps its
// bins. `n_tones` is 8 for FT8 and 4 for FT4. A full failed list only means a
// candidate that failed is retried, which is the safe direction.
int decode_retry_unchanged(const decode_retry_t* retry, const ftx_candidate_t* candidate,
                           int n_tones);

#ifdef __cplusplus
}
#endif
