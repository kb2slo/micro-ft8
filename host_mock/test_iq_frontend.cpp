// Host test for iq_frontend.h, the I/Q receive front end (RFC 0014 §3.4): a tone swept
// across the whole input band for passband flatness and stopband rejection, output bit-identical
// however the input is chunked, and image rejection under a gain and phase imbalance, in dB.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "iq_frontend.h"

static int g_fails = 0;

static void check(bool ok, const char* msg)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    }
}

static const double kTwoPi = 6.283185307179586;
static const double kCenterHz = 13500.0;  // a QMX's FT8 channel: dial 12 kHz above the LO, plus 1,500 Hz

static iq_frontend_config_t qmx_config(uint32_t output_rate)
{
    iq_frontend_config_t config = {};
    config.input_rate_hz = 48000;
    config.output_rate_hz = output_rate;
    config.center_hz = kCenterHz;
    return config;
}

// A complex tone at `hz` from the LO, amplitude 1, through an I/Q mixer whose Q path has
// `gain_db` more gain and `phase_deg` more phase than I. Samples [start, start + frames).
static std::vector<float> tone(double hz, size_t frames, double gain_db = 0.0, double phase_deg = 0.0)
{
    std::vector<float> iq(2 * frames);
    double g = pow(10.0, gain_db / 20.0);
    double phi = phase_deg * kTwoPi / 360.0;
    for (size_t n = 0; n < frames; ++n) {
        double theta = kTwoPi * hz * (double)n / 48000.0;
        iq[2 * n] = (float)cos(theta);
        iq[2 * n + 1] = (float)(g * sin(theta + phi));
    }
    return iq;
}

static std::vector<float> run(iq_frontend_t* frontend, const std::vector<float>& iq)
{
    size_t frames = iq.size() / 2;
    std::vector<float> out(iq_frontend_output_capacity(frontend, frames));
    int n = iq_frontend_process(frontend, iq.data(), frames, out.data(), out.size());
    out.resize(n < 0 ? 0 : (size_t)n);
    return out;
}

// Amplitude of the output at `audio_hz`, by projection over the last `count` samples (a whole
// number of cycles on the test's 10 Hz grid), so another frequency in the output does not count.
static double amplitude_at(const std::vector<float>& y, size_t count, double audio_hz, double rate)
{
    double c = 0.0, s = 0.0;
    size_t start = y.size() - count;
    for (size_t m = start; m < y.size(); ++m) {
        double a = kTwoPi * audio_hz * (double)m / rate;
        c += y[m] * cos(a);
        s += y[m] * sin(a);
    }
    return 2.0 * sqrt(c * c + s * s) / (double)count;
}

static double rms(const std::vector<float>& y, size_t count)
{
    double sum = 0.0;
    for (size_t m = y.size() - count; m < y.size(); ++m) sum += (double)y[m] * y[m];
    return sqrt(sum / (double)count);
}

static double wrap(double hz, double rate)
{
    double w = fmod(hz, rate);
    if (w < -rate / 2) w += rate;
    if (w >= rate / 2) w -= rate;
    return w;
}

