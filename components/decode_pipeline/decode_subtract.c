#include <math.h>
#include <stddef.h>

#include "ft8/constants.h"
#include "ft8/encode.h"

#include "decode_subtract.h"

static int protocol_num_tones(ftx_protocol_t protocol)
{
    switch (protocol) {
        case FTX_PROTOCOL_FT4:
            return 4;
        case FTX_PROTOCOL_FT8:
            return 8;
    }
    return 8;
}

static int protocol_num_symbols(ftx_protocol_t protocol)
{
    switch (protocol) {
        case FTX_PROTOCOL_FT4:
            return FT4_NN;
        case FTX_PROTOCOL_FT8:
            return FT8_NN;
    }
    return FT8_NN;
}

// mag layout is [block][time_sub][freq_sub][bin].
static uint8_t* tone_row(ftx_waterfall_t* wf, int block, int time_sub, int freq_sub, int freq_offset)
{
    size_t index = (size_t)block * (size_t)wf->block_stride;
    index += (size_t)time_sub * (size_t)wf->freq_osr * (size_t)wf->num_bins;
    index += (size_t)freq_sub * (size_t)wf->num_bins;
    index += (size_t)freq_offset;
    return wf->mag + index;
}

// Lower the decoded tone's bin. SNR is the mean tone level minus the mean of the
// per-symbol minimum of the other tones. That minimum is biased low, which is why
// the fraction stays at a half.
static void subtract_time_sub(ftx_waterfall_t* wf, const ftx_candidate_t* candidate,
                              const uint8_t* tones, int n_tones, int time_sub, float fraction)
{
    const int n_bins = protocol_num_tones(wf->protocol);
    for (int freq_sub = 0; freq_sub < wf->freq_osr; ++freq_sub) {
        float noise = 0.0f;
        float signal = 0.0f;
        int count = 0;
        for (int i = 0; i < n_tones; ++i) {
            int block = candidate->time_offset + i;
            if (block < 0 || block >= wf->num_blocks) continue;
            int tone = tones[i];
            if (tone < 0 || tone >= n_bins) continue;
            uint8_t* row = tone_row(wf, block, time_sub, freq_sub, candidate->freq_offset);
            float noise_val = 1000.0f;
            for (int s = 0; s < n_bins; ++s) {
                if (s == tone) continue;
                float mag = (float)row[s] * 0.5f - 120.0f;
                if (mag < noise_val) noise_val = mag;
            }
            noise += noise_val;
            signal += (float)row[tone] * 0.5f - 120.0f;
            ++count;
        }
        if (count == 0) continue;
        float snr = signal / (float)count - noise / (float)count;
        if (snr <= 0.0f) continue;
        float delta = fraction * snr * 2.0f;
        for (int i = 0; i < n_tones; ++i) {
            int block = candidate->time_offset + i;
            if (block < 0 || block >= wf->num_blocks) continue;
            int tone = tones[i];
            if (tone < 0 || tone >= n_bins) continue;
            uint8_t* row = tone_row(wf, block, time_sub, freq_sub, candidate->freq_offset);
            int next = (int)lrintf((float)row[tone] - delta);
            if (next < 0) next = 0;
            if (next > 255) next = 255;
            row[tone] = (uint8_t)next;
        }
    }
}

int decode_subtract_already(const float* seen_hz, int seen, float freq_hz)
{
    for (int i = 0; i < seen; ++i) {
        float distance = freq_hz - seen_hz[i];
        if (distance < 0.0f) distance = -distance;
        if (distance < DECODE_SUBTRACT_FREQ_SKIP_HZ) return 1;
    }
    return 0;
}

void decode_subtract_message(ftx_waterfall_t* wf, const ftx_candidate_t* candidate,
                             const uint8_t* payload, int neighbors, float fraction)
{
    uint8_t tones[FT4_NN];
    int n_tones = protocol_num_symbols(wf->protocol);
    switch (wf->protocol) {
        case FTX_PROTOCOL_FT4:
            ft4_encode(payload, tones);
            break;
        case FTX_PROTOCOL_FT8:
            ft8_encode(payload, tones);
            break;
    }
    int radius = neighbors ? 1 : 0;
    for (int delta = -radius; delta <= radius; ++delta) {
        int time_sub = (int)candidate->time_sub + delta;
        if (time_sub < 0 || time_sub >= wf->time_osr) continue;
        subtract_time_sub(wf, candidate, tones, n_tones, time_sub, fraction);
    }
}
