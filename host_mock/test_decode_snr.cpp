#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <vector>

extern "C" {
#include "decode_snr.h"
#include "ft8/constants.h"
#include "ft8/encode.h"
}

// The firmware's FT8 shape: 6 kHz, f 200-2900 Hz, time_osr 2, freq_osr 2, decode at 79 blocks.
static const int kBlocks = 79;
static const int kBins = 432;
static const int kBlockSize = 960;
static const int kNoiseCode = 100;  // -70 dB

static int g_fails = 0;

static void expect_int(int got, int want, const char* msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %d want %d)\n", msg, got, want);
        ++g_fails;
    }
}

static void expect_near(int got, int want, int tolerance, const char* msg)
{
    if (got < want - tolerance || got > want + tolerance) {
        fprintf(stderr, "FAIL: %s (got %d want %d +/- %d)\n", msg, got, want, tolerance);
        ++g_fails;
    }
}

struct Slot {
    monitor_t monitor;
    std::vector<uint8_t> mag;
    std::vector<float> window;
    std::vector<float> gain;

    Slot()
    {
        memset(&monitor, 0, sizeof(monitor));
        monitor.symbol_period = FT8_SYMBOL_PERIOD;
        monitor.block_size = kBlockSize;
        monitor.subblock_size = kBlockSize / 2;
        monitor.nfft = kBlockSize * 2;
        window.resize(monitor.nfft);
        for (int n = 0; n < monitor.nfft; ++n) {
            const float s = sinf((float)M_PI * (float)n / (float)monitor.nfft);
            window[n] = s * s;
        }
        monitor.window = window.data();
        ftx_waterfall_t& wf = monitor.wf;
        wf.max_blocks = 93;
        wf.num_blocks = kBlocks;
        wf.num_bins = kBins;
        wf.time_osr = 2;
        wf.freq_osr = 2;
        wf.block_stride = wf.time_osr * wf.freq_osr * wf.num_bins;
        wf.protocol = FTX_PROTOCOL_FT8;
        mag.assign((size_t)wf.max_blocks * wf.block_stride, (uint8_t)kNoiseCode);
        wf.mag = mag.data();
        gain.assign(wf.max_blocks, 1.0f);
    }

    uint8_t& cell(int block, int time_sub, int freq_sub, int bin)
    {
        const ftx_waterfall_t& wf = monitor.wf;
        return mag[(size_t)block * wf.block_stride + (size_t)time_sub * wf.freq_osr * wf.num_bins +
                   (size_t)freq_sub * wf.num_bins + (size_t)bin];
    }
};

static const ftx_candidate_t kCandidate = { 0, 20, 200, 0, 1 };

static void payload(uint8_t* out)
{
    for (int i = 0; i < FTX_PAYLOAD_LENGTH_BYTES; ++i) out[i] = (uint8_t)(0x5A ^ (i * 37));
}

// The decoded message's tones `rise` codes (0.5 dB each) above whatever the cell holds.
static void add_signal(Slot& slot, const uint8_t* bytes, int rise)
{
    uint8_t tones[FT8_NN];
    ft8_encode(bytes, tones);
    for (int k = 0; k < FT8_NN; ++k) {
        const int block = kCandidate.time_offset + k;
        if (block >= slot.monitor.wf.num_blocks) break;
        uint8_t& c = slot.cell(block, kCandidate.time_sub, kCandidate.freq_sub, kCandidate.freq_offset + tones[k]);
        c = (uint8_t)(c + rise);
    }
}

static int measure(Slot& slot, const uint8_t* bytes)
{
    decode_snr_slot_t snr;
    if (!decode_snr_prepare(&snr, &slot.monitor, slot.gain.data())) return -999;
    return decode_snr_message(&snr, &slot.monitor, &kCandidate, bytes);
}

