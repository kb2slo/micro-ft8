#include "monitor.h"
#include "common.h"
#include "decode_psram.h"

#define LOG_LEVEL LOG_INFO
#include "ft8/debug.h"

#include <stdlib.h>
#include <string.h>

// Maximum nfft for static buffer sizing.
// FT8 at 6 kHz, freq_osr=1: nfft = 6000 * 0.160 * 1 = 960
// freq_osr=2 needs 1920. The P4 build sets that (poe_p4/CMakeLists.txt). The
// default suits tx_e2e, which decodes at freq_osr=1.
#ifndef MONITOR_NFFT_MAX
#define MONITOR_NFFT_MAX 960
#endif

static float* window_buf = NULL;
static float* last_frame_buf = NULL;
static WF_ELEM_T* waterfall_mag_buf = NULL;
static uint8_t* fft_work_buf = NULL;
static bool fft_work_is_static = false;

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
    me->mag = waterfall_mag_buf;
}

// The waterfall (docs/architecture/memory.md). A device build sets MONITOR_WF_STATIC_BYTES to
// the largest waterfall it runs, and the waterfall is that one static buffer: the linker places
// it or the build fails, and heap fragmentation cannot take it away. On the P4 it lives in
// PSRAM (DECODE_PSRAM_BSS / row p3x6); DMA buffers never do. A configuration that needs more
// is refused, never moved to the heap; a silent fallback is how a static buffer once sat unused
// in the image. Host tools run any configuration, so without the define the waterfall is
// malloc'd to size and freed by monitor_free().
#ifdef MONITOR_WF_STATIC_BYTES
static DECODE_PSRAM_BSS WF_ELEM_T waterfall_static_buf[MONITOR_WF_STATIC_BYTES / sizeof(WF_ELEM_T)];
#endif

static void waterfall_free(ftx_waterfall_t* me)
{
    if (me && me->mag)
    {
#ifndef MONITOR_WF_STATIC_BYTES
        free(me->mag);
#endif
        me->mag = NULL;
    }
}

