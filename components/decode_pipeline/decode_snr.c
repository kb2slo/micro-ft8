#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "ft8/constants.h"
#include "ft8/encode.h"

#include "decode_snr.h"

// Widest percentile window in columns: +/-400 Hz at freq_osr 2. A finer freq_osr gets a narrower window.
#define WINDOW_MAX 257

// Waterfall code v is 0.5 dB steps from -120 dB at 0 (monitor_process()). Linear power per code.
static float s_code_power[256];
static int s_code_power_ready;

static void fill_code_power(void)
{
    if (s_code_power_ready) return;
    for (int v = 0; v < 256; ++v) {
        s_code_power[v] = powf(10.0f, ((float)v - 240.0f) / 20.0f);
    }
    s_code_power_ready = 1;
}

// The frame behind time cell t (block * time_osr + time_sub) is the frame_subblocks subblocks ending at
// subblock t. Its noise was scaled by each subblock's block gain, weighted by the window. Returns the factor
// that undoes that, or 0 for a frame that reaches back before the slot: on the device it holds the previous
// slot's audio and on the host zeros, so neither is measured.
static float cell_scale(const decode_snr_slot_t* slot, const ftx_waterfall_t* wf, int t)
{
    const int first = t - slot->frame_subblocks + 1;
    if (first < 0) return 0.0f;
    float g2 = 0.0f;
    for (int q = 0; q < slot->frame_subblocks; ++q) {
        const float gain = slot->block_gain[(first + q) / wf->time_osr];
        g2 += slot->frame_weight[q] * gain * gain;
    }
    return (g2 > 1e-20f) ? 1.0f / g2 : 0.0f;
}

static size_t cell_index(const ftx_waterfall_t* wf, int block, int time_sub, int freq_sub, int bin)
{
    size_t index = (size_t)block * (size_t)wf->block_stride;
    index += (size_t)time_sub * (size_t)wf->freq_osr * (size_t)wf->num_bins;
    index += (size_t)freq_sub * (size_t)wf->num_bins;
    return index + (size_t)bin;
}

int decode_snr_prepare(decode_snr_slot_t* slot, const monitor_t* monitor, const float* block_gain)
{
    const ftx_waterfall_t* wf = &monitor->wf;
    slot->frame_subblocks = 0;
    slot->num_columns = 0;
    slot->block_gain = block_gain;
    const int num_columns = wf->num_bins * wf->freq_osr;
    if (block_gain == NULL || wf->mag == NULL || monitor->window == NULL) return 0;
    if (num_columns <= 0 || num_columns > DECODE_SNR_MAX_COLUMNS) return 0;
    if (monitor->subblock_size <= 0 || monitor->nfft % monitor->subblock_size != 0) return 0;
    const int subblocks = monitor->nfft / monitor->subblock_size;
    if (subblocks < 1 || subblocks > DECODE_SNR_MAX_FRAME_SUBBLOCKS) return 0;
    fill_code_power();

    float total = 0.0f;
    for (int q = 0; q < subblocks; ++q) {
        float part = 0.0f;
        for (int n = q * monitor->subblock_size; n < (q + 1) * monitor->subblock_size; ++n) {
            part += monitor->window[n] * monitor->window[n];
        }
        slot->frame_weight[q] = part;
        total += part;
    }
    if (total <= 0.0f) return 0;
    for (int q = 0; q < subblocks; ++q) slot->frame_weight[q] /= total;
    slot->frame_subblocks = subblocks;

    // Columns a chunk at a time, time cells inside, so each read walks one row of the waterfall.
    const int row = wf->freq_osr * wf->num_bins;
    const int t_count = wf->num_blocks * wf->time_osr;
    for (int start = 0; start < row; start += 64) {
        const int width = (row - start < 64) ? row - start : 64;
        float sum[64] = { 0 };
        int cells = 0;
        for (int t = 0; t < t_count; ++t) {
            const float scale = cell_scale(slot, wf, t);
            if (scale <= 0.0f) continue;
            const uint8_t* cell = wf->mag + (size_t)t * (size_t)row + (size_t)start;
            for (int i = 0; i < width; ++i) sum[i] += s_code_power[cell[i]] * scale;
            ++cells;
        }
        for (int i = 0; i < width; ++i) {
            const int freq_sub = (start + i) / wf->num_bins;
            const int bin = (start + i) % wf->num_bins;
            const float mean = (cells > 0) ? sum[i] / (float)cells : 1e-12f;
            slot->column_cdb[bin * wf->freq_osr + freq_sub] = (int16_t)lrintf(1000.0f * log10f(mean + 1e-30f));
        }
    }
    slot->num_columns = num_columns;
    return 1;
}

