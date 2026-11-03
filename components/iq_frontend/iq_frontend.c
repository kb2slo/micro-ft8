// Receive front end: I/Q to upper-sideband audio (iq_frontend.h). Written from RFC 0014 §3.4 and
// textbook multirate DSP (Kaiser-windowed FIR design, half-band decimators).
//
// The filter, and why this structure
//
// The decoder searches 200-2,900 Hz of audio (ft8_audio_pipeline.cpp's f_min and f_max), and the QMX's
// own audio mode passes down to about 150 Hz, so this front end is flat from L = 150 Hz up: below the
// decoder's floor, so the edge's roll-off does not touch the lowest signals it searches. Put the
// channel's middle at 0 Hz and call P the half-width of the flat band: P = 1,350 Hz at a 6 kHz output.
// After the real part, a component at b Hz of the filtered channel lands at |b + shift| of audio,
// where shift = P + L. So three regions matter:
//
//   * |b| <= P is the audio band: flat within 0.1 dB.
//   * b <= -(P + 2L) is the other sideband. Taking the real part folds it onto the audio band
//     (b = -(P + 2L) lands at L Hz), so it must be at least 70 dB down. The 2L Hz between is
//     audio 0-L Hz, which is the transition band, and it is what sets the filter's length.
//   * Anything that aliases onto |b| <= P when the rate is halved, at every stage.
//
// The upper side needs no such stop at a 12 kHz output (b just above P lands just above the band's
// top edge), but a symmetric low-pass is cheaper to run than a complex one, so the filter is
// symmetric: pass |b| <= P, stop |b| >= P + 2L. That is why the shift is P + L rather than 1,500 Hz: it
// centres the filter on the band. center_hz still names the 1,500 Hz point, so the NCO sits at
// center_hz - 1,500 + shift.
//
// At a 6 kHz output, 3,000 Hz is the Nyquist frequency, and the transition band above P folds back
// under it: b in [P, P + 2L] lands at 6,000 - (shift + b). Keeping that above the band's top edge
// needs output_rate >= 4P + 4L, so P = 1,350 there and the flat band is 150-2,850 Hz.
//
// One filter with a 300 Hz transition at 48 kHz would be about 800 taps, run on every input sample.
// Instead:
//
//   1. Half-band decimators, 2:1 each, 48 -> 24 -> 12 (-> 6) kHz. Each must keep only what would
//      alias onto |b| <= P away, so its stop band starts at fs/2 - P and its transition is wide:
//      15 taps at 48 and 24 kHz, 19 at 12 kHz, and half of each half-band's taps are zero. What
//      aliases into a half-band's transition lands outside |b| <= P, where step 2 removes it or
//      the audio is outside the decoder's band.
//   2. One channel low-pass at the output rate carrying the sharp 2L = 300 Hz transition: 203 taps at
//      12 kHz, 103 at 6 kHz, symmetric, so half the multiplies.
//
// All are Kaiser-windowed sinc designed for 80 dB, 10 dB inside the 70 dB asked for, since the
// Kaiser length formula is an estimate; host_test_iq_frontend measures the cascade. Kaiser ripple
// is the same in both bands, so 80 dB stop also means a passband flat to about 0.001 dB.
//
// Cost, by count rather than measurement (RFC 0014 S1 measures it on the P4): per second of 48 kHz
// input at a 12 kHz output, about 0.7 M multiplies for the NCO and the half-bands, and 1.9 M for the
// channel filter's 77 folded taps on two rails; at a 6 kHz output the channel filter is about 0.5 M.

#include <math.h>
#include <string.h>

#include "iq_frontend.h"

#define DESIGN_ATTENUATION_DB 80.0
#define AUDIO_LOW_HZ 150            // L: the flat band's low edge (see the top of this file)
#define TRANSITION_HZ (2 * AUDIO_LOW_HZ)
#define SHIFT_MAX_HZ 1600           // keeps shift/output_rate to a short table at a 12 kHz output
#define NCO_RESYNC 256  // samples between exact NCO phases; the float rotation drifts ~1e-5 rad in that

