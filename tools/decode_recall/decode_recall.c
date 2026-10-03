// Host CLI for RFC 0006 P1. Decodes one or more WAVs with the firmware's decode steps
// (bench_run_slot) and prints one jt9-style line per message. A 12 kHz WAV is
// low-pass filtered and decimated to 6 kHz here; that filter is not the firmware's
// stride decimator (RFC 0006 P3). `--synth` writes the tier-1 AWGN grid.

#include <getopt.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bench_core.h"
#include "decode_limits.h"
#include "decode_subtract.h"
#include "common/wave.h"
#include "ft8/constants.h"
#include "ft8/encode.h"
#include "ft8/message.h"

void synth_gfsk(const uint8_t* symbols, int n_sym, float f0, float symbol_bt,
                 float symbol_period, int signal_rate, float* signal);

#define DECIM_FROM 12000
#define DECIM_TAPS 1023

static int64_t clock_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static void on_message(void* context, const char* text, float freq_hz, float time_s, int snr)
{
    (void)context;
    printf("000000 %3d %+5.2f %4.0f ~ %s\n", snr, time_s, freq_hz, text);
}

static uint16_t read_u16(const uint8_t* p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t read_u32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 16-bit mono PCM. Extra RIFF chunks are skipped.
static int16_t* read_wav(const char* path, int* rate, size_t* count)
{
    FILE* file = fopen(path, "rb");
    if (file == NULL) return NULL;
    uint8_t header[12];
    if (fread(header, 1, 12, file) != 12 || memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        fclose(file);
        return NULL;
    }
    int fmt_rate = 0;
    int16_t* samples = NULL;
    size_t n = 0;
    for (;;) {
        uint8_t chunk[8];
        if (fread(chunk, 1, 8, file) != 8) break;
        uint32_t size = read_u32(chunk + 4);
        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (size < 16 || fread(fmt, 1, 16, file) != 16) break;
            if (read_u16(fmt) != 1 || read_u16(fmt + 2) != 1 || read_u16(fmt + 14) != 16) {
                fclose(file);
                return NULL;
            }
            fmt_rate = (int)read_u32(fmt + 4);
            if (size > 16 && fseek(file, (long)(size - 16), SEEK_CUR) != 0) break;
        } else if (memcmp(chunk, "data", 4) == 0) {
            n = size / 2;
            samples = (int16_t*)malloc(n * sizeof(int16_t));
            if (samples == NULL || fread(samples, sizeof(int16_t), n, file) != n) {
                free(samples);
                fclose(file);
                return NULL;
            }
            break;
        } else if (fseek(file, (long)(size + (size & 1)), SEEK_CUR) != 0) {
            break;
        }
    }
    fclose(file);
    if (samples == NULL || fmt_rate <= 0) {
        free(samples);
        return NULL;
    }
    *rate = fmt_rate;
    *count = n;
    return samples;
}

// Blackman-Harris low-pass, cutoff 2900 Hz, then keep every other sample.
// Group delay is compensated by centring the window on the kept sample.
static int16_t* decimate_12k_to_6k(const int16_t* in, size_t in_count, size_t* out_count)
{
    static float taps[DECIM_TAPS];
    static int ready = 0;
    if (!ready) {
        const double fc = 2900.0 / DECIM_FROM;
        const int mid = (DECIM_TAPS - 1) / 2;
        double sum = 0.0;
        for (int n = 0; n < DECIM_TAPS; ++n) {
            double k = (double)n - mid;
            double sinc = (k == 0.0) ? 2.0 * fc : sin(2.0 * M_PI * fc * k) / (M_PI * k);
            double phase = 2.0 * M_PI * n / (DECIM_TAPS - 1);
            double window = 0.35875 - 0.48829 * cos(phase) + 0.14128 * cos(2.0 * phase) - 0.01168 * cos(3.0 * phase);
            taps[n] = (float)(sinc * window);
            sum += taps[n];
        }
        for (int n = 0; n < DECIM_TAPS; ++n) taps[n] = (float)(taps[n] / sum);
        ready = 1;
    }
    const int mid = (DECIM_TAPS - 1) / 2;
    size_t n_out = in_count / 2;
    int16_t* out = (int16_t*)malloc(n_out * sizeof(int16_t));
    if (out == NULL) return NULL;
    for (size_t n = 0; n < n_out; ++n) {
        int center = (int)(2 * n);
        double acc = 0.0;
        for (int k = 0; k < DECIM_TAPS; ++k) {
            int idx = center + k - mid;
            if (idx >= 0 && (size_t)idx < in_count) acc += (double)taps[k] * (in[idx] / 32768.0);
        }
        if (acc > 1.0) acc = 1.0;
        if (acc < -1.0) acc = -1.0;
        out[n] = (int16_t)lrint(acc * 32767.0);
    }
    *out_count = n_out;
    return out;
}

