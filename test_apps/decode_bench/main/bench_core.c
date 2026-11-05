#include "bench_core.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/decode_psram.h"
#include "common/monitor.h"
#include "decode_core.h"
#include "decode_subtract.h"
#include "ft8/constants.h"
#include "ft8/decode.h"
#include "ft8/message.h"

// Same placement as the firmware's decode working set (memory.md).
static DECODE_PSRAM_BSS decode_core_t s_core;

// No callsign hash table: hashed calls unpack as "<...>", which is how the scorer normalises them anyway.
static bool bench_lookup_hash(ftx_callsign_hash_type_t hash_type, uint32_t hash, char* callsign)
{
    (void)hash_type;
    (void)hash;
    callsign[0] = '\0';
    return false;
}

static void bench_save_hash(const char* callsign, uint32_t n22)
{
    (void)callsign;
    (void)n22;
}

static ftx_callsign_hash_interface_t s_hash_interface = {
    .lookup_hash = bench_lookup_hash,
    .save_hash = bench_save_hash,
};

// Cell code at `percent` of the waterfall, the noise floor the fewer-bits simulation anchors on.
static int mag_percentile(const monitor_t* monitor, int percent)
{
    const size_t total = (size_t)monitor->wf.num_blocks * (size_t)monitor->wf.block_stride;
    uint32_t hist[256];
    memset(hist, 0, sizeof(hist));
    if (monitor->wf.mag == NULL || total == 0) return 0;
    for (size_t i = 0; i < total; ++i) hist[monitor->wf.mag[i]]++;
    uint64_t target = total * (uint64_t)percent / 100;
    uint64_t accum = 0;
    for (int v = 0; v < 256; ++v) {
        accum += hist[v];
        if (accum >= target) return v;
    }
    return 0;
}

// RFC 0006 §4, fewer-bits simulation: `levels` codes across a 32 dB window (64 counts at
// 0.5 dB), with the noise floor two steps above the bottom. One value per byte; no packing.
// levels <= 1 or >= 256 leaves the 8-bit cells alone.
static void quantise_waterfall(monitor_t* monitor, int levels)
{
    if (levels <= 1 || levels >= 256 || monitor->wf.mag == NULL) return;
    const size_t total = (size_t)monitor->wf.num_blocks * (size_t)monitor->wf.block_stride;
    int noise = mag_percentile(monitor, 25);
    int step = 64 / levels;
    if (step < 1) step = 1;
    int bottom = noise - 2 * step;
    for (size_t i = 0; i < total; ++i) {
        int level = ((int)monitor->wf.mag[i] - bottom) / step;
        if (level < 0) level = 0;
        if (level >= levels) level = levels - 1;
        int stored = bottom + level * step;
        if (stored < 0) stored = 0;
        if (stored > 255) stored = 255;
        monitor->wf.mag[i] = (uint8_t)stored;
    }
}

// Same normalisation as ft8_audio_pipeline_run(): scale each block to a mean |x| of 0.1, gain in [0.1, 10].
// Returns the gain, which the SNR divides back out.
static float bench_block_agc(float* block, int size)
{
    double accumulator = 0.0;
    for (int j = 0; j < size; ++j) {
        accumulator += fabsf(block[j]);
    }
    float level = (float)(accumulator / size);
    float gain = (level > 1e-6f) ? 0.1f / level : 1.0f;
    if (gain < 0.1f) gain = 0.1f;
    if (gain > 10.0f) gain = 10.0f;
    for (int j = 0; j < size; ++j) {
        block[j] *= gain;
    }
    return gain;
}

