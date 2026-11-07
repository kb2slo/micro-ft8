// qmx_iq_channel.h: an FT4 channel beside FT8 in the QMX's I/Q window, band by band.
#include <cstdio>
#include <cstring>

#include "qmx_iq_channel.h"

static int g_fails = 0;

static void expect(bool ok, const char* msg)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    }
}

// FT4's dial minus FT8's, in Hz, from webapp/app/bands.js and kFt4DefaultKhz.
static int32_t offset(double ft4_khz, double ft8_khz)
{
    return (int32_t)((ft4_khz - ft8_khz) * 1000.0 + (ft4_khz >= ft8_khz ? 0.5 : -0.5));
}

int main()
{
    expect(qmx_iq_channel_center_hz(0) == 13500.0, "FT8 is 13,500 Hz above the LO");
    expect(qmx_iq_channel_center_hz(offset(14080, 14074)) == 19500.0, "20 m FT4 is 19,500 Hz above it");
    expect(qmx_iq_channel_center_hz(offset(7047.5, 7074)) == -13000.0, "40 m FT4 is 13,000 Hz below it");
    // Fits: 160, 80, 60, 40, 30, 20, 17, 12, 6 and 2 m.
    const double fits[][2] = {{1843, 1840}, {3575, 3573}, {5357, 5357}, {7047.5, 7074}, {10140, 10136},
                              {14080, 14074}, {18104, 18100}, {24919, 24915}, {50318, 50313}, {144170, 144174}};
    for (const auto& b : fits) {
        char msg[80];
        snprintf(msg, sizeof msg, "FT4 at %.1f kHz beside FT8 at %.0f fits", b[0], b[1]);
        expect(qmx_iq_channel_refusal(offset(b[0], b[1])) == nullptr, msg);
    }
    // 15 and 10 m: FT4 is 66 and 106 kHz from FT8, outside the 48 kHz window.
    expect(qmx_iq_channel_refusal(offset(21140, 21074)) != nullptr, "15 m FT4 is refused");
    expect(qmx_iq_channel_refusal(offset(28180, 28074)) != nullptr, "10 m FT4 is refused");
    // The edges: audio to 21.0 kHz either side, and never over the LO.
    expect(qmx_iq_channel_refusal(6000) == nullptr, "19,500 + 1,500 reaches the edge exactly");
    expect(qmx_iq_channel_refusal(6001) != nullptr, "a hertz past it does not");
    expect(qmx_iq_channel_refusal(-33000) == nullptr, "audio down to -21,000 at the bottom edge fits");
    expect(qmx_iq_channel_refusal(-33001) != nullptr, "a hertz below does not");
    expect(qmx_iq_channel_refusal(-13500) != nullptr, "a channel centred on the LO is refused");
    expect(qmx_iq_channel_refusal(-12000) == nullptr, "one that ends at the LO fits");
    if (g_fails == 0) printf("qmx_iq_channel: all checks passed\n");
    return g_fails ? 1 : 0;
}