#define TWO_PI 6.283185307179586476925286766559

// Kaiser's estimate of the taps for a given attenuation and transition (as a fraction of fs).
static int kaiser_taps(double attenuation_db, double transition)
{
    return (int)ceil((attenuation_db - 7.95) / (14.36 * transition)) + 1;
}

static double kaiser_beta(double attenuation_db)
{
    return 0.1102 * (attenuation_db - 8.7);
}

// Modified Bessel function of the first kind, order 0, by its power series.
static double bessel_i0(double x)
{
    double sum = 1.0;
    double term = 1.0;
    double half = x / 2.0;
    for (int k = 1; k < 64; ++k) {
        term *= (half / k) * (half / k);
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

// Windowed sinc, cutoff `cutoff` as a fraction of fs, at offset n from the middle of `taps`.
static double kaiser_sinc(int n, int taps, double cutoff, double beta)
{
    double half = (taps - 1) / 2.0;
    double r = n / half;
    double window = bessel_i0(beta * sqrt(1.0 - r * r)) / bessel_i0(beta);
    double ideal = (n == 0) ? 2.0 * cutoff : sin(TWO_PI * cutoff * n) / (TWO_PI / 2.0 * n);
    return ideal * window;
}

static int gcd(int a, int b)
{
    while (b != 0) {
        int t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static void design_halfband(iq_frontend_halfband_t* stage, int taps)
{
    double beta = kaiser_beta(DESIGN_ATTENUATION_DB);
    int sides = (taps + 1) / 4;
    double sum = kaiser_sinc(0, taps, 0.25, beta);
    double side[(IQ_FRONTEND_MAX_HALFBAND_TAPS + 1) / 4];
    for (int j = 0; j < sides; ++j) {
        side[j] = kaiser_sinc(2 * j + 1, taps, 0.25, beta);
        sum += 2.0 * side[j];
    }
    memset(stage, 0, sizeof(*stage));
    stage->taps = taps;
    stage->center = (float)(kaiser_sinc(0, taps, 0.25, beta) / sum);
    for (int j = 0; j < sides; ++j) stage->side[j] = (float)(side[j] / sum);
}

iq_frontend_status_t iq_frontend_init(iq_frontend_t* frontend, const iq_frontend_config_t* config)
{
    memset(frontend, 0, sizeof(*frontend));
    if (config->output_rate_hz < 6000 || config->input_rate_hz <= config->output_rate_hz
        || config->input_rate_hz % config->output_rate_hz != 0) {
        return IQ_FRONTEND_BAD_RATES;
    }
    uint32_t ratio = config->input_rate_hz / config->output_rate_hz;
    int stages = 0;
    while ((1u << stages) < ratio) ++stages;
    if ((1u << stages) != ratio || stages > IQ_FRONTEND_MAX_STAGES) return IQ_FRONTEND_BAD_RATES;

    int fo = (int)config->output_rate_hz;
    int half_width = (fo - 2 * TRANSITION_HZ) / 4;  // P: see the top of this file
    if (half_width > SHIFT_MAX_HZ - AUDIO_LOW_HZ) half_width = SHIFT_MAX_HZ - AUDIO_LOW_HZ;
    int shift = half_width + TRANSITION_HZ / 2;

    frontend->config = *config;
    frontend->stages = stages;
    frontend->shift_hz = shift;
    frontend->audio_low_hz = AUDIO_LOW_HZ;
    frontend->audio_high_hz = shift + half_width;

    double nco_hz = config->center_hz + config->if_trim_hz - 1500.0 + shift;
    double fs = config->input_rate_hz;
    if (fabs(nco_hz) + half_width + TRANSITION_HZ > fs / 2) return IQ_FRONTEND_BAD_CENTER;

    // Half-bands: pass |b| <= P, stop from fs_stage/2 - P.
    double stage_rate = fs;
    for (int s = 0; s < stages; ++s) {
        double transition = (stage_rate / 2.0 - 2.0 * half_width) / stage_rate;
        int taps = kaiser_taps(DESIGN_ATTENUATION_DB, transition);
        while (taps % 4 != 3) ++taps;
        if (taps > IQ_FRONTEND_MAX_HALFBAND_TAPS) return IQ_FRONTEND_TOO_MANY_TAPS;
        design_halfband(&frontend->halfband[s], taps);
        stage_rate /= 2.0;
    }

    // Channel low-pass: pass |b| <= P, stop |b| >= P + 2L, at the output rate.
    int taps = kaiser_taps(DESIGN_ATTENUATION_DB, (double)TRANSITION_HZ / fo);
    if (taps % 2 == 0) ++taps;
    if (taps > IQ_FRONTEND_MAX_CHANNEL_TAPS) return IQ_FRONTEND_TOO_MANY_TAPS;
    frontend->channel_taps = taps;
    double beta = kaiser_beta(DESIGN_ATTENUATION_DB);
    double cutoff = (half_width + TRANSITION_HZ / 2.0) / fo;
    // Two passes over the design rather than a coefficient array: init runs on a 4,096-byte task.
    double sum = 0.0;
    for (int k = 0; k <= taps / 2; ++k) {
        const double c = kaiser_sinc(k, taps, cutoff, beta);
        sum += (k == 0) ? c : 2.0 * c;
    }
    for (int k = 0; k <= taps / 2; ++k) frontend->channel[k] = (float)(kaiser_sinc(k, taps, cutoff, beta) / sum);

    // Shift up: shift/fo is p/q in lowest terms, so the phasor repeats every q samples.
    int period = fo / gcd(shift, fo);
    if (period > IQ_FRONTEND_MAX_SHIFT_PERIOD) return IQ_FRONTEND_TOO_MANY_TAPS;
    frontend->shift_period = period;
    for (int m = 0; m < period; ++m) {
        double angle = TWO_PI * (double)(((int64_t)m * shift) % fo) / fo;
        frontend->shift_cos[m] = (float)cos(angle);
        frontend->shift_sin[m] = (float)sin(angle);
    }

    // NCO: mix down by nco_hz. The step is the phase per sample in 2^-32 cycles; a negative
    // frequency wraps, which unsigned arithmetic does exactly.
    double cycles = nco_hz / fs;
    frontend->nco_step = (uint32_t)(int64_t)llround(cycles * 4294967296.0);
    double step_angle = -TWO_PI * (double)(int32_t)frontend->nco_step / 4294967296.0;
    frontend->nco_rotate.re = (float)cos(step_angle);
    frontend->nco_rotate.im = (float)sin(step_angle);
    return IQ_FRONTEND_OK;
}

void iq_frontend_reset(iq_frontend_t* frontend)
{
    frontend->input_count = 0;
    memset(&frontend->nco, 0, sizeof(frontend->nco));
    for (int s = 0; s < IQ_FRONTEND_MAX_STAGES; ++s) {
        iq_frontend_halfband_t* stage = &frontend->halfband[s];
        memset(stage->line, 0, sizeof(stage->line));
        stage->pos = 0;
        stage->skip = 0;
    }
    memset(frontend->channel_line, 0, sizeof(frontend->channel_line));
    frontend->channel_pos = 0;
    frontend->shift_index = 0;
}

size_t iq_frontend_output_capacity(const iq_frontend_t* frontend, size_t frames)
{
    return frames / ((size_t)1 << frontend->stages) + 1;
}

double iq_frontend_nco_hz(const iq_frontend_t* frontend)
{
    return (double)(int32_t)frontend->nco_step * frontend->config.input_rate_hz / 4294967296.0;
}

static iq_frontend_complex_t nco_exact(const iq_frontend_t* frontend)
{
    uint32_t phase = (uint32_t)(frontend->input_count * frontend->nco_step);
    double angle = -TWO_PI * (double)phase / 4294967296.0;
    iq_frontend_complex_t p = {(float)cos(angle), (float)sin(angle)};
    return p;
}

// Pushes one sample; returns true and sets *out on every second one.
static bool halfband_push(iq_frontend_halfband_t* stage, iq_frontend_complex_t x, iq_frontend_complex_t* out)
{
    int n = stage->taps;
    stage->line[stage->pos] = x;
    stage->line[stage->pos + n] = x;
    stage->pos = (stage->pos + 1) % n;
    stage->skip ^= 1;
    if (stage->skip) return false;

    // line[pos .. pos + n - 1] is oldest to newest; the middle is at pos + (n - 1) / 2.
    const iq_frontend_complex_t* w = &stage->line[stage->pos];
    int mid = (n - 1) / 2;
    float re = stage->center * w[mid].re;
    float im = stage->center * w[mid].im;
    for (int j = 0; j < (n + 1) / 4; ++j) {
        int d = 2 * j + 1;
        re += stage->side[j] * (w[mid - d].re + w[mid + d].re);
        im += stage->side[j] * (w[mid - d].im + w[mid + d].im);
    }
    out->re = re;
    out->im = im;
    return true;
}

static float channel_and_shift(iq_frontend_t* frontend, iq_frontend_complex_t x)
{
    int n = frontend->channel_taps;
    frontend->channel_line[frontend->channel_pos] = x;
    frontend->channel_line[frontend->channel_pos + n] = x;
    frontend->channel_pos = (frontend->channel_pos + 1) % n;

    const iq_frontend_complex_t* w = &frontend->channel_line[frontend->channel_pos];
    int mid = n / 2;
    float re = frontend->channel[0] * w[mid].re;
    float im = frontend->channel[0] * w[mid].im;
    for (int k = 1; k <= mid; ++k) {
        re += frontend->channel[k] * (w[mid - k].re + w[mid + k].re);
        im += frontend->channel[k] * (w[mid - k].im + w[mid + k].im);
    }

    // Re{(re + j im) e^{j 2 pi shift m / fo}}
    int m = frontend->shift_index;
    frontend->shift_index = (m + 1 == frontend->shift_period) ? 0 : m + 1;
    return re * frontend->shift_cos[m] - im * frontend->shift_sin[m];
}

int iq_frontend_process(iq_frontend_t* frontend, const float* iq, size_t frames, float* out, size_t out_capacity)
{
    if (out_capacity < iq_frontend_output_capacity(frontend, frames)) return -1;
    int written = 0;
    for (size_t i = 0; i < frames; ++i) {
        float in_i = iq[2 * i];
        float in_q = iq[2 * i + 1];
        if (frontend->config.swap_iq) {
            float t = in_i;
            in_i = in_q;
            in_q = t;
        }
        if (frontend->config.negate_q) in_q = -in_q;

        if (frontend->input_count % NCO_RESYNC == 0) frontend->nco = nco_exact(frontend);
        iq_frontend_complex_t p = frontend->nco;
        iq_frontend_complex_t x = {in_i * p.re - in_q * p.im, in_i * p.im + in_q * p.re};
        frontend->nco.re = p.re * frontend->nco_rotate.re - p.im * frontend->nco_rotate.im;
        frontend->nco.im = p.re * frontend->nco_rotate.im + p.im * frontend->nco_rotate.re;
        ++frontend->input_count;

        bool ready = true;
        for (int s = 0; s < frontend->stages && ready; ++s) {
            ready = halfband_push(&frontend->halfband[s], x, &x);
        }
        if (ready) out[written++] = channel_and_shift(frontend, x);
    }
    return written;
}