// The sweep: every 50 Hz from -24 to +24 kHz. In band, each tone must come out at its audio
// frequency within 0.1 dB peak to peak; any tone that lands inside the audio band from anywhere else
// (the other sideband, an alias of a decimation, the far side of the LO) at least 70 dB down.
static void test_sweep(uint32_t output_rate)
{
    iq_frontend_t frontend;
    iq_frontend_config_t config = qmx_config(output_rate);
    check(iq_frontend_init(&frontend, &config) == IQ_FRONTEND_OK, "init");
    const double fo = output_rate;
    const double nco = iq_frontend_nco_hz(&frontend);
    const int half_width = frontend.audio_high_hz - frontend.shift_hz;
    const size_t settle = 4800;   // 0.1 s: every filter's delay many times over
    const size_t measure = 4800;  // 0.1 s, a whole number of cycles of any 10 Hz multiple
    const size_t out_count = measure * output_rate / 48000;

    double pass_min = 1e9, pass_max = -1e9, stop_worst = -1e9, stop_worst_hz = 0;
    int pass_n = 0, stop_n = 0;
    for (int hz = -24000; hz <= 24000; hz += 50) {
        iq_frontend_t run_frontend;
        check(iq_frontend_init(&run_frontend, &config) == IQ_FRONTEND_OK, "init per tone");
        std::vector<float> y = run(&run_frontend, tone(hz, settle + measure));

        double b = wrap(hz - nco, 48000.0);
        double audio = fabs(wrap(wrap(b, fo) + frontend.shift_hz, fo));
        bool in_band = audio >= frontend.audio_low_hz && audio <= frontend.audio_high_hz;
        if (fabs(b) <= half_width) {
            double db = 20.0 * log10(amplitude_at(y, out_count, audio, fo));
            pass_min = fmin(pass_min, db);
            pass_max = fmax(pass_max, db);
            ++pass_n;
        } else if (in_band) {
            double db = 20.0 * log10(rms(y, out_count) * sqrt(2.0) + 1e-30);
            if (db > stop_worst) {
                stop_worst = db;
                stop_worst_hz = hz;
            }
            ++stop_n;
        }
    }
    printf("sweep at %u Hz out: audio %d-%d Hz, NCO %.4f Hz\n", output_rate, frontend.audio_low_hz,
           frontend.audio_high_hz, nco);
    printf("  passband: %d tones, %+.4f .. %+.4f dB (%.4f dB peak to peak)\n", pass_n, pass_min, pass_max,
           pass_max - pass_min);
    printf("  into the audio band from outside the channel: %d tones, worst %.1f dB (tone at %+.0f Hz)\n",
           stop_n, stop_worst, stop_worst_hz);
    // The decoder searches from 200 Hz (ft8_audio_pipeline.cpp) and the QMX's audio passes to about
    // 150 Hz: the flat band starts at 150 Hz, and the sweep's tone at 150 Hz is inside it.
    check(frontend.audio_low_hz == 150, "the flat band starts at 150 Hz");
    check(frontend.audio_high_hz >= 2850, "the flat band reaches the top of the decoder's search");
    check(pass_n > 50, "sweep reaches the passband");
    check(pass_max - pass_min <= 0.1 && fabs(pass_max) <= 0.1 && fabs(pass_min) <= 0.1,
          "passband flat within 0.1 dB");
    check(stop_n > 300, "sweep reaches the stopband");
    check(stop_worst <= -70.0, "at least 70 dB down wherever a tone would alias into the audio band");
}

