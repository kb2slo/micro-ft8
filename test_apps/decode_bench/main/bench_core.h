#pragma once

// Decode benchmark core (RFC 0006 §5 P0, B87). Runs the firmware's decode steps on one 15 s slot of
// 6 kHz audio: per-block AGC and monitor_process() as ft8_audio_pipeline_run() does, then
// ftx_find_candidates() / ftx_decode_candidate() / ftx_message_decode() as decode_monitor_results()
// does. Portable C: the same file builds into the device image and the host parity binary.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BENCH_SAMPLE_RATE 6000

typedef struct {
    const char* name;
    int time_osr;
    int freq_osr;
    int max_candidates;   // decode_monitor_results(): max_cand
    int max_decodes;      // DEC_MAX (RX list size)
    int min_score;        // ftx_find_candidates() argument
    int ldpc_iterations;  // ftx_decode_candidate() argument
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
typedef void (*bench_message_fn)(void* context, const char* text, float freq_hz, float time_s);

// Decode one slot. `samples` is 6 kHz mono int16 starting at the slot boundary.
void bench_run_slot(const BenchConfig* config, const int16_t* samples, size_t sample_count,
                    bench_clock_fn clock_us, bench_message_fn on_message, void* context,
                    BenchResult* result);

#ifdef __cplusplus
}
#endif
