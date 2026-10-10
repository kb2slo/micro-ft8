#pragma once

// Decode benchmark core (docs/architecture/decode.md). Runs the firmware's decode steps on one slot:
// per-block AGC and monitor_process() as ft8_audio_pipeline_run() does, then ftx_find_candidates() /
// ftx_decode_candidate() / ftx_message_decode() as decode_monitor_results() does. Portable C: the same
// file builds into the device image, the host parity binary, and tools/decode_recall.
//
// A zero in sample_rate, agc, or mag_levels means the firmware default (6 kHz, AGC on, 8-bit cells),
// so the device benchmark's existing initialisers keep today's behaviour.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BENCH_SAMPLE_RATE 6000
#define BENCH_MAX_CANDIDATES 256

typedef struct {
    const char* name;
    int time_osr;
    int freq_osr;
    int max_candidates;   // decode_monitor_results(): max_cand
    int max_decodes;      // DEC_MAX (RX list size)
    int min_score;        // ftx_find_candidates() argument
    int ldpc_iterations;  // ftx_decode_candidate() argument
    int sample_rate;      // 0 = 6000, the firmware's FT8_SAMPLE_RATE
    int agc;              // 0 = on, 2 = off. Any other value is on
    int mag_levels;       // 0 or >= 256 = stored 8-bit cells. Else quantise onto this many levels
    int subtract_passes;  // 0 = one search, no subtraction. 1..3 re-search that many times
    int subtract_neighbors; // 0 = the decoded time_sub only. 1 = that and the neighbouring time_sub
    float subtract_fraction; // 0 = half the estimated SNR. Otherwise the fraction removed, in (0, 1]
} BenchConfig;

typedef struct {
    int ok;               // 0 if the monitor could not be set up (e.g. allocation failed)
    int candidates;
    int decoded;
    int64_t process_us;   // all monitor_process() calls: capture-time CPU, spread across the slot
    int64_t find_us;      // ftx_find_candidates(): on the decode clock
    int64_t decode_us;    // candidate loop incl. unpack: on the decode clock
} BenchResult;

typedef int64_t (*bench_clock_fn)(void);
typedef void (*bench_message_fn)(void* context, const char* text, float freq_hz, float time_s, int snr);

// Decode one slot. `samples` is mono int16 at `config->sample_rate` (or 6 kHz when that is 0),
// starting at the slot boundary. `snr` is decode_snr_message()'s, as the firmware reports it, for the jt9-style line.
void bench_run_slot(const BenchConfig* config, const int16_t* samples, size_t sample_count,
                    bench_clock_fn clock_us, bench_message_fn on_message, void* context,
                    BenchResult* result);

#ifdef __cplusplus
}
#endif