// The same input, in one call and in uneven chunks, gives the same bits.
static void test_chunking()
{
    iq_frontend_config_t config = qmx_config(12000);
    config.if_trim_hz = 37.25;
    const size_t frames = 60000;
    std::vector<float> iq(2 * frames);
    uint32_t r = 2463534242u;
    for (size_t n = 0; n < frames; ++n) {
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        double noise = ((double)(r & 0xffff) / 32768.0 - 1.0) * 0.1;
        double theta = kTwoPi * 14123.0 * (double)n / 48000.0;
        iq[2 * n] = (float)(0.5 * cos(theta) + noise);
        iq[2 * n + 1] = (float)(0.5 * sin(theta) - noise);
    }

    iq_frontend_t whole;
    iq_frontend_init(&whole, &config);
    std::vector<float> expected = run(&whole, iq);

    const size_t sizes[] = {1, 3, 7, 64, 1000, 4095, 2, 513};
    for (int pass = 0; pass < 2; ++pass) {
        // pass 1 interleaves a second instance between chunks: no shared state.
        iq_frontend_t frontend, other;
        iq_frontend_init(&frontend, &config);
        iq_frontend_init(&other, &config);
        std::vector<float> got;
        std::vector<float> chunk_out(4096);
        size_t at = 0;
        int which = 0;
        while (at < frames) {
            size_t n = sizes[which++ % (sizeof(sizes) / sizeof(sizes[0]))];
            if (n > frames - at) n = frames - at;
            int w = iq_frontend_process(&frontend, &iq[2 * at], n, chunk_out.data(), chunk_out.size());
            got.insert(got.end(), chunk_out.begin(), chunk_out.begin() + w);
            if (pass == 1) iq_frontend_process(&other, &iq[0], n, chunk_out.data(), chunk_out.size());
            at += n;
        }
        bool same = got.size() == expected.size()
                    && memcmp(got.data(), expected.data(), got.size() * sizeof(float)) == 0;
        check(same, pass == 0 ? "chunked output bit-identical to one call"
                              : "bit-identical with a second instance running between chunks");
    }
    check(expected.size() == frames / 4, "12 kHz out is a quarter of the input frames");

    std::vector<float> small(2);
    check(iq_frontend_process(&whole, iq.data(), 100, small.data(), small.size()) == -1,
          "refuses an output buffer smaller than the capacity");
}

// iq_frontend_reset() gives what a fresh init gives, bit for bit, mid-stream and after any chunking.
static void test_reset()
{
    iq_frontend_config_t config = qmx_config(6000);
    const size_t frames = 24000;
    std::vector<float> iq(2 * frames);
    for (size_t n = 0; n < frames; ++n) {
        double theta = kTwoPi * 14123.0 * (double)n / 48000.0;
        iq[2 * n] = (float)(0.5 * cos(theta) + 0.1 * sin(0.37 * (double)n));
        iq[2 * n + 1] = (float)(0.5 * sin(theta));
    }
    iq_frontend_t fresh, used;
    iq_frontend_init(&fresh, &config);
    iq_frontend_init(&used, &config);
    std::vector<float> expected = run(&fresh, iq);
    run(&used, iq);                                  // leave state in every stage
    std::vector<float> odd(2 * 777, 0.25f);
    run(&used, odd);                                 // and an odd phase in the half-bands
    iq_frontend_reset(&used);
    std::vector<float> got = run(&used, iq);
    check(got.size() == expected.size()
              && memcmp(got.data(), expected.data(), got.size() * sizeof(float)) == 0,
          "reset gives a fresh front end's output, bit for bit");
}

// Image rejection: a tone at the channel's mirror about the LO, through an imbalanced mixer, against
// the same tone at the channel. Reported in dB; checked against the textbook image-rejection ratio
// (1 + 2g cos phi + g^2) / (1 - 2g cos phi + g^2).
static double image_db(double gain_db, double phase_deg)
{
    iq_frontend_config_t config = qmx_config(12000);
    const double audio = 1730.0;
    const double wanted = kCenterHz - 1500.0 + audio;
    const size_t frames = 9600;
    const size_t out_count = 1200;

    iq_frontend_t frontend;
    iq_frontend_init(&frontend, &config);
    double signal = amplitude_at(run(&frontend, tone(wanted, frames, gain_db, phase_deg)), out_count, audio, 12000);
    iq_frontend_init(&frontend, &config);
    double image = amplitude_at(run(&frontend, tone(-wanted, frames, gain_db, phase_deg)), out_count, audio, 12000);
    return 20.0 * log10(image / signal + 1e-30);
}

