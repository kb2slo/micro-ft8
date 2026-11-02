// Offline I/Q front end (row y9bt, stage 2). Runs components/iq_frontend, the code the firmware
// will run, over a recording, and makes synthetic I/Q from a corpus WAV to test it with.
//
//   iq_convert [--center-hz F]... [--if-trim-hz T] [--swap-iq] [--negate-q] [--output-rate R] -o OUT.wav IQ.wav
//       A 48 kHz stereo I/Q WAV (16- or 24-bit) to one mono 16-bit WAV per --center-hz (default
//       13,500: a QMX's FT8 channel). With several centres, each output is OUT_<centre>Hz.wav.
//   iq_convert --real [--output-rate R] -o OUT.wav AUDIO.wav
//       The radio's demodulated audio instead: L and R averaged, as uac_pcm_to_ft8_samples() does,
//       through the same filters, so both arms of an A/B share one decimator.
//   iq_convert --synth [--center-hz F] [--if-trim-hz T] [--swap-iq] [--negate-q] [--gain-db G]
//              [--phase-deg P] [--dc-db D] [--mirror-tone-db L] [--alias-tone-db L] [--tone-offset-hz H]
//              [--mirror-wav W] [--mirror-wav-db L] [--level-db L] [--bits 24|16] -o IQ.wav AUDIO_12K.wav
//       Synthetic I/Q: the audio's analytic signal at 48 kHz, its 0 Hz at the dial (centre - 1,500 Hz
//       + trim) from the LO, then the mixer's imbalance (Q's gain and phase against I's), a DC offset
//       at the LO, a tone at the channel's mirror about the LO, another 12 kHz WAV filling the mirror
//       channel (each audio frequency f at -(dial + f), so an image of it lands at f), and a tone
//       48 kHz from the channel (which a 48 kHz ADC samples onto the channel itself: an alias).
//       Levels are dB against the signal's RMS; the tones land at 1,500 + tone-offset Hz of audio.
//       Deterministic.
//
// Every output prints an OUT line naming the NCO frequency applied, trim included, so a wrong trim
// is visible: it moves every decoded frequency by that many Hz. --synth also prints the image level
// its imbalance gives, measured through the front end.

#include <getopt.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fft/kiss_fft.h"
#include "iq_frontend.h"

#define IQ_RATE 48000
#define MAX_CENTERS 8
#define CHUNK 4096
#define TWO_PI 6.283185307179586476925286766559

typedef struct {
    int rate;
    int channels;
    size_t frames;
    float* samples;  // interleaved, full scale ±1
} wav_t;

static uint32_t le32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t* p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

// PCM 16- or 24-bit, any channel count, WAVE_FORMAT_PCM or EXTENSIBLE. Other chunks are skipped.
static int read_wav(const char* path, wav_t* wav)
{
    FILE* file = fopen(path, "rb");
    if (file == NULL) return -1;
    uint8_t header[12];
    int bits = 0;
    int ok = -1;
    memset(wav, 0, sizeof(*wav));
    if (fread(header, 1, 12, file) != 12 || memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        fclose(file);
        return -1;
    }
    for (;;) {
        uint8_t chunk[8];
        if (fread(chunk, 1, 8, file) != 8) break;
        uint32_t size = le32(chunk + 4);
        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[40];
            uint32_t want = size < sizeof(fmt) ? size : sizeof(fmt);
            if (size < 16 || fread(fmt, 1, want, file) != want) break;
            int tag = le16(fmt);
            if (tag == 0xFFFE && size >= 26) tag = le16(fmt + 24);  // EXTENSIBLE: the sub-format's tag
            wav->channels = le16(fmt + 2);
            wav->rate = (int)le32(fmt + 4);
            bits = le16(fmt + 14);
            if (tag != 1 || (bits != 16 && bits != 24) || wav->channels < 1) break;
            if (size > want && fseek(file, (long)(size - want), SEEK_CUR) != 0) break;
            if (size % 2 && fseek(file, 1, SEEK_CUR) != 0) break;
        } else if (memcmp(chunk, "data", 4) == 0 && bits != 0) {
            int bytes = bits / 8;
            wav->frames = size / (uint32_t)(bytes * wav->channels);
            size_t n = wav->frames * (size_t)wav->channels;
            uint8_t* raw = malloc(n * bytes);
            wav->samples = malloc(n * sizeof(float));
            if (raw == NULL || wav->samples == NULL || fread(raw, (size_t)bytes, n, file) != n) {
                free(raw);
                break;
            }
            for (size_t i = 0; i < n; ++i) {
                const uint8_t* p = raw + i * bytes;
                int32_t v = (bytes == 2) ? (int16_t)le16(p)
                                         : (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8;
                wav->samples[i] = (float)v / (float)(bytes == 2 ? 32768 : 8388608);
            }
            free(raw);
            ok = 0;
            break;
        } else {
            if (fseek(file, (long)(size + (size % 2)), SEEK_CUR) != 0) break;
        }
    }
    fclose(file);
    if (ok != 0) {
        free(wav->samples);
        wav->samples = NULL;
    }
    return ok;
}

