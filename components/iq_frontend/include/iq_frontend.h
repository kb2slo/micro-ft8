// Receive front end: one channel of upper-sideband audio from a complex I/Q stream
// (docs/architecture/decode.md, **I/Q front end**).
//
// What it does, per channel:
//
//   I/Q at input_rate_hz ──NCO──> channel at 0 Hz ──half-band ÷2 (× stages)──> output_rate_hz
//     ──channel low-pass──> shift up into the audio band ──> real part = USB audio
//
// The output is what a receiver's audio output would be, so the decoder, its fitted SNR constant,
// the recall harness and the corpora take it unchanged.
//
// The channel is placed by center_hz: the frequency, relative to the LO, that comes out at
// 1,500 Hz of audio. That is the dial frequency plus 1,500 Hz. A QMX's IF is 12 kHz (QMX operating manual, §5.22),
// and qmx-panadapter's documentation puts its LO 12 kHz below the dial, so FT8's channel is at
// 13,500 Hz. if_trim_hz is added to
// it: a unit whose IF is 40 Hz high is corrected with a trim of +40. An uncorrected trim error moves
// every decoded audio frequency by that many Hz, and the station replies at the decoded frequency,
// so whoever sets it should report what it applied (iq_frontend_nco_hz()).
//
// The QMX's IQ format is not documented, so its unknowns are parameters: which channel carries I
// (swap_iq) and the sign of Q (negate_q). Either one wrong puts the band on the wrong side of the
// LO; the first real recording settles them.
//
// There is no I/Q balance correction. A gain or phase error between I and Q leaks each signal
// into its mirror about the LO (the image); host_test_iq_frontend measures how much.
//
// State lives in the caller's iq_frontend_t, one per channel: no statics, no heap. Any block size
// may be passed in, and the output is bit-identical however the input is chunked.
//
// A real (already demodulated) input goes through the same filters as I = 2 * audio, Q = 0 and
// center_hz 1,500: the dial at 0 Hz. The factor 2 restores the half of a real signal's power
// that sits at negative frequencies, which the filter rejects.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IQ_FRONTEND_MAX_STAGES 6           // input/output ratio up to 64 (384 kHz to 6 kHz)
#define IQ_FRONTEND_MAX_HALFBAND_TAPS 31
#define IQ_FRONTEND_MAX_CHANNEL_TAPS 203   // enough for output rates up to 12 kHz
#define IQ_FRONTEND_MAX_SHIFT_PERIOD 64

typedef struct {
    uint32_t input_rate_hz;   // e.g. 48,000 (QMX)
    uint32_t output_rate_hz;  // 12,000 offline, 6,000 in firmware; input/output a power of two, at least 2
    double center_hz;         // relative to the LO: the frequency that comes out at 1,500 Hz of audio
    double if_trim_hz;        // added to center_hz; may be fractional
    bool swap_iq;             // the first channel of each frame is Q, not I
    bool negate_q;            // Q's sign is inverted
} iq_frontend_config_t;

typedef struct {
    float re;
    float im;
} iq_frontend_complex_t;

typedef struct {
    int taps;                  // 4k + 3, so the outermost taps are non-zero
    float center;              // the middle tap
    float side[(IQ_FRONTEND_MAX_HALFBAND_TAPS + 1) / 4];  // taps at offsets ±1, ±3, ... from the middle
    iq_frontend_complex_t line[2 * IQ_FRONTEND_MAX_HALFBAND_TAPS];  // each sample written twice
    int pos;
    int skip;                  // 1: the next input completes a pair and yields an output
} iq_frontend_halfband_t;

typedef struct {
    // Configuration, as applied.
    iq_frontend_config_t config;
    int stages;
    int audio_low_hz;          // the flat band of the output: low edge
    int audio_high_hz;         // and high edge
    int shift_hz;              // where 0 Hz of the filtered channel lands in the audio

    // NCO: a 32-bit phase accumulator, read exactly every 256 samples and
    // rotated in float between.
    uint32_t nco_step;
    uint64_t input_count;
    iq_frontend_complex_t nco;
    iq_frontend_complex_t nco_rotate;

    iq_frontend_halfband_t halfband[IQ_FRONTEND_MAX_STAGES];

    // Channel low-pass at the output rate: symmetric, odd length.
    int channel_taps;
    float channel[(IQ_FRONTEND_MAX_CHANNEL_TAPS + 1) / 2];  // the middle tap, then each side's
    iq_frontend_complex_t channel_line[2 * IQ_FRONTEND_MAX_CHANNEL_TAPS];
    int channel_pos;

    // Shift up into the audio band: shift_hz / output_rate_hz is rational, so a short table.
    int shift_period;
    int shift_index;
    float shift_cos[IQ_FRONTEND_MAX_SHIFT_PERIOD];
    float shift_sin[IQ_FRONTEND_MAX_SHIFT_PERIOD];
} iq_frontend_t;

typedef enum {
    IQ_FRONTEND_OK = 0,
    IQ_FRONTEND_BAD_RATES = -1,     // ratio not a power of two in 2..64, or output under 6 kHz
    IQ_FRONTEND_TOO_MANY_TAPS = -2, // output rate too high for the fixed buffers
    IQ_FRONTEND_BAD_CENTER = -3,    // the channel does not fit inside ±input_rate_hz/2
} iq_frontend_status_t;

// Designs the filters and clears the state. Not real-time: it calls libm.
iq_frontend_status_t iq_frontend_init(iq_frontend_t* frontend, const iq_frontend_config_t* config);

// Clears the state iq_frontend_init() clears (filter history, the NCO's phase, the shift's position)
// and keeps the design: the output after it is bit-identical to a freshly initialised front end's,
// without calling libm. For a stream that starts again (the QMX's receive output switched).
void iq_frontend_reset(iq_frontend_t* frontend);

// Samples iq_frontend_process() may write for this many input frames.
size_t iq_frontend_output_capacity(const iq_frontend_t* frontend, size_t frames);

// Takes `frames` interleaved stereo frames (I, Q unless swap_iq), full scale ±1, and writes the
// audio produced so far to `out`. Returns the number of samples written, or -1 (and consumes
// nothing) if out_capacity is under iq_frontend_output_capacity(frontend, frames).
int iq_frontend_process(iq_frontend_t* frontend, const float* iq, size_t frames, float* out, size_t out_capacity);

// The frequency the NCO mixes down from, relative to the LO, after the trim and the 32-bit
// accumulator's rounding. center_hz + if_trim_hz - 1,500 Hz + the shift sits within 0.00002 Hz of it.
double iq_frontend_nco_hz(const iq_frontend_t* frontend);

#ifdef __cplusplus
}
#endif