static void test_image()
{
    printf("image rejection (a tone at the mirror, against the same tone in the channel):\n");
    const double cases[][2] = {{0.0, 0.0}, {0.1, 0.5}, {0.5, 2.0}, {1.0, 1.0}, {2.0, 5.0}};
    for (const auto& c : cases) {
        double measured = image_db(c[0], c[1]);
        double g = pow(10.0, c[0] / 20.0);
        double cp = cos(c[1] * kTwoPi / 360.0);
        double theory = (c[0] == 0.0 && c[1] == 0.0)
                            ? -1e9
                            : -10.0 * log10((1 + 2 * g * cp + g * g) / (1 - 2 * g * cp + g * g));
        printf("  gain %.1f dB, phase %.1f deg: image %.1f dB", c[0], c[1], measured);
        if (theory > -1e8) printf(" (textbook %.1f dB)", theory);
        printf("\n");
        if (theory > -1e8) {
            check(fabs(measured - theory) < 0.2, "image matches the textbook ratio");
        } else {
            check(measured <= -70.0, "a balanced mixer's image is at least 70 dB down");
        }
    }

    // An RF signal 48 kHz from the channel is sampled onto the channel itself: an alias, which no
    // balance correction removes. It comes through at full level; only the radio's analog filter
    // ahead of the ADC can stop it. Pinned so an on-air ghost at +48 kHz is not mistaken for an image.
    iq_frontend_config_t config = qmx_config(12000);
    iq_frontend_t frontend;
    iq_frontend_init(&frontend, &config);
    const double audio = 1730.0;
    double alias = amplitude_at(run(&frontend, tone(kCenterHz - 1500.0 + audio - 48000.0, 9600)), 1200, audio, 12000);
    printf("  a tone 48 kHz below the channel, sampled at 48 kHz: %.2f dB (an alias, not an image)\n",
           20.0 * log10(alias));
    check(fabs(20.0 * log10(alias)) < 0.1, "a 48 kHz alias passes at full level");
}

// swap_iq and negate_q each move a signal to the mirror; the trim moves the channel by that many Hz.
static void test_format_and_trim()
{
    const double audio = 1730.0;
    const double wanted = kCenterHz - 1500.0 + audio;
    for (int which = 0; which < 2; ++which) {
        iq_frontend_config_t config = qmx_config(12000);
        if (which == 0) config.swap_iq = true;
        else config.negate_q = true;
        iq_frontend_t frontend;
        iq_frontend_init(&frontend, &config);
        double a = amplitude_at(run(&frontend, tone(-wanted, 9600)), 1200, audio, 12000);
        check(fabs(20.0 * log10(a)) < 0.1, which == 0 ? "swap_iq takes the band from below the LO"
                                                      : "negate_q takes the band from below the LO");
    }

    iq_frontend_config_t config = qmx_config(12000);
    config.if_trim_hz = -61.5;
    iq_frontend_t frontend;
    iq_frontend_init(&frontend, &config);
    double a = amplitude_at(run(&frontend, tone(wanted - 61.5, 9600)), 1200, audio, 12000);
    printf("trim -61.5 Hz: NCO %.4f Hz, tone 61.5 Hz low comes out at %.0f Hz at %.3f dB\n",
           iq_frontend_nco_hz(&frontend), audio, 20.0 * log10(a));
    check(fabs(20.0 * log10(a)) < 0.1, "the trim moves the channel");
    check(fabs(iq_frontend_nco_hz(&frontend) - (kCenterHz - 61.5 - 1500.0 + frontend.shift_hz)) < 1e-4,
          "the NCO reports the applied trim");

    iq_frontend_config_t bad = qmx_config(12000);
    bad.output_rate_hz = 16000;
    check(iq_frontend_init(&frontend, &bad) == IQ_FRONTEND_BAD_RATES, "48 to 16 kHz is not a power of two");
    bad = qmx_config(12000);
    bad.center_hz = 23000;
    check(iq_frontend_init(&frontend, &bad) == IQ_FRONTEND_BAD_CENTER, "a channel past the band edge is refused");
}

int main()
{
    test_sweep(12000);
    test_sweep(6000);
    test_chunking();
    test_reset();
    test_image();
    test_format_and_trim();
    if (g_fails) {
        fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    printf("iq_frontend: all passed\n");
    return 0;
}
