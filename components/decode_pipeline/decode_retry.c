#include "decode_retry.h"

void decode_retry_reset(decode_retry_t* retry)
{
    retry->n_failed = 0;
    retry->n_subtracted = 0;
    retry->subtracted_overflow = 0;
}

void decode_retry_note_failed(decode_retry_t* retry, const ftx_candidate_t* candidate)
{
    if (retry->n_failed < DECODE_MAX_CANDIDATES) retry->failed[retry->n_failed++] = *candidate;
}

void decode_retry_note_subtracted(decode_retry_t* retry, const ftx_candidate_t* candidate)
{
    if (retry->n_subtracted < DECODE_MAX_CANDIDATES) {
        retry->subtracted_bin[retry->n_subtracted++] = candidate->freq_offset;
    } else {
        retry->subtracted_overflow = 1;
    }
}

int decode_retry_unchanged(const decode_retry_t* retry, const ftx_candidate_t* candidate,
                           int n_tones)
{
    if (retry->subtracted_overflow) return 0;
    // Both signals span n_tones bins from their freq_offset.
    for (int i = 0; i < retry->n_subtracted; ++i) {
        int distance = candidate->freq_offset - retry->subtracted_bin[i];
        if (distance < 0) distance = -distance;
        if (distance < n_tones) return 0;
    }
    for (int i = 0; i < retry->n_failed; ++i) {
        const ftx_candidate_t* f = &retry->failed[i];
        if (f->time_offset == candidate->time_offset && f->time_sub == candidate->time_sub &&
            f->freq_offset == candidate->freq_offset && f->freq_sub == candidate->freq_sub) {
            return 1;
        }
    }
    return 0;
}
