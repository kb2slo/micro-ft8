#include "monitor.h"
#include "common.h"

#define LOG_LEVEL LOG_INFO
#include "ft8/debug.h"

#include <stdlib.h>
#include <string.h>

static float hann_i(int i, int N)
{
    float x = sinf((float)M_PI * i / N);
    return x * x;
}

// static float hamming_i(int i, int N)
// {
//     const float a0 = (float)25 / 46;
//     const float a1 = 1 - a0;

//     float x1 = cosf(2 * (float)M_PI * i / N);
//     return a0 - a1 * x1;
// }

// static float blackman_i(int i, int N)
// {
//     const float alpha = 0.16f; // or 2860/18608
//     const float a0 = (1 - alpha) / 2;
//     const float a1 = 1.0f / 2;
//     const float a2 = alpha / 2;

//     float x1 = cosf(2 * (float)M_PI * i / N);
//     float x2 = 2 * x1 * x1 - 1; // Use double angle formula

//     return a0 - a1 * x1 + a2 * x2;
// }

static void waterfall_init(ftx_waterfall_t* me, int max_blocks, int num_bins, int time_osr, int freq_osr)
{
    me->max_blocks = max_blocks;
    me->num_blocks = 0;
    me->num_bins = num_bins;
    me->time_osr = time_osr;
    me->freq_osr = freq_osr;
    me->block_stride = (time_osr * freq_osr * num_bins);
}

void monitor_sizes(const monitor_config_t* cfg, monitor_sizes_t* sizes)
{
    float slot_time = (cfg->protocol == FTX_PROTOCOL_FT4) ? FT4_SLOT_TIME : FT8_SLOT_TIME;
    float symbol_period = (cfg->protocol == FTX_PROTOCOL_FT4) ? FT4_SYMBOL_PERIOD : FT8_SYMBOL_PERIOD;
    const int block_size = (int)(cfg->sample_rate * symbol_period);
    sizes->nfft = block_size * cfg->freq_osr;
    // Enough blocks to fit the entire FT8/FT4 slot
    const int max_blocks = (int)(slot_time / symbol_period);
    const int num_bins = ((int)(cfg->f_max * symbol_period) + 1) - (int)(cfg->f_min * symbol_period);
    sizes->mag_bytes = (size_t)max_blocks * cfg->time_osr * cfg->freq_osr * num_bins * sizeof(WF_ELEM_T);
    size_t fft_needed = 0;
    kiss_fftr_alloc(sizes->nfft, 0, NULL, &fft_needed);
    sizes->fft_work_bytes = fft_needed;
}

// Every buffer is the caller's (docs/architecture/memory.md): on the P4 one static set per channel in PSRAM,
// so a second monitor never shares the first's frame, waterfall or FFT scratch. A buffer smaller than
// this configuration needs refuses it, logging both sizes; nothing falls back to the heap.
void monitor_init_buffers(monitor_t* me, const monitor_config_t* cfg, const monitor_buffers_t* buffers)
{
    memset(me, 0, sizeof(*me));
    float slot_time = (cfg->protocol == FTX_PROTOCOL_FT4) ? FT4_SLOT_TIME : FT8_SLOT_TIME;
    float symbol_period = (cfg->protocol == FTX_PROTOCOL_FT4) ? FT4_SYMBOL_PERIOD : FT8_SYMBOL_PERIOD;
    // Compute DSP parameters that depend on the sample rate
    me->block_size = (int)(cfg->sample_rate * symbol_period); // samples corresponding to one FSK symbol
    me->subblock_size = me->block_size / cfg->time_osr;
    me->nfft = me->block_size * cfg->freq_osr;
    me->fft_norm = 2.0f / me->nfft;
    // const int len_window = 1.8f * me->block_size; // hand-picked and optimized

    monitor_sizes_t need;
    monitor_sizes(cfg, &need);
    // Allocate enough blocks to fit the entire FT8/FT4 slot in memory
    const int max_blocks = (int)(slot_time / symbol_period);
    // Keep only FFT bins in the specified frequency range (f_min/f_max)
    me->min_bin = (int)(cfg->f_min * symbol_period);
    me->max_bin = (int)(cfg->f_max * symbol_period) + 1;
    const int num_bins = me->max_bin - me->min_bin;

    LOG(LOG_INFO, "Block size = %d\n", me->block_size);
    LOG(LOG_INFO, "Subblock size = %d\n", me->subblock_size);

    if (buffers->nfft_max < me->nfft || buffers->mag_bytes < need.mag_bytes ||
        buffers->fft_work_bytes < need.fft_work_bytes)
    {
        LOG(LOG_ERROR, "Monitor needs nfft %d, waterfall %zu, FFT plan %zu bytes; buffers are %d, %zu, %zu\n",
            me->nfft, need.mag_bytes, need.fft_work_bytes, buffers->nfft_max, buffers->mag_bytes,
            buffers->fft_work_bytes);
        return;
    }
    if (fft_plan_init_with_buffer(&me->fft_plan, me->nfft, buffers->fft_work, buffers->fft_work_bytes) != 0)
    {
        LOG(LOG_ERROR, "FFT plan init failed (needed %zu)\n", need.fft_work_bytes);
        return;
    }

    waterfall_init(&me->wf, max_blocks, num_bins, cfg->time_osr, cfg->freq_osr);
    me->wf.mag = buffers->mag;
    me->wf.protocol = cfg->protocol;

    me->timedata = buffers->timedata;
    me->freqdata = buffers->freqdata;
    me->last_frame = buffers->last_frame;
    memset(me->last_frame, 0, sizeof(float) * me->nfft);
    me->window = buffers->window;
    for (int i = 0; i < me->nfft; ++i)
    {
        me->window[i] = me->fft_norm * hann_i(i, me->nfft);
    }

    LOG(LOG_INFO, "N_FFT = %d\n", me->nfft);

#ifdef WATERFALL_USE_PHASE
    me->nifft = 64; // Gives 200 Hz sample rate for FT8 (160ms symbol period)

    size_t ifft_work_size = 0;
    kiss_fft_alloc(me->nifft, 1, 0, &ifft_work_size);
    me->ifft_work = malloc(ifft_work_size);
    me->ifft_cfg = kiss_fft_alloc(me->nifft, 1, me->ifft_work, &ifft_work_size);

    LOG(LOG_INFO, "N_iFFT = %d\n", me->nifft);
    LOG(LOG_DEBUG, "iFFT work area = %zu\n", ifft_work_size);
#endif

    me->symbol_period = symbol_period;

    me->max_mag = -120.0f;
}

