#ifndef _INCLUDE_MONITOR_H_
#define _INCLUDE_MONITOR_H_

#ifdef __cplusplus
extern "C"
{
#endif

#include <stddef.h>

#include "ft8/decode.h"
#include "fft_wrapper.h"

/// Configuration options for FT4/FT8 monitor
typedef struct
{
    float f_min;             ///< Lower frequency bound for analysis
    float f_max;             ///< Upper frequency bound for analysis
    int sample_rate;         ///< Sample rate in Hertz
    int time_osr;            ///< Number of time subdivisions
    int freq_osr;            ///< Number of frequency subdivisions
    ftx_protocol_t protocol; ///< Protocol: FT4 or FT8
} monitor_config_t;

/// The buffers one monitor works in (docs/architecture/memory.md). On a device, one static set per
/// monitor, at least monitor_sizes() large; window, last_frame and timedata hold nfft_max values,
/// freqdata nfft_max / 2 + 1. fft_work is 16-byte aligned.
typedef struct
{
    WF_ELEM_T* mag;
    size_t mag_bytes;
    uint8_t* fft_work;
    size_t fft_work_bytes;
    float* window;
    float* last_frame;
    kiss_fft_scalar* timedata;
    kiss_fft_cpx* freqdata;
    int nfft_max;
} monitor_buffers_t;

/// What a configuration needs of monitor_buffers_t.
typedef struct
{
    int nfft;
    size_t mag_bytes;
    size_t fft_work_bytes;
} monitor_sizes_t;

/// kiss_fftr_alloc()'s plan size at 32-bit float, for sizing a static fft_work: 3 pointers +
/// kiss_fft_state (272) + (nfft/2-1) twiddles + (nfft/2)*3/2 super-twiddles, each cpx 8 bytes, +32 slack.
/// monitor_init_buffers() checks the real size.
#define MONITOR_FFT_WORK_BYTES(nfft) (12 + (272 + 8 * ((nfft) / 2 - 1)) + 8 * (((nfft) / 2) * 3 / 2) + 32)

/// FT4/FT8 monitor object that manages DSP processing of incoming audio data
/// and prepares a waterfall object
typedef struct
{
    float symbol_period; ///< FT4/FT8 symbol period in seconds
    int min_bin;         ///< First FFT bin in the frequency range (begin)
    int max_bin;         ///< First FFT bin outside the frequency range (end)
    int block_size;      ///< Number of samples per symbol (block)
    int subblock_size;   ///< Analysis shift size (number of samples)
    int nfft;            ///< FFT size
    float fft_norm;      ///< FFT normalization factor
    float* window;       ///< Window function for STFT analysis (nfft samples)
    float* last_frame;   ///< Current STFT analysis frame (nfft samples)
    kiss_fft_scalar* timedata; ///< FFT input scratch (nfft)
    kiss_fft_cpx* freqdata;    ///< FFT output scratch (nfft / 2 + 1)
    monitor_buffers_t owned;   ///< What monitor_init() allocated, freed by monitor_free(); zero otherwise
    ftx_waterfall_t wf;  ///< Waterfall object
    float max_mag;       ///< Maximum detected magnitude (debug stats)

    // FFT housekeeping variables
    fft_plan_t fft_plan;   ///< FFT plan wrapper
#ifdef WATERFALL_USE_PHASE
    int nifft;             ///< iFFT size
    void* ifft_work;       ///< Work area required by inverse Kiss FFT
    kiss_fft_cfg ifft_cfg; ///< Inverse Kiss FFT housekeeping object
#endif
} monitor_t;

void monitor_sizes(const monitor_config_t* cfg, monitor_sizes_t* sizes);
/// Device: works in the caller's buffers. On refusal (a buffer too small, logged with both sizes) wf.mag is NULL.
void monitor_init_buffers(monitor_t* me, const monitor_config_t* cfg, const monitor_buffers_t* buffers);
/// Host tools: mallocs each buffer to size; monitor_free() frees them. wf.mag is NULL on failure.
void monitor_init(monitor_t* me, const monitor_config_t* cfg);
void monitor_reset(monitor_t* me);
void monitor_process(monitor_t* me, const float* frame);
void monitor_free(monitor_t* me);

#ifdef WATERFALL_USE_PHASE
void monitor_resynth(const monitor_t* me, const candidate_t* candidate, float* signal);
#endif

#ifdef __cplusplus
}
#endif

#endif // _INCLUDE_MONITOR_H_