static void put16(FILE* f, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    fwrite(b, 1, 2, f);
}

static void put32(FILE* f, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    fwrite(b, 1, 4, f);
}

// Writes interleaved float samples as PCM; returns how many were clipped, or -1.
static long write_wav(const char* path, const float* samples, size_t frames, int channels, int rate, int bits)
{
    FILE* f = fopen(path, "wb");
    if (f == NULL) return -1;
    int bytes = bits / 8;
    uint32_t data = (uint32_t)(frames * channels * bytes);
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + data);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, (uint16_t)channels);
    put32(f, (uint32_t)rate);
    put32(f, (uint32_t)(rate * channels * bytes));
    put16(f, (uint16_t)(channels * bytes));
    put16(f, (uint16_t)bits);
    fwrite("data", 1, 4, f);
    put32(f, data);
    long clipped = 0;
    double full = (bits == 16) ? 32767.0 : 8388607.0;
    for (size_t i = 0; i < frames * (size_t)channels; ++i) {
        double v = floor(samples[i] * full + 0.5);
        if (v > full) {
            v = full;
            ++clipped;
        } else if (v < -full - 1) {
            v = -full - 1;
            ++clipped;
        }
        int32_t s = (int32_t)v;
        uint8_t b[3] = {(uint8_t)s, (uint8_t)(s >> 8), (uint8_t)(s >> 16)};
        fwrite(b, 1, (size_t)bytes, f);
    }
    if (fclose(f) != 0) return -1;
    return clipped;
}

typedef struct {
    double centers[MAX_CENTERS];
    int n_centers;
    double if_trim_hz;
    int swap_iq;
    int negate_q;
    int output_rate;
    int real;
    int synth;
    double gain_db;
    double phase_deg;
    double dc_db;
    double mirror_tone_db;
    double alias_tone_db;
    double tone_offset_hz;
    const char* mirror_wav;
    double mirror_wav_db;
    double level_db;
    int bits;
    const char* out;
} options_t;

static iq_frontend_config_t frontend_config(const options_t* o, double center, int input_rate)
{
    iq_frontend_config_t c;
    memset(&c, 0, sizeof(c));
    c.input_rate_hz = (uint32_t)input_rate;
    c.output_rate_hz = (uint32_t)o->output_rate;
    c.center_hz = center;
    c.if_trim_hz = o->if_trim_hz;
    c.swap_iq = o->swap_iq != 0;
    c.negate_q = o->negate_q != 0;
    return c;
}