void monitor_init(monitor_t* me, const monitor_config_t* cfg)
{
    float slot_time = (cfg->protocol == FTX_PROTOCOL_FT4) ? FT4_SLOT_TIME : FT8_SLOT_TIME;
    float symbol_period = (cfg->protocol == FTX_PROTOCOL_FT4) ? FT4_SYMBOL_PERIOD : FT8_SYMBOL_PERIOD;
    // Compute DSP parameters that depend on the sample rate
    me->block_size = (int)(cfg->sample_rate * symbol_period); // samples corresponding to one FSK symbol
    me->subblock_size = me->block_size / cfg->time_osr;
    me->nfft = me->block_size * cfg->freq_osr;
    me->fft_norm = 2.0f / me->nfft;
    // const int len_window = 1.8f * me->block_size; // hand-picked and optimized

    // Allocate enough blocks to fit the entire FT8/FT4 slot in memory
    const int max_blocks = (int)(slot_time / symbol_period);
    // Keep only FFT bins in the specified frequency range (f_min/f_max)
    me->min_bin = (int)(cfg->f_min * symbol_period);
    me->max_bin = (int)(cfg->f_max * symbol_period) + 1;
    const int num_bins = me->max_bin - me->min_bin;

    LOG(LOG_INFO, "Block size = %d\n", me->block_size);
    LOG(LOG_INFO, "Subblock size = %d\n", me->subblock_size);

    // FFT work buffer in BSS, not the heap: long-lived, so static
    // (docs/architecture/memory.md). kiss_fftr_alloc's size, 32-bit float:
    // 3 pointers + kiss_fft_state (272) + (nfft/2-1) twiddles +
    // (nfft/2)*3/2 super-twiddles, each cpx 8 bytes. +32 for slack.
    // Sized from MONITOR_NFFT_MAX.
#define KISS_FFTR_PLAN_BYTES(nfft) \
    (12 + (272 + 8 * ((nfft) / 2 - 1)) + 8 * (((nfft) / 2) * 3 / 2) + 32)
    static DECODE_PSRAM_BSS uint8_t fft_work_static[KISS_FFTR_PLAN_BYTES(MONITOR_NFFT_MAX)]
        __attribute__((aligned(16)));
    size_t fft_needed = 0;
    kiss_fftr_alloc(me->nfft, 0, NULL, &fft_needed);
    if (fft_needed <= sizeof(fft_work_static))
    {
        fft_work_buf = fft_work_static;
        fft_work_is_static = true;
        LOG(LOG_INFO, "FFT plan using static buffer (%zu / %zu bytes)\n",
            fft_needed, sizeof(fft_work_static));
    }
    else
    {
        fft_work_buf = (uint8_t*)malloc(fft_needed);
        fft_work_is_static = false;
    }
    if (fft_work_buf == NULL || fft_plan_init_with_buffer(&me->fft_plan, me->nfft, fft_work_buf, fft_needed) != 0)
    {
        LOG(LOG_ERROR, "FFT plan init failed (needed %zu)\n", fft_needed);
        return;
    }

    int block_stride = cfg->time_osr * cfg->freq_osr * num_bins;
    size_t wf_bytes = (size_t)max_blocks * block_stride * sizeof(WF_ELEM_T);
#ifdef MONITOR_WF_STATIC_BYTES
    if (wf_bytes > sizeof(waterfall_static_buf))
    {
        LOG(LOG_ERROR, "Waterfall needs %zu bytes, static buffer is %zu\n", wf_bytes, sizeof(waterfall_static_buf));
        monitor_free(me);
        return;
    }
    waterfall_mag_buf = waterfall_static_buf;
#else
    waterfall_mag_buf = (WF_ELEM_T*)malloc(wf_bytes);
    if (waterfall_mag_buf == NULL)
    {
        LOG(LOG_ERROR, "Waterfall alloc failed (%zu bytes)\n", wf_bytes);
        monitor_free(me);
        return;
    }
#endif

    waterfall_init(&me->wf, max_blocks, num_bins, cfg->time_osr, cfg->freq_osr);
    me->wf.mag = waterfall_mag_buf;
    me->wf.protocol = cfg->protocol;

    // Static window and analysis frame — avoids heap fragmentation
    static DECODE_PSRAM_BSS float window_static[MONITOR_NFFT_MAX];
    static DECODE_PSRAM_BSS float last_frame_static[MONITOR_NFFT_MAX];
    if (me->nfft > MONITOR_NFFT_MAX)
    {
        LOG(LOG_ERROR, "Monitor nfft %d exceeds static buffer %d\n", me->nfft, MONITOR_NFFT_MAX);
        monitor_free(me);
        return;
    }
    window_buf = window_static;
    last_frame_buf = last_frame_static;
    memset(last_frame_buf, 0, sizeof(float) * me->nfft);

    me->window = window_buf;
    for (int i = 0; i < me->nfft; ++i)
    {
        me->window[i] = me->fft_norm * hann_i(i, me->nfft);
    }
    me->last_frame = last_frame_buf;

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

void monitor_free(monitor_t* me)
{
    waterfall_free(&me->wf);
    fft_plan_free(&me->fft_plan);
    // window_buf and last_frame_buf are static arrays — just NULL the pointers
    window_buf = NULL;
    last_frame_buf = NULL;
    if (fft_work_buf && !fft_work_is_static) {
        free(fft_work_buf);
    }
    fft_work_buf = NULL;
    fft_work_is_static = false;
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

    // Static FFT work buffers — avoids heap allocation in the hot path
    static DECODE_PSRAM_BSS kiss_fft_scalar timedata[MONITOR_NFFT_MAX];
    static DECODE_PSRAM_BSS kiss_fft_cpx   freqdata[MONITOR_NFFT_MAX / 2 + 1];
    if (me->nfft > MONITOR_NFFT_MAX)
    {
        LOG(LOG_ERROR, "nfft %d exceeds static buffer %d\n", me->nfft, MONITOR_NFFT_MAX);
        return;
    }

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
