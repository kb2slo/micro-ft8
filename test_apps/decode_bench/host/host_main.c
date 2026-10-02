// Host side of the decode benchmark: same core, same configurations, same output format as the device
// image, reading the slot files the device embeds. Diff the MSG lines of the two logs for the
// host/device parity check (RFC 0006 §3). Host timings rank configurations; they are not device timings.

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "bench_configs.h"
#include "bench_core.h"

typedef struct {
    const char* config;
    int slot;
} MessageContext;

static int64_t bench_clock_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static void print_message(void* context, const char* text, float freq_hz, float time_s, int snr)
{
    (void)snr;
    const MessageContext* where = (const MessageContext*)context;
    printf("MSG %s %d %4.0f %+5.2f %s\n", where->config, where->slot, freq_hz, time_s, text);
}

static int16_t* read_slot(const char* path, size_t* count)
{
    FILE* file = fopen(path, "rb");
    if (file == NULL) return NULL;
    fseek(file, 0, SEEK_END);
    long bytes = ftell(file);
    fseek(file, 0, SEEK_SET);
    int16_t* samples = (int16_t*)malloc((size_t)bytes);
    if (samples != NULL && fread(samples, 1, (size_t)bytes, file) != (size_t)bytes) {
        free(samples);
        samples = NULL;
    }
    fclose(file);
    *count = (size_t)bytes / sizeof(int16_t);
    return samples;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s slot_0.raw [slot_1.raw ...]\n", argv[0]);
        return 2;
    }
    printf("BENCH target=host\n");
    for (int c = 0; c < BENCH_CONFIG_COUNT; ++c) {
        const BenchConfig* config = &k_bench_configs[c];
        for (int s = 1; s < argc; ++s) {
            size_t count = 0;
            int16_t* samples = read_slot(argv[s], &count);
            if (samples == NULL) {
                fprintf(stderr, "cannot read %s\n", argv[s]);
                return 1;
            }
            MessageContext where = { config->name, s - 1 };
            BenchResult result;
            bench_run_slot(config, samples, count, bench_clock_us, print_message, &where, &result);
            printf("RESULT %s %d ok=%d cand=%d dec=%d find_ms=%.1f decode_ms=%.1f total_ms=%.1f process_ms=%.1f\n",
                   config->name, s - 1, result.ok, result.candidates, result.decoded,
                   result.find_us / 1000.0, result.decode_us / 1000.0,
                   (result.find_us + result.decode_us) / 1000.0, result.process_us / 1000.0);
            free(samples);
        }
    }
    printf("BENCH done\n");
    return 0;
}