static const char* status_text(iq_frontend_status_t s)
{
    switch (s) {
        case IQ_FRONTEND_OK: return "ok";
        case IQ_FRONTEND_BAD_RATES: return "input/output rate is not a power of two in 2..64, or output is under 6 kHz";
        case IQ_FRONTEND_TOO_MANY_TAPS: return "output rate too high for the filter buffers";
        case IQ_FRONTEND_BAD_CENTER: return "the channel does not fit inside the input band";
    }
    return "?";
}

// Runs one channel over interleaved I/Q and writes its audio.
static int receive(const options_t* o, const float* iq, size_t frames, int rate, double center, const char* path)
{
    iq_frontend_t* frontend = malloc(sizeof(*frontend));
    iq_frontend_config_t config = frontend_config(o, center, rate);
    iq_frontend_status_t status = iq_frontend_init(frontend, &config);
    if (status != IQ_FRONTEND_OK) {
        fprintf(stderr, "iq_convert: centre %g Hz: %s\n", center, status_text(status));
        free(frontend);
        return -1;
    }
    float* audio = malloc((iq_frontend_output_capacity(frontend, frames) + 1) * sizeof(float));
    size_t written = 0;
    for (size_t at = 0; at < frames; at += CHUNK) {
        size_t n = frames - at < CHUNK ? frames - at : CHUNK;
        written += (size_t)iq_frontend_process(frontend, iq + 2 * at, n, audio + written, iq_frontend_output_capacity(frontend, n));
    }
    long clipped = write_wav(path, audio, written, 1, o->output_rate, 16);
    printf("OUT %s center_hz=%g if_trim_hz=%g nco_hz=%.4f audio_hz=%d-%d samples=%zu clipped=%ld\n", path, center,
           o->if_trim_hz, iq_frontend_nco_hz(frontend), frontend->audio_low_hz, frontend->audio_high_hz, written, clipped);
    free(audio);
    free(frontend);
    return clipped < 0 ? -1 : 0;
}

static void output_name(char* buf, size_t size, const char* out, double center, int several)
{
    if (!several) {
        snprintf(buf, size, "%s", out);
        return;
    }
    size_t len = strlen(out);
    if (len > 4 && strcmp(out + len - 4, ".wav") == 0) len -= 4;
    snprintf(buf, size, "%.*s_%gHz.wav", (int)len, out, center);
}

// Smallest n >= want with no prime factor above 5, so kiss_fft stays fast.
static size_t smooth_size(size_t want)
{
    for (size_t n = want;; ++n) {
        size_t m = n;
        while (m % 2 == 0) m /= 2;
        while (m % 3 == 0) m /= 3;
        while (m % 5 == 0) m /= 5;
        if (m == 1) return n;
    }
}

// Image level for this imbalance, measured through the front end: a tone at the channel's mirror
// against the same tone in the channel, at 1,730 Hz of audio.
static double measure_image_db(const options_t* o, double center)
{
    const size_t frames = 9600;
    const double audio = 1730.0;
    const double g = pow(10.0, o->gain_db / 20.0);
    const double phi = o->phase_deg * TWO_PI / 360.0;
    double level[2];
    float* iq = malloc(2 * frames * sizeof(float));
    float* out = malloc(frames * sizeof(float));
    iq_frontend_t* frontend = malloc(sizeof(*frontend));
    options_t plain = *o;
    plain.swap_iq = plain.negate_q = 0;
    plain.if_trim_hz = 0;
    for (int k = 0; k < 2; ++k) {
        double hz = (k == 0 ? 1 : -1) * (center - 1500.0 + audio);
        for (size_t n = 0; n < frames; ++n) {
            double theta = TWO_PI * hz * (double)n / IQ_RATE;
            iq[2 * n] = (float)cos(theta);
            iq[2 * n + 1] = (float)(g * sin(theta + phi));
        }
        iq_frontend_config_t config = frontend_config(&plain, center, IQ_RATE);
        config.output_rate_hz = 12000;
        iq_frontend_init(frontend, &config);
        int w = iq_frontend_process(frontend, iq, frames, out, frames);
        double c = 0, s = 0;
        for (int m = w - 1200; m < w; ++m) {
            c += out[m] * cos(TWO_PI * audio * m / 12000.0);
            s += out[m] * sin(TWO_PI * audio * m / 12000.0);
        }
        level[k] = sqrt(c * c + s * s);
    }
    free(iq);
    free(out);
    free(frontend);
    return 20.0 * log10(level[1] / level[0] + 1e-30);
}