int main()
{
    uint8_t bytes[FTX_PAYLOAD_LENGTH_BYTES];
    payload(bytes);

    // 16 dB in the tone cell over a flat floor: 10*log10(10^1.6 - 1) - 26.1 = -10.2.
    {
        Slot slot;
        add_signal(slot, bytes, 32);
        expect_int(measure(slot, bytes), -10, "flat floor, tones 16 dB up");
    }

    // A station next door, about 74 dB on this scale, is sent as +49: above that a WSJT-X partner reads garbage.
    {
        Slot slot;
        std::fill(slot.mag.begin(), slot.mag.end(), (uint8_t)40);
        add_signal(slot, bytes, 200);
        expect_int(measure(slot, bytes), DECODE_SNR_CEILING_DB, "a very strong signal is capped at +49");
    }

    // Tone cells at the floor: no excess power, so jt9's floor.
    {
        Slot slot;
        expect_int(measure(slot, bytes), DECODE_SNR_FLOOR_DB, "no signal reads the floor");
    }

    // A busy band: other stations fill 60% of the columns within 400 Hz, 20 dB up for the whole slot.
    // The baseline is the quiet 10th percentile, so the report does not move.
    {
        Slot slot;
        const int center = (kCandidate.freq_offset + 4) * 2 + kCandidate.freq_sub;
        for (int column = center - 128; column <= center + 128; ++column) {
            if (column % 5 < 2) continue;
            if (column >= center - 20 && column <= center + 20) continue;  // the signal's own columns
            for (int t = 0; t < kBlocks * 2; ++t) {
                slot.cell(t / 2, t % 2, column % 2, column / 2) = (uint8_t)(kNoiseCode + 40);
            }
        }
        add_signal(slot, bytes, 32);
        expect_near(measure(slot, bytes), -10, 1, "busy columns do not raise the noise");
    }

    // Frames that reach back before the slot hold the previous slot's audio on the device. Not measured.
    {
        Slot slot;
        add_signal(slot, bytes, 32);
        for (int t = 0; t < 3; ++t) {
            for (int f = 0; f < 2 * kBins; ++f) slot.mag[(size_t)t * 2 * kBins + f] = 255;
        }
        expect_int(measure(slot, bytes), -10, "frames before the slot are not measured");
    }

    // Per-block AGC: before the signal starts (blocks 0-19) the AGC ran 20 dB hotter, as it does when a busy
    // band's stations start late. Each cell's code carries the gain^2 under its analysis frame, which spans
    // the four half-blocks ending at that cell, Hann^2-weighted. Dividing the gain back out gives the flat case.
    {
        Slot slot;
        for (int b = 0; b < 20; ++b) slot.gain[b] = 10.0f;
        float weight[4] = { 0, 0, 0, 0 };
        float total = 0.0f;
        for (int n = 0; n < slot.monitor.nfft; ++n) {
            weight[n / slot.monitor.subblock_size] += slot.window[n] * slot.window[n];
            total += slot.window[n] * slot.window[n];
        }
        for (int t = 3; t < kBlocks * 2; ++t) {
            float g2 = 0.0f;
            for (int q = 0; q < 4; ++q) {
                const float g = slot.gain[(t - 3 + q) / 2];
                g2 += weight[q] / total * g * g;
            }
            const int shift = (int)lrintf(20.0f * log10f(g2));
            for (int f = 0; f < 2 * kBins; ++f) slot.mag[(size_t)t * 2 * kBins + f] = (uint8_t)(kNoiseCode + shift);
        }
        add_signal(slot, bytes, 32);
        expect_near(measure(slot, bytes), -10, 1, "AGC gain divided back out");
        // Without that, the hot start inflates the noise about 14 dB and the same signal reads about -21.
        for (int b = 0; b < 20; ++b) slot.gain[b] = 1.0f;
        const int uncorrected = measure(slot, bytes);
        if (uncorrected > -18) {
            fprintf(stderr, "FAIL: the hot start, left in, should read at least 8 dB low (got %d)\n", uncorrected);
            ++g_fails;
        }
    }

    // A waterfall wider than the column store is refused, and every SNR reads the floor.
    {
        Slot slot;
        slot.monitor.wf.freq_osr = 3;
        decode_snr_slot_t snr;
        expect_int(decode_snr_prepare(&snr, &slot.monitor, slot.gain.data()), 0, "too many columns refused");
        expect_int(decode_snr_message(&snr, &slot.monitor, &kCandidate, bytes), DECODE_SNR_FLOOR_DB,
                   "unprepared slot reads the floor");
    }

    if (g_fails) {
        fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    printf("decode_snr: all passed\n");
    return 0;
}
