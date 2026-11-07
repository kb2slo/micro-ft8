#pragma once

// Decode benchmark core (docs/architecture/decode.md). Runs the firmware's decode on one slot: per-block
// AGC and monitor_process() as ft8_audio_pipeline_run() does, then the firmware's own decode
// (components/decode_pipeline/decode_core.c, which decode_slot() drives too). Portable C: the same file
// builds into the device image, the host parity binary, and tools/decode_recall.
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
    int max_candidates;   // decode_slot(): max_cand
    int max_decodes;      // DEC_MAX (RX list size)
    int min_score;        // ftx_find_candidates() argument
    int ldpc_iterations;  // ftx_decode_candidate() argument
    int sample_rate;      // 0 = 6000, the firmware's FT8_SAMPLE_RATE
    int agc;              // 0 = on, 2 = off. Any other value is on
    int mag_levels;       // 0 or >= 256 = stored 8-bit cells. Else quantise onto this many levels
    int subtract_passes;  // 0 = one search, no subtraction. 1..3 re-search that many times
    int subtract_neighbors; // 0 = the decoded time_sub only. 1 = that and the neighbouring time_sub
    float subtract_fraction; // 0 = half the estimated SNR. Otherwise the fraction removed, in (0, 1]
    int budget_units;     // decode_core_config_t: 0 = no limit
    int cost_search;      // decode_core_config_t
    int cost_candidate;   // decode_core_config_t
    // 0 = each candidate decoded just before its commit, as decode_slot() does. N = each decoded N
    // candidates ahead of its commit, on the waterfall as the commits so far left it: what N + 1 workers
    // sharing a search do (decode_core.h). The output does not change; `redone` and `wasted` do.
    // BENCH_LOOKAHEAD_PASS = every candidate decoded at the start of its search, the farthest ahead.
    int lookahead;
    // 0 or 1 = one worker. 2 = a helper thread decodes the next candidate ahead (decode_workers.h), on the
    // other core from the caller's on the P4. Ignored when lookahead is set.
    int workers;
    // 0 = FT8, the station's mode; 1 = FT4: 7.5 s periods, decoded once its 105 symbols are in, as the
    // firmware's channel triggers on its mode's total_symbols (docs/roadmap/several-channels.md, an8x).
    int ft4;
} BenchConfig;

#define BENCH_LOOKAHEAD_PASS BENCH_MAX_CANDIDATES

typedef struct {
    int ok;               // 0 if the monitor could not be set up (e.g. allocation failed)
    int candidates;
    int decoded;
    int64_t process_us;   // all monitor_process() calls: capture-time CPU, spread across the slot
    int64_t find_us;      // ftx_find_candidates(): on the decode clock
    int64_t decode_us;    // candidate loop incl. unpack: on the decode clock
    int searches;         // ftx_find_candidates() calls
    int tried;            // candidates decoded, every search
    int iterations;       // LDPC iterations charged: the committed decodes'
    int units;            // decode_core_t spent: what the budget counted
    int cut;              // 1 when the budget stopped the slot early
    int redone;           // decodes run again at commit (decode_core_t redone)
    int wasted;           // decodes run ahead for a candidate skipped, cut or past the last decode
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