// The analytic signal at 48 kHz, `frames` long: the audio's positive frequencies, doubled, and
// inverse-transformed at 48 kHz. Its RMS goes in *rms.
static kiss_fft_cpx* analytic_48k(const wav_t* in, size_t frames, double* rms)
{
    const int up = IQ_RATE / in->rate;
    const size_t n = smooth_size(in->frames > frames / up ? in->frames : frames / up);
    const size_t m = n * (size_t)up;
    kiss_fft_cpx* x = calloc(n, sizeof(*x));
    kiss_fft_cpx* xf = calloc(n, sizeof(*xf));
    kiss_fft_cpx* spectrum = calloc(m, sizeof(*spectrum));
    kiss_fft_cpx* analytic = calloc(m, sizeof(*analytic));
    for (size_t i = 0; i < in->frames; ++i) x[i].r = in->samples[i];
    kiss_fft_cfg forward = kiss_fft_alloc((int)n, 0, NULL, NULL);
    kiss_fft_cfg inverse = kiss_fft_alloc((int)m, 1, NULL, NULL);
    kiss_fft(forward, x, xf);
    for (size_t k = 1; k < (n + 1) / 2; ++k) {
        spectrum[k].r = 2.0f * xf[k].r / (float)n;
        spectrum[k].i = 2.0f * xf[k].i / (float)n;
    }
    spectrum[0].r = xf[0].r / (float)n;
    kiss_fft(inverse, spectrum, analytic);
    double power = 0.0;
    for (size_t i = 0; i < frames; ++i) power += (double)analytic[i].r * analytic[i].r + (double)analytic[i].i * analytic[i].i;
    *rms = sqrt(power / (double)frames);
    free(x);
    free(xf);
    free(spectrum);
    kiss_fft_free(forward);
    kiss_fft_free(inverse);
    return analytic;
}