static int decode_file(const char* path, const BenchConfig* config)
{
    int rate = 0;
    size_t count = 0;
    int16_t* samples = read_wav(path, &rate, &count);
    if (samples == NULL) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    int16_t* owned = NULL;
    const int want = config->sample_rate > 0 ? config->sample_rate : BENCH_SAMPLE_RATE;
    if (rate == DECIM_FROM && want == BENCH_SAMPLE_RATE) {
        owned = decimate_12k_to_6k(samples, count, &count);
        free(samples);
        samples = owned;
        if (samples == NULL) return 1;
        rate = BENCH_SAMPLE_RATE;
    }
    if (rate != want) {
        fprintf(stderr, "%s: sample rate %d, want %d\n", path, rate, want);
        free(samples);
        return 1;
    }
    printf("FILE %s\n", path);
    BenchResult result;
    int64_t t0 = clock_us();
    bench_run_slot(config, samples, count, clock_us, on_message, NULL, &result);
    int64_t wall_ms = (clock_us() - t0) / 1000;
    free(samples);
    if (!result.ok) {
        printf("FAIL %s\n", path);
        return 1;
    }
    printf("END cand=%d dec=%d wall_ms=%lld find_ms=%lld decode_ms=%lld\n",
           result.candidates, result.decoded, (long long)wall_ms,
           (long long)(result.find_us / 1000), (long long)(result.decode_us / 1000));
    return 0;
}

static uint32_t g_rng;

static float urand(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return (g_rng >> 8) * (1.0f / 16777216.0f);
}

static float gauss(void)
{
    float u1 = urand();
    if (u1 < 1e-7f) u1 = 1e-7f;
    float u2 = urand();
    return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * (float)M_PI * u2);
}