// k-th smallest of values[0..n) by quickselect; reorders values.
static int16_t select_kth(int16_t* values, int n, int k)
{
    int lo = 0;
    int hi = n - 1;
    while (lo < hi) {
        const int16_t pivot = values[(lo + hi) / 2];
        int i = lo;
        int j = hi;
        while (i <= j) {
            while (values[i] < pivot) ++i;
            while (values[j] > pivot) --j;
            if (i <= j) {
                const int16_t swap = values[i];
                values[i] = values[j];
                values[j] = swap;
                ++i;
                --j;
            }
        }
        if (k <= j) {
            hi = j;
        } else if (k >= i) {
            lo = i;
        } else {
            break;
        }
    }
    return values[k];
}

// Linear power of the noise baseline at one column: the percentile, interpolated between neighbouring ranks,
// of the column means within DECODE_SNR_BASELINE_HZ.
static float baseline_power(const decode_snr_slot_t* slot, const monitor_t* monitor, int column)
{
    int half = (int)lrintf(DECODE_SNR_BASELINE_HZ * monitor->symbol_period * (float)monitor->wf.freq_osr);
    if (2 * half + 1 > WINDOW_MAX) half = (WINDOW_MAX - 1) / 2;
    const int first = (column - half < 0) ? 0 : column - half;
    const int last = (column + half >= slot->num_columns) ? slot->num_columns - 1 : column + half;
    int16_t window[WINDOW_MAX];
    const int n = last - first + 1;
    for (int i = 0; i < n; ++i) window[i] = slot->column_cdb[first + i];

    const float rank = (float)(n - 1) * (float)DECODE_SNR_BASELINE_PERCENT / 100.0f;
    const int below = (int)rank;
    const float low = (float)select_kth(window, n, below);
    // After the select everything right of `below` is at least as large; the next rank is their minimum.
    float high = low;
    if (below + 1 < n && rank > (float)below) {
        int16_t next = window[below + 1];
        for (int i = below + 2; i < n; ++i) {
            if (window[i] < next) next = window[i];
        }
        high = (float)next;
    }
    const float cdb = low + (high - low) * (rank - (float)below);
    return powf(10.0f, cdb / 1000.0f);
}

int decode_snr_message(const decode_snr_slot_t* slot, const monitor_t* monitor,
                       const ftx_candidate_t* candidate, const uint8_t* payload)
{
    const ftx_waterfall_t* wf = &monitor->wf;
    if (slot->frame_subblocks == 0 || slot->num_columns == 0) return DECODE_SNR_FLOOR_DB;

    uint8_t tones[FT4_NN];
    int n_symbols = FT8_NN;
    int n_tones = 8;
    float offset_db = DECODE_SNR_FT8_OFFSET_DB;
    switch (wf->protocol) {
        case FTX_PROTOCOL_FT4:
            ft4_encode(payload, tones);
            n_symbols = FT4_NN;
            n_tones = 4;
            // Not fitted: FT8's constant moved by the wider noise bandwidth of FT4's shorter symbol (row xph7).
            offset_db = DECODE_SNR_FT8_OFFSET_DB + 10.0f * log10f(FT8_SYMBOL_PERIOD / FT4_SYMBOL_PERIOD);
            break;
        case FTX_PROTOCOL_FT8:
            ft8_encode(payload, tones);
            break;
    }

    // Signal: mean power in the decoded tone's cell, over every symbol the waterfall holds.
    float signal = 0.0f;
    int count = 0;
    for (int k = 0; k < n_symbols; ++k) {
        const int block = candidate->time_offset + k;
        if (block < 0 || block >= wf->num_blocks) continue;
        const int bin = candidate->freq_offset + tones[k];
        if (bin < 0 || bin >= wf->num_bins) continue;
        const float scale = cell_scale(slot, wf, block * wf->time_osr + candidate->time_sub);
        if (scale <= 0.0f) continue;
        signal += s_code_power[wf->mag[cell_index(wf, block, candidate->time_sub, candidate->freq_sub, bin)]] * scale;
        ++count;
    }

    // Noise: the baseline at the middle of the signal's tones. Over a +/-400 Hz window it barely moves
    // across the signal's 50 Hz, and one select per message instead of one per tone is 8x cheaper.
    int column = (candidate->freq_offset + n_tones / 2) * wf->freq_osr + candidate->freq_sub;
    if (column < 0) column = 0;
    if (column >= slot->num_columns) column = slot->num_columns - 1;
    const float noise = baseline_power(slot, monitor, column);
    if (count == 0 || noise <= 0.0f) return DECODE_SNR_FLOOR_DB;

    const float excess = (signal / (float)count) / noise - 1.0f;
    const float snr = 10.0f * log10f(excess > 1e-3f ? excess : 1e-3f) + offset_db;
    const long rounded = lroundf(snr);
    if (rounded < DECODE_SNR_FLOOR_DB) return DECODE_SNR_FLOOR_DB;
    if (rounded > DECODE_SNR_CEILING_DB) return DECODE_SNR_CEILING_DB;
    return (int)rounded;
}