// Host tools run any configuration, so each buffer is malloc'd to size here and freed by monitor_free().
// Device code calls monitor_init_buffers() with static buffers instead.
void monitor_init(monitor_t* me, const monitor_config_t* cfg)
{
    monitor_sizes_t need;
    monitor_sizes(cfg, &need);
    monitor_buffers_t b = {
        .mag = (WF_ELEM_T*)malloc(need.mag_bytes),
        .mag_bytes = need.mag_bytes,
        .fft_work = (uint8_t*)malloc(need.fft_work_bytes),
        .fft_work_bytes = need.fft_work_bytes,
        .window = (float*)malloc(sizeof(float) * need.nfft),
        .last_frame = (float*)malloc(sizeof(float) * need.nfft),
        .timedata = (kiss_fft_scalar*)malloc(sizeof(kiss_fft_scalar) * need.nfft),
        .freqdata = (kiss_fft_cpx*)malloc(sizeof(kiss_fft_cpx) * (need.nfft / 2 + 1)),
        .nfft_max = need.nfft,
    };
    if (!b.mag || !b.fft_work || !b.window || !b.last_frame || !b.timedata || !b.freqdata)
    {
        LOG(LOG_ERROR, "Monitor alloc failed (waterfall %zu bytes)\n", need.mag_bytes);
        memset(me, 0, sizeof(*me));
    }
    else
    {
        monitor_init_buffers(me, cfg, &b);
    }
    if (me->wf.mag == NULL)
    {
        free(b.mag);
        free(b.fft_work);
        free(b.window);
        free(b.last_frame);
        free(b.timedata);
        free(b.freqdata);
        return;
    }
    me->owned = b;
}

void monitor_free(monitor_t* me)
{
    fft_plan_free(&me->fft_plan);
    free(me->owned.mag);
    free(me->owned.fft_work);
    free(me->owned.window);
    free(me->owned.last_frame);
    free(me->owned.timedata);
    free(me->owned.freqdata);
    memset(&me->owned, 0, sizeof(me->owned));
    me->wf.mag = NULL;
    me->window = NULL;
    me->last_frame = NULL;
    me->timedata = NULL;
    me->freqdata = NULL;
}

void monitor_reset(monitor_t* me)
{
    me->wf.num_blocks = 0;
    me->max_mag = -120.0f;
}

