// The HL2's receive front end (components/hl2_proto/include/hl2_rx_frontend.h, through components/iq_frontend):
// with the receiver's NCO on the dial, a signal above the dial, as the HL2 sends it (Q negated, found on the bench
// 2026-11-03), comes out at that audio frequency, and one below it is rejected. Mirrored, nothing decodes.

#include <cmath>
#include <cstdio>
#include <vector>

#include "hl2_rx_frontend.h"

static int g_fails = 0;

static void expect(bool ok, const char* msg)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    }
}

// Power at hz in the output (Goertzel), after the filters have settled.
static double tone_power(const std::vector<float>& x, double hz, double rate)
{
    const double w = 2.0 * M_PI * hz / rate;
    const double c = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = x.size() / 4; i < x.size(); ++i) {
        const double s0 = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

// A complex tone `offset_hz` from the NCO (negative: below the dial) as the HL2 sends it, I = cos and Q = -sin,
// 1 s at 48 kHz, through the HL2's front end.
static std::vector<float> run(double offset_hz)
{
    static iq_frontend_t fe;
    expect(iq_frontend_init(&fe, &kHl2RxFrontendConfig) == IQ_FRONTEND_OK, "the HL2's configuration is accepted");
    const size_t frames = 48000;
    std::vector<float> iq(2 * frames);
    for (size_t n = 0; n < frames; ++n) {
        const double ph = 2.0 * M_PI * offset_hz * n / 48000.0;
        iq[2 * n] = 0.25f * static_cast<float>(std::cos(ph));      // I
        iq[2 * n + 1] = -0.25f * static_cast<float>(std::sin(ph));  // Q, as the HL2 sends it
    }
    std::vector<float> out(iq_frontend_output_capacity(&fe, frames));
    const int n = iq_frontend_process(&fe, iq.data(), frames, out.data(), out.size());
    out.resize(n > 0 ? n : 0);
    return out;
}

int main()
{
    const double rate = kHl2RxFrontendConfig.output_rate_hz;
    // Above the dial against below it is the whole test of orientation: iq_frontend_nco_hz() includes the front
    // end's own shift into the audio band, so it is not where the dial sits.

    const double up1000 = tone_power(run(1000.0), 1000.0, rate);
    const double down1000 = tone_power(run(-1000.0), 1000.0, rate);
    expect(up1000 > 0, "a tone 1 kHz above the dial comes out");
    const double reject_db = 10.0 * std::log10(up1000 / (down1000 + 1e-30));
    printf("1 kHz above against 1 kHz below the dial: %.1f dB\n", reject_db);
    expect(reject_db > 40.0, "one 1 kHz below the dial is rejected by more than 40 dB");

    // Flat across FT8's band: 500 Hz to 2.5 kHz within 1 dB of 1 kHz.
    for (double hz : {500.0, 1500.0, 2500.0}) {
        const double p = tone_power(run(hz), hz, rate);
        const double db = 10.0 * std::log10(p / up1000);
        printf("%.0f Hz above the dial: %+.2f dB against 1 kHz\n", hz, db);
        expect(std::fabs(db) < 1.0, "flat across 500 Hz to 2.5 kHz");
    }

    if (g_fails) {
        fprintf(stderr, "test_hl2_frontend: %d failure(s)\n", g_fails);
        return 1;
    }
    printf("test_hl2_frontend: ok\n");
    return 0;
}
