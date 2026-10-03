#include "bench_core.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/monitor.h"
#include "decode_subtract.h"
#include "ft8/constants.h"
#include "ft8/decode.h"
#include "ft8/message.h"

#define BENCH_HASH_SLOTS     128

static ftx_candidate_t s_candidates[BENCH_MAX_CANDIDATES];
static ftx_message_t s_decoded[BENCH_HASH_SLOTS];
static ftx_message_t* s_decoded_table[BENCH_HASH_SLOTS];

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

// 25th-percentile cell, the firmware's noise-floor estimate in decode_monitor_results().
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

// Same cell read as decode_monitor_results(), then cand_db - noise_db, clamped to [-30, 99].
static int candidate_snr(const monitor_t* monitor, const ftx_candidate_t* candidate, int noise_scaled)
{
    float noise_db = 0.5f * ((float)noise_scaled - 240.0f);
    float cand_db = noise_db;
    int t_index = candidate->time_offset * monitor->wf.time_osr + candidate->time_sub;
    const int t_count = monitor->wf.num_blocks * monitor->wf.time_osr;
    if (t_count > 0) {
        if (t_index < 0) t_index = 0;
        if (t_index >= t_count) t_index = t_count - 1;
    } else {
        t_index = 0;
    }
    int f_index = candidate->freq_sub * monitor->wf.num_bins + candidate->freq_offset;
    const int f_count = monitor->wf.freq_osr * monitor->wf.num_bins;
    if (f_count > 0) {
        if (f_index < 0) f_index = 0;
        if (f_index >= f_count) f_index = f_count - 1;
    } else {
        f_index = 0;
    }
    size_t offset = (size_t)t_index * (size_t)f_count + (size_t)f_index;
    size_t total = (size_t)monitor->wf.num_blocks * (size_t)monitor->wf.block_stride;
    if (monitor->wf.mag != NULL && offset < total) {
        cand_db = 0.5f * ((float)monitor->wf.mag[offset] - 240.0f);
    }
    int snr_q = (int)lrintf(cand_db - noise_db);
    if (snr_q < -30) snr_q = -30;
    if (snr_q > 99) snr_q = 99;
    return snr_q;
}

// Same normalisation as ft8_audio_pipeline_run(): scale each block to a mean |x| of 0.1, gain in [0.1, 10].
static void bench_block_agc(float* block, int size)
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
}

// Open-addressing payload dedupe, as decode_monitor_results() does. Returns true if the message is new.
static bool bench_remember(const ftx_message_t* message)
{
    int index = (int)(message->hash % BENCH_HASH_SLOTS);
    for (int probe = 0; probe < BENCH_HASH_SLOTS; ++probe) {
        ftx_message_t* slot = s_decoded_table[index];
        if (slot == NULL) {
            s_decoded[index] = *message;
            s_decoded_table[index] = &s_decoded[index];
            return true;
        }
        if (slot->hash == message->hash && memcmp(slot->payload, message->payload, sizeof(message->payload)) == 0) {
            return false;
        }
        index = (index + 1) % BENCH_HASH_SLOTS;
    }
    return false;
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
    if (block == NULL) {
        monitor_free(&monitor);
        return;
    }

    int64_t t0 = clock_us();
    for (size_t offset = 0; offset + (size_t)block_size <= sample_count; offset += (size_t)block_size) {
        if (monitor.wf.num_blocks >= target_blocks) break;
        for (int j = 0; j < block_size; ++j) {
            block[j] = samples[offset + (size_t)j] / 32768.0f;
        }
        if (agc_on) bench_block_agc(block, block_size);
        monitor_process(&monitor, block);
    }
    free(block);
    quantise_waterfall(&monitor, mag_levels);
    int64_t t1 = clock_us();

    int max_candidates = config->max_candidates;
    if (max_candidates > BENCH_MAX_CANDIDATES) max_candidates = BENCH_MAX_CANDIDATES;
    int passes = config->subtract_passes;
    if (passes < 1) passes = 1;
    if (passes > 3) passes = 3;
    const int do_subtract = config->subtract_passes > 0;
    const float fraction = config->subtract_fraction > 0.0f ? config->subtract_fraction : DECODE_SUBTRACT_FRACTION;

    const int noise_scaled = mag_percentile(&monitor, 25);
    for (int i = 0; i < BENCH_HASH_SLOTS; ++i) {
        s_decoded_table[i] = NULL;
    }
    int candidate_count = 0;
    int decoded = 0;
    int64_t find_us = 0;
    int64_t decode_us = 0;
    for (int pass = 0; pass < passes && decoded < config->max_decodes; ++pass) {
        int want = max_candidates;
        if (do_subtract) {
            want = max_candidates / (pass + 1);
            if (want < 1) want = 1;
        }
        int64_t find_t0 = clock_us();
        int found = ftx_find_candidates(&monitor.wf, want, s_candidates, config->min_score);
        find_us += clock_us() - find_t0;
        if (pass == 0) candidate_count = found;

        float seen_hz[BENCH_MAX_CANDIDATES];
        int seen = 0;
        int64_t decode_t0 = clock_us();
        for (int i = 0; i < found && decoded < config->max_decodes; ++i) {
            float freq_hz = (monitor.min_bin + s_candidates[i].freq_offset +
                             s_candidates[i].freq_sub / (float)config->freq_osr) / monitor.symbol_period;
            float time_s = (s_candidates[i].time_offset +
                            s_candidates[i].time_sub / (float)config->time_osr) * monitor.symbol_period;
            // Both freq_osr subdivisions of one signal land within DECODE_SUBTRACT_FREQ_SKIP_HZ.
            // Subtracting the second would dig the hole twice in this pass.
            if (do_subtract && decode_subtract_already(seen_hz, seen, freq_hz)) continue;

            ftx_message_t message;
            ftx_decode_status_t status;
            memset(&message, 0, sizeof(message));
            memset(&status, 0, sizeof(status));
            if (!ftx_decode_candidate(&monitor.wf, &s_candidates[i], config->ldpc_iterations, &message, &status)) {
                continue;
            }
            bool fresh = bench_remember(&message);
            char text[FTX_MAX_MESSAGE_LENGTH] = {0};
            ftx_message_offsets_t offsets;
            if (ftx_message_decode(&message, &s_hash_interface, text, &offsets) != FTX_MESSAGE_RC_OK || text[0] == '\0') {
                continue;
            }
            int snr = candidate_snr(&monitor, &s_candidates[i], noise_scaled);
            if (do_subtract) {
                if (seen < BENCH_MAX_CANDIDATES) seen_hz[seen++] = freq_hz;
                decode_subtract_message(&monitor.wf, &s_candidates[i], message.payload,
                                        config->subtract_neighbors, fraction);
            }
            if (!fresh) continue;
            ++decoded;
            if (on_message != NULL) on_message(context, text, freq_hz, time_s, snr);
        }
        decode_us += clock_us() - decode_t0;
        if (!do_subtract) break;
    }
    int64_t t2 = t1 + find_us;
    int64_t t3 = t2 + decode_us;

    result->ok = 1;
    result->candidates = candidate_count;
    result->decoded = decoded;
    result->process_us = t1 - t0;
    result->find_us = t2 - t1;
    result->decode_us = t3 - t2;
    monitor_free(&monitor);
}
