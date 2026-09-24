#include "bench_core.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "common/monitor.h"
#include "ft8/constants.h"
#include "ft8/decode.h"
#include "ft8/message.h"

#define BENCH_MAX_CANDIDATES 256
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

    monitor_config_t monitor_config = {
        .f_min = 200.0f,
        .f_max = 2900.0f,
        .sample_rate = BENCH_SAMPLE_RATE,
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
    static float block[1920];
    const int block_size = monitor.block_size;

    int64_t t0 = clock_us();
    for (size_t offset = 0; offset + (size_t)block_size <= sample_count; offset += (size_t)block_size) {
        if (monitor.wf.num_blocks >= target_blocks) break;
        for (int j = 0; j < block_size; ++j) {
            block[j] = samples[offset + (size_t)j] / 32768.0f;
        }
        bench_block_agc(block, block_size);
        monitor_process(&monitor, block);
    }
    int64_t t1 = clock_us();

    int max_candidates = config->max_candidates;
    if (max_candidates > BENCH_MAX_CANDIDATES) max_candidates = BENCH_MAX_CANDIDATES;
    int candidate_count = ftx_find_candidates(&monitor.wf, max_candidates, s_candidates, config->min_score);
    int64_t t2 = clock_us();

    for (int i = 0; i < BENCH_HASH_SLOTS; ++i) {
        s_decoded_table[i] = NULL;
    }
    int decoded = 0;
    for (int i = 0; i < candidate_count && decoded < config->max_decodes; ++i) {
        ftx_message_t message;
        ftx_decode_status_t status;
        memset(&message, 0, sizeof(message));
        memset(&status, 0, sizeof(status));
        if (!ftx_decode_candidate(&monitor.wf, &s_candidates[i], config->ldpc_iterations, &message, &status)) {
            continue;
        }
        if (!bench_remember(&message)) {
            continue;
        }
        char text[FTX_MAX_MESSAGE_LENGTH] = {0};
        ftx_message_offsets_t offsets;
        if (ftx_message_decode(&message, &s_hash_interface, text, &offsets) != FTX_MESSAGE_RC_OK || text[0] == '\0') {
            continue;
        }
        ++decoded;
        if (on_message != NULL) {
            float freq_hz = (monitor.min_bin + s_candidates[i].freq_offset +
                             s_candidates[i].freq_sub / (float)config->freq_osr) / monitor.symbol_period;
            float time_s = (s_candidates[i].time_offset +
                            s_candidates[i].time_sub / (float)config->time_osr) * monitor.symbol_period;
            on_message(context, text, freq_hz, time_s);
        }
    }
    int64_t t3 = clock_us();

    result->ok = 1;
    result->candidates = candidate_count;
    result->decoded = decoded;
    result->process_us = t1 - t0;
    result->find_us = t2 - t1;
    result->decode_us = t3 - t2;
    monitor_free(&monitor);
}