static int synthesize(const options_t* o, const wav_t* in, const wav_t* mirror_in)
{
    if (in->channels != 1 || IQ_RATE % in->rate != 0
        || (mirror_in && (mirror_in->channels != 1 || mirror_in->rate != in->rate))) {
        fprintf(stderr, "iq_convert --synth: want mono WAVs at one rate dividing 48 kHz\n");
        return -1;
    }
    const int up = IQ_RATE / in->rate;
    const size_t frames = in->frames * (size_t)up;
    double rms = 0.0;
    double mirror_rms = 0.0;
    kiss_fft_cpx* analytic = analytic_48k(in, frames, &rms);
    kiss_fft_cpx* mirror_signal = mirror_in ? analytic_48k(mirror_in, frames, &mirror_rms) : NULL;
    const double mirror_gain = mirror_in ? rms / mirror_rms * pow(10.0, o->mirror_wav_db / 20.0) : 0.0;

    const double center = o->centers[0];
    const double dial = center + o->if_trim_hz - 1500.0;
    const double tone_hz = center + o->if_trim_hz + o->tone_offset_hz;
    const double mirror = (o->mirror_tone_db > -999) ? rms * pow(10.0, o->mirror_tone_db / 20.0) : 0.0;
    const double alias = (o->alias_tone_db > -999) ? rms * pow(10.0, o->alias_tone_db / 20.0) : 0.0;
    const double dc = (o->dc_db > -999) ? rms * pow(10.0, o->dc_db / 20.0) : 0.0;
    const double g = pow(10.0, o->gain_db / 20.0);
    const double phi = o->phase_deg * TWO_PI / 360.0;
    const double level = pow(10.0, o->level_db / 20.0);

    float* iq = malloc(2 * frames * sizeof(float));
    for (size_t i = 0; i < frames; ++i) {
        double t = (double)i / IQ_RATE;
        double a = TWO_PI * fmod(dial * t, 1.0);
        double re = analytic[i].r * cos(a) - analytic[i].i * sin(a);
        double im = analytic[i].r * sin(a) + analytic[i].i * cos(a);
        if (mirror_signal) {
            // The other slot in the mirror channel: conj(s e^{ja}), so audio f sits at -(dial + f).
            re += mirror_gain * (mirror_signal[i].r * cos(a) - mirror_signal[i].i * sin(a));
            im -= mirror_gain * (mirror_signal[i].r * sin(a) + mirror_signal[i].i * cos(a));
        }
        // The mirror tone sits at -(channel) from the LO; the alias tone 48 kHz below the channel.
        double am = -TWO_PI * fmod(tone_hz * t, 1.0);
        double aa = TWO_PI * fmod((tone_hz - IQ_RATE) * t, 1.0);
        re += mirror * cos(am) + alias * cos(aa);
        im += mirror * sin(am) + alias * sin(aa);
        // The mixer: Q's path has g more gain and phi more phase than I's. Q = g (sin cos phi + cos sin phi).
        double i_out = re + dc;
        double q_out = g * (im * cos(phi) + re * sin(phi)) + dc;
        if (o->negate_q) q_out = -q_out;
        iq[2 * i] = (float)(level * (o->swap_iq ? q_out : i_out));
        iq[2 * i + 1] = (float)(level * (o->swap_iq ? i_out : q_out));
    }
    long clipped = write_wav(o->out, iq, frames, 2, IQ_RATE, o->bits);
    printf("SYNTH %s center_hz=%g if_trim_hz=%g dial_hz=%g gain_db=%g phase_deg=%g image_db=%.1f clipped=%ld\n",
           o->out, center, o->if_trim_hz, dial, o->gain_db, o->phase_deg, measure_image_db(o, center), clipped);
    free(iq);
    free(analytic);
    free(mirror_signal);
    return clipped < 0 ? -1 : 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: iq_convert [--center-hz F]... [--if-trim-hz T] [--swap-iq] [--negate-q] [--output-rate R] -o OUT.wav IQ.wav\n"
            "       iq_convert --real [--output-rate R] -o OUT.wav AUDIO.wav\n"
            "       iq_convert --synth [--center-hz F] [--if-trim-hz T] [--swap-iq] [--negate-q] [--gain-db G]\n"
            "                  [--phase-deg P] [--dc-db D] [--mirror-tone-db L] [--alias-tone-db L]\n"
            "                  [--tone-offset-hz H] [--mirror-wav W] [--mirror-wav-db L] [--level-db L]\n"
            "                  [--bits 24|16] -o IQ.wav AUDIO_12K.wav\n");
    exit(2);
}