void bench_run_slot(const BenchConfig* config, const int16_t* samples, size_t sample_count,
                    bench_clock_fn clock_us, bench_message_fn on_message, void* context,
                    BenchResult* result)
{
    memset(result, 0, sizeof(*result));

    const int sample_rate = config->sample_rate > 0 ? config->sample_rate : BENCH_SAMPLE_RATE;
    const int agc_on = config->agc != 2;
    const int mag_levels = config->mag_levels > 0 ? config->mag_levels : 256;
    monitor_config_t monitor_config = {
        .f_min = 200.0f,
        .f_max = 2900.0f,
        .sample_rate = sample_rate,
        .time_osr = config->time_osr,
        .freq_osr = config->freq_osr,
        .protocol = FTX_PROTOCOL_FT8,
    };
    monitor_t monitor;
    memset(&monitor, 0, sizeof(monitor));
    monitor_init(&monitor, &monitor_config);
    if (monitor.wf.mag == NULL || monitor.window == NULL) {
        monitor_free(&monitor);
        return;
    }
    monitor_reset(&monitor);

    // ft8_audio_pipeline_run() triggers decode once FT8_NN (79) blocks are in the waterfall, i.e. 12.64 s after
    // the slot boundary; its FT8_NN + 1 fill limit is never reached before the trigger.
    const int target_blocks = FT8_NN;
    const int block_size = monitor.block_size;
    float* block = (float*)malloc(sizeof(float) * (size_t)block_size);
    float* block_gain = (float*)malloc(sizeof(float) * (size_t)monitor.wf.max_blocks);
    if (block == NULL || block_gain == NULL) {
        free(block);
        free(block_gain);
        monitor_free(&monitor);
        return;
    }

    int64_t t0 = clock_us();
    for (size_t offset = 0; offset + (size_t)block_size <= sample_count; offset += (size_t)block_size) {
        if (monitor.wf.num_blocks >= target_blocks) break;
        for (int j = 0; j < block_size; ++j) {
            block[j] = samples[offset + (size_t)j] / 32768.0f;
        }
        block_gain[monitor.wf.num_blocks] = agc_on ? bench_block_agc(block, block_size) : 1.0f;
        monitor_process(&monitor, block);
    }
    free(block);
    quantise_waterfall(&monitor, mag_levels);
    int64_t t1 = clock_us();

    const decode_core_config_t core_config = {
        .max_candidates = config->max_candidates,
        .min_score = config->min_score,
        .ldpc_iterations = config->ldpc_iterations,
        .passes = config->subtract_passes,
        .subtract_neighbors = config->subtract_neighbors,
        .subtract_fraction = config->subtract_fraction > 0.0f ? config->subtract_fraction : DECODE_SUBTRACT_FRACTION,
        .hash_if = &s_hash_interface,
    };
    int candidate_count = 0;
    int decoded = 0;
    int64_t find_us = 0;
    int64_t decode_us = 0;
    // The SNR's preparation is on the decode clock, as in decode_monitor_results().
    int64_t snr_t0 = clock_us();
    decode_core_begin(&s_core, &monitor, block_gain, &core_config);
    decode_us += clock_us() - snr_t0;
    do {
        int64_t find_t0 = clock_us();
        int found = decode_core_search(&s_core);
        find_us += clock_us() - find_t0;
        if (s_core.pass == 0) candidate_count = found;

        int64_t decode_t0 = clock_us();
        for (int i = 0; i < found && decoded < config->max_decodes; ++i) {
            if (decode_core_skip(&s_core, i)) continue;
            decode_core_attempt_t attempt;
            decode_core_message_t message;
            decode_core_decode(&s_core, i, &attempt);
            if (decode_core_commit(&s_core, i, &attempt, &message) != DECODE_CORE_NEW) continue;
            ++decoded;
            if (on_message != NULL) on_message(context, message.text, message.freq_hz, message.time_s, message.snr);
        }
        decode_us += clock_us() - decode_t0;
    } while (decoded < config->max_decodes && decode_core_next_pass(&s_core));
    int64_t t2 = t1 + find_us;
    int64_t t3 = t2 + decode_us;

    result->ok = 1;
    result->candidates = candidate_count;
    result->decoded = decoded;
    result->process_us = t1 - t0;
    result->find_us = t2 - t1;
    result->decode_us = t3 - t2;
    free(block_gain);
    monitor_free(&monitor);
}