// One full FT8 signal in white noise. SNR is in a 2500 Hz bandwidth, WSJT-X's definition.
// The whole buffer is scaled so nothing clips; that scale does not change the SNR.
static int write_synth(const char* dir, int index, const char* message, float freq_hz, int snr_db)
{
    ftx_message_t msg;
    if (ftx_message_encode(&msg, NULL, message) != FTX_MESSAGE_RC_OK) {
        fprintf(stderr, "cannot encode %s\n", message);
        return 1;
    }
    uint8_t tones[FT8_NN];
    ft8_encode(msg.payload, tones);

    const int rate = BENCH_SAMPLE_RATE;
    const int n_signal = (int)(0.5f + FT8_NN * FT8_SYMBOL_PERIOD * rate);
    const int n_slot = (int)(0.5f + FT8_SLOT_TIME * rate);
    float* slot = (float*)calloc((size_t)n_slot, sizeof(float));
    if (slot == NULL) return 1;
    synth_gfsk(tones, FT8_NN, freq_hz, 2.0f, FT8_SYMBOL_PERIOD, rate, slot);

    double power = 0.0;
    for (int i = 0; i < n_signal; ++i) power += (double)slot[i] * slot[i];
    power /= n_signal;
    double snr_lin = pow(10.0, snr_db / 10.0);
    // Noise power in 2500 Hz equals Ps / snr. Discrete white noise spreads across fs/2.
    double sigma = sqrt(power * (double)rate / (5000.0 * snr_lin));
    g_rng = 0xC0FFEEu ^ ((uint32_t)snr_db * 131u) ^ (uint32_t)freq_hz ^ ((uint32_t)index << 16);
    float peak = 0.0f;
    for (int i = 0; i < n_slot; ++i) {
        slot[i] += gauss() * (float)sigma;
        float a = fabsf(slot[i]);
        if (a > peak) peak = a;
    }
    if (peak > 0.95f) {
        float scale = 0.95f / peak;
        for (int i = 0; i < n_slot; ++i) slot[i] *= scale;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/snr%d_f%d_%d.wav", dir, snr_db, (int)freq_hz, index);
    int rc = save_wav(slot, n_slot, rate, path);
    free(slot);
    if (rc != 0) return 1;
    printf("%d\t%d\t%s\t%s\n", snr_db, (int)freq_hz, message, path);
    return 0;
}

static int synth_grid(const char* dir)
{
    static const char* messages[] = { "CQ KB2SLO FN31", "W1AW KB2SLO R-15" };
    static const int snrs[] = { -24, -22, -20, -18, -16, -14, -12, -10, -6, 0 };
    static const float freqs[] = { 800.0f, 1500.0f };
    int index = 0;
    for (int m = 0; m < 2; ++m) {
        for (int s = 0; s < 10; ++s) {
            for (int f = 0; f < 2; ++f) {
                if (write_synth(dir, index, messages[m], freqs[f], snrs[s]) != 0) return 1;
                ++index;
            }
        }
    }
    return 0;
}

static void usage(const char* argv0)
{
    fprintf(stderr,
            "usage: %s [options] wav...\n"
            "       %s --synth DIR\n"
            "defaults are the firmware's: time-osr 2, freq-osr 2, candidates %d, decodes 32,\n"
            "min-score 5, ldpc 25, sample-rate 6000, agc on, levels 256,\n"
            "subtract-passes %d, subtract-neighbors %d, subtract-fraction 0.5\n",
            argv0, argv0, DECODE_MAX_CANDIDATES, DECODE_SUBTRACT_PASSES, DECODE_SUBTRACT_NEIGHBORS);
}

int main(int argc, char** argv)
{
    BenchConfig config = {
        .name = "recall",
        .time_osr = 2,
        .freq_osr = 2,
        .max_candidates = DECODE_MAX_CANDIDATES,
        .max_decodes = 32,
        .min_score = 5,
        .ldpc_iterations = 25,
        .sample_rate = 6000,
        .agc = 0,
        .mag_levels = 256,
        .subtract_passes = DECODE_SUBTRACT_PASSES,
        .subtract_neighbors = DECODE_SUBTRACT_NEIGHBORS,
        .subtract_fraction = DECODE_SUBTRACT_FRACTION,
    };
    const char* synth_dir = NULL;
    static const struct option options[] = {
        { "time-osr", required_argument, NULL, 't' },
        { "freq-osr", required_argument, NULL, 'f' },
        { "candidates", required_argument, NULL, 'c' },
        { "decodes", required_argument, NULL, 'd' },
        { "min-score", required_argument, NULL, 's' },
        { "ldpc", required_argument, NULL, 'l' },
        { "sample-rate", required_argument, NULL, 'r' },
        { "agc", required_argument, NULL, 'a' },
        { "levels", required_argument, NULL, 'q' },
        { "subtract-passes", required_argument, NULL, 'p' },
        { "subtract-neighbors", required_argument, NULL, 'n' },
        { "subtract-fraction", required_argument, NULL, 'F' },
        { "synth", required_argument, NULL, 'y' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "h", options, NULL)) != -1) {
        switch (opt) {
            case 't': config.time_osr = atoi(optarg); break;
            case 'f': config.freq_osr = atoi(optarg); break;
            case 'c': config.max_candidates = atoi(optarg); break;
            case 'd': config.max_decodes = atoi(optarg); break;
            case 's': config.min_score = atoi(optarg); break;
            case 'l': config.ldpc_iterations = atoi(optarg); break;
            case 'r': config.sample_rate = atoi(optarg); break;
            case 'a':
                if (strcmp(optarg, "off") == 0) config.agc = 2;
                else if (strcmp(optarg, "on") == 0) config.agc = 0;
                else {
                    usage(argv[0]);
                    return 2;
                }
                break;
            case 'q': config.mag_levels = atoi(optarg); break;
            case 'p': config.subtract_passes = atoi(optarg); break;
            case 'n': config.subtract_neighbors = atoi(optarg); break;
            case 'F': config.subtract_fraction = (float)atof(optarg); break;
            case 'y': synth_dir = optarg; break;
            default:
                usage(argv[0]);
                return 2;
        }
    }
    if (config.max_candidates < 1 || config.max_candidates > BENCH_MAX_CANDIDATES ||
        config.max_decodes < 1 || config.time_osr < 1 || config.freq_osr < 1 ||
        config.subtract_passes < 0 || config.subtract_passes > 3 ||
        (config.subtract_neighbors != 0 && config.subtract_neighbors != 1) ||
        config.subtract_fraction < 0.0f || config.subtract_fraction > 1.0f) {
        fprintf(stderr, "candidates must be 1..%d; osr and decodes must be >= 1; "
                        "subtract-passes is 0..3; subtract-neighbors is 0 or 1; "
                        "subtract-fraction is 0 (half) or (0, 1]\n",
                BENCH_MAX_CANDIDATES);
        return 2;
    }
    if (synth_dir != NULL) return synth_grid(synth_dir);
    if (optind >= argc) {
        usage(argv[0]);
        return 2;
    }
    int failed = 0;
    for (int i = optind; i < argc; ++i) {
        if (decode_file(argv[i], &config) != 0) failed = 1;
    }
    return failed;
}