int main(int argc, char** argv)
{
    options_t o;
    memset(&o, 0, sizeof(o));
    o.output_rate = 12000;
    o.dc_db = o.mirror_tone_db = o.alias_tone_db = -1000;
    o.tone_offset_hz = 230;
    o.level_db = -6;
    o.bits = 24;
    static const struct option longs[] = {
        {"center-hz", required_argument, 0, 'c'},     {"if-trim-hz", required_argument, 0, 't'},
        {"swap-iq", no_argument, 0, 'S'},             {"negate-q", no_argument, 0, 'N'},
        {"output-rate", required_argument, 0, 'r'},   {"real", no_argument, 0, 'R'},
        {"synth", no_argument, 0, 'Y'},               {"gain-db", required_argument, 0, 'g'},
        {"phase-deg", required_argument, 0, 'p'},     {"dc-db", required_argument, 0, 'd'},
        {"mirror-tone-db", required_argument, 0, 'm'}, {"alias-tone-db", required_argument, 0, 'a'},
        {"tone-offset-hz", required_argument, 0, 'f'}, {"level-db", required_argument, 0, 'l'},
        {"mirror-wav", required_argument, 0, 'M'},    {"mirror-wav-db", required_argument, 0, 'D'},
        {"bits", required_argument, 0, 'b'},          {"out", required_argument, 0, 'o'},
        {0, 0, 0, 0},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "o:", longs, NULL)) != -1) {
        switch (opt) {
            case 'c':
                if (o.n_centers == MAX_CENTERS) usage();
                o.centers[o.n_centers++] = atof(optarg);
                break;
            case 't': o.if_trim_hz = atof(optarg); break;
            case 'S': o.swap_iq = 1; break;
            case 'N': o.negate_q = 1; break;
            case 'r': o.output_rate = atoi(optarg); break;
            case 'R': o.real = 1; break;
            case 'Y': o.synth = 1; break;
            case 'g': o.gain_db = atof(optarg); break;
            case 'p': o.phase_deg = atof(optarg); break;
            case 'd': o.dc_db = atof(optarg); break;
            case 'm': o.mirror_tone_db = atof(optarg); break;
            case 'a': o.alias_tone_db = atof(optarg); break;
            case 'f': o.tone_offset_hz = atof(optarg); break;
            case 'l': o.level_db = atof(optarg); break;
            case 'M': o.mirror_wav = optarg; break;
            case 'D': o.mirror_wav_db = atof(optarg); break;
            case 'b': o.bits = atoi(optarg); break;
            case 'o': o.out = optarg; break;
            default: usage();
        }
    }
    if (optind != argc - 1 || o.out == NULL || (o.real && o.synth) || (o.bits != 16 && o.bits != 24)) usage();
    if (o.real && (o.n_centers || o.swap_iq || o.negate_q || o.if_trim_hz != 0)) {
        fprintf(stderr, "iq_convert --real: the dial is at 0 Hz; centre, trim and I/Q flags do not apply\n");
        return 2;
    }
    if (o.synth && o.n_centers > 1) usage();
    if (o.n_centers == 0) o.centers[o.n_centers++] = 13500.0;

    wav_t in;
    if (read_wav(argv[optind], &in) != 0) {
        fprintf(stderr, "iq_convert: cannot read %s as 16- or 24-bit PCM WAV\n", argv[optind]);
        return 1;
    }
    int rc = 0;
    if (o.synth) {
        wav_t mirror;
        int have_mirror = o.mirror_wav != NULL;
        if (have_mirror && read_wav(o.mirror_wav, &mirror) != 0) {
            fprintf(stderr, "iq_convert: cannot read %s as 16- or 24-bit PCM WAV\n", o.mirror_wav);
            return 1;
        }
        rc = synthesize(&o, &in, have_mirror ? &mirror : NULL);
        if (have_mirror) free(mirror.samples);
    } else if (o.real) {
        // I = L + R (twice the average: the filter keeps half a real signal's power), Q = 0.
        float* iq = calloc(2 * in.frames, sizeof(float));
        for (size_t i = 0; i < in.frames; ++i) {
            float sum = 0.0f;
            for (int ch = 0; ch < in.channels; ++ch) sum += in.samples[i * in.channels + ch];
            iq[2 * i] = 2.0f * sum / (float)in.channels;
        }
        rc = receive(&o, iq, in.frames, in.rate, 1500.0, o.out);
        free(iq);
    } else {
        if (in.channels != 2) {
            fprintf(stderr, "iq_convert: %s has %d channel(s); I/Q needs 2\n", argv[optind], in.channels);
            return 1;
        }
        for (int c = 0; c < o.n_centers && rc == 0; ++c) {
            char path[1024];
            output_name(path, sizeof(path), o.out, o.centers[c], o.n_centers > 1);
            rc = receive(&o, in.samples, in.frames, in.rate, o.centers[c], path);
        }
    }
    free(in.samples);
    return rc == 0 ? 0 : 1;
}