// Compute FFT magnitudes (log wf) for a frame in the signal and update waterfall data
void monitor_process(monitor_t* me, const float* frame)
{
    // Check if we can still store more waterfall data
    if (me->wf.num_blocks >= me->wf.max_blocks)
        return;

    int offset = me->wf.num_blocks * me->wf.block_stride;
    int frame_pos = 0;

    // This monitor's FFT scratch, so monitors on different tasks do not share it
    kiss_fft_scalar* timedata = me->timedata;
    kiss_fft_cpx* freqdata = me->freqdata;

    // Loop over block subdivisions
    for (int time_sub = 0; time_sub < me->wf.time_osr; ++time_sub)
    {
        // Shift the new data into analysis frame
        for (int pos = 0; pos < me->nfft - me->subblock_size; ++pos)
        {
            me->last_frame[pos] = me->last_frame[pos + me->subblock_size];
        }
        for (int pos = me->nfft - me->subblock_size; pos < me->nfft; ++pos)
        {
            me->last_frame[pos] = frame[frame_pos];
            ++frame_pos;
        }

        // Do DFT of windowed analysis frame
        for (int pos = 0; pos < me->nfft; ++pos)
        {
            timedata[pos] = me->window[pos] * me->last_frame[pos];
        }
        fft_execute(&me->fft_plan, timedata, freqdata);

        // Loop over possible frequency OSR offsets
        for (int freq_sub = 0; freq_sub < me->wf.freq_osr; ++freq_sub)
        {
            for (int bin = me->min_bin; bin < me->max_bin; ++bin)
            {
                int src_bin = (bin * me->wf.freq_osr) + freq_sub;
                float mag2 = (freqdata[src_bin].i * freqdata[src_bin].i) + (freqdata[src_bin].r * freqdata[src_bin].r);
                float db = 10.0f * log10f(1E-12f + mag2);

#ifdef WATERFALL_USE_PHASE
                // Save the magnitude in dB and phase in radians
                float phase = atan2f(freqdata[src_bin].i, freqdata[src_bin].r);
                me->wf.mag[offset].mag = db;
                me->wf.mag[offset].phase = phase;
#else
                // Scale decibels to unsigned 8-bit range and clamp the value
                // Range 0-240 covers -120..0 dB in 0.5 dB steps
                int scaled = (int)(2 * db + 240);
                me->wf.mag[offset] = (scaled < 0) ? 0 : ((scaled > 255) ? 255 : scaled);
#endif
                ++offset;

                if (db > me->max_mag)
                    me->max_mag = db;
            }
        }
    }

    ++me->wf.num_blocks;
}

#ifdef WATERFALL_USE_PHASE
void monitor_resynth(const monitor_t* me, const candidate_t* candidate, float* signal)
{
    const int num_ifft = me->nifft;
    const int num_shift = num_ifft / 2;
    const int taper_width = 4;
    const int num_tones = 8;

    // Starting offset is 3 subblocks due to analysis buffer loading
    int offset = 1;                          // candidate->time_offset;
    offset = (offset * me->wf.time_osr) + 1; // + candidate->time_sub;
    offset = (offset * me->wf.freq_osr);     // + candidate->freq_sub;
    offset = (offset * me->wf.num_bins);     // + candidate->freq_offset;

    WF_ELEM_T* el = me->wf.mag + offset;

    // DFT frequency data - initialize to zero
    kiss_fft_cpx freqdata[num_ifft];
    for (int i = 0; i < num_ifft; ++i)
    {
        freqdata[i].r = 0;
        freqdata[i].i = 0;
    }

    int pos = 0;
    for (int num_block = 1; num_block < me->wf.num_blocks; ++num_block)
    {
        // Extract frequency data around the selected candidate only
        for (int i = candidate->freq_offset - taper_width - 1; i < candidate->freq_offset + 8 + taper_width - 1; ++i)
        {
            if ((i >= 0) && (i < me->wf.num_bins))
            {
                int tgt_bin = (me->wf.freq_osr * (i - candidate->freq_offset) + num_ifft) % num_ifft;
                float weight = 1.0f;
                if (i < candidate->freq_offset)
                {
                    weight = ((i - candidate->freq_offset) + taper_width) / (float)taper_width;
                }
                else if (i > candidate->freq_offset + 7)
                {
                    weight = ((candidate->freq_offset + 7 - i) + taper_width) / (float)taper_width;
                }

                // Convert (dB magnitude, phase) to (real, imaginary)
                float mag = powf(10.0f, el[i].mag / 20) / 2 * weight;
                freqdata[tgt_bin].r = mag * cosf(el[i].phase);
                freqdata[tgt_bin].i = mag * sinf(el[i].phase);

                int i2 = i + me->wf.num_bins;
                tgt_bin = (tgt_bin + 1) % num_ifft;
                float mag2 = powf(10.0f, el[i2].mag / 20) / 2 * weight;
                freqdata[tgt_bin].r = mag2 * cosf(el[i2].phase);
                freqdata[tgt_bin].i = mag2 * sinf(el[i2].phase);
            }
        }

        // Compute inverse DFT and overlap-add the waveform
        kiss_fft_cpx timedata[num_ifft];
        kiss_fft(me->ifft_cfg, freqdata, timedata);
        for (int i = 0; i < num_ifft; ++i)
        {
            signal[pos + i] += timedata[i].i;
        }

        // Move to the next symbol
        el += me->wf.block_stride;
        pos += num_shift;
    }
}
#endif
