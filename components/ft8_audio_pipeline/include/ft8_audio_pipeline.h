#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "channel_limits.h"
#include "protocol.h"

extern "C" {
#include "common/monitor.h"
}

// One read from the source: up to max_samples samples at 6 kHz into each of the n_channels buffers in
// out, one per channel in the config's order, the same count in each. Returns that count, 0 if nothing
// was ready. A source that carries one channel (a real-audio radio, an HL2 receiver) takes n_channels 1.
typedef int (*ft8_audio_read_cb_t)(void* ctx, float* const* out, int n_channels, int max_samples);
typedef bool (*ft8_audio_should_stop_cb_t)(void* ctx);
typedef void (*ft8_audio_block_cb_t)(void* ctx);
// The microseconds the receive front end has spent (components/iq_frontend) since the last call, then
// forgotten. Called on the capture task at each slot boundary, for BDRY's fe; the front end runs
// on the ingest task, so the count is atomic. May be null: BDRY's fe then reads 0.
typedef uint32_t (*ft8_audio_take_frontend_us_cb_t)(void* ctx);
// One batch of band activity (ft8_block_activity.h): `n` blocks of `block_ms` each, the first at
// position `first` in slot `slot_idx`; per block a level and a mask of FT8_ACTIVITY_CELLS cells
// spanning lo_hz..hi_hz. Called on the capture task; queue, do not block.
typedef void (*ft8_audio_activity_cb_t)(void* ctx, int64_t slot_idx, int block_ms, int first,
                                        const uint8_t* levels, const uint8_t* masks, int n,
                                        int lo_hz, int hi_hz);

// One channel the source carries (docs/roadmap/several-channels.md, A channel): which of the
// RX_MAX_CHANNELS sets of buffers and decode state it uses, its mode, and the waterfall's subdivisions.
// The buffers are sized at build time for FT8 at t2/f2; a channel that needs more is refused at start.
typedef struct {
    int channel;
    const ProtocolConfig* mode;
    int time_osr;
    int freq_osr;
    // This channel's dial against the radio's, in Hz: 0 for the station's FT8, FT4's dial minus FT8's for
    // an FT4 channel beside it. Each decode reports the radio's dial plus this.
    int32_t dial_offset_hz;
    // The channel the station transmits on. Its slots gate TX (decode_tx_state.h) and its decodes go to
    // autoseq; any other channel only listens: it skips what our transmission covers and touches no TX
    // state (row an8x).
    bool station;
} ft8_audio_channel_config_t;

typedef struct {
    const char* tag;
    void* ctx;
    ft8_audio_read_cb_t read;
    ft8_audio_should_stop_cb_t should_stop;
    ft8_audio_block_cb_t on_block_processed;
    ft8_audio_activity_cb_t on_activity;  // may be null
    ft8_audio_take_frontend_us_cb_t take_frontend_us;  // may be null
    // The source's channels, 1..RX_MAX_CHANNELS, which read() fills in this order.
    int n_channels;
    ft8_audio_channel_config_t channels[RX_MAX_CHANNELS];
} ft8_audio_pipeline_config_t;

// One channel's finished slot, handed to decode (decode_slot(), decode_pipeline.h) on the decode task.
// The waterfall is one of the channel's two and decode subtracts from it; capture fills the other
// meanwhile, and takes this one back once decode_slot() returns.
typedef struct {
    int channel;
    const ProtocolConfig* mode;
    bool station;             // ft8_audio_channel_config_t's
    int32_t dial_offset_hz;   // ft8_audio_channel_config_t's
    bool beside_other;        // the source carries another channel too, so decodes share a window
    monitor_t* monitor;
    const float* block_gain;  // the AGC gain per waterfall block (decode_snr.h)
    int64_t slot_idx;         // the slot the audio was recorded in, dated by the audio
    // The slot began one boundary after a whole slot and the ingest ring dropped nothing since: only
    // such a slot may step the clock (decode_sync.h).
    bool capture_clean;
} ft8_slot_t;

// Runs until should_stop(). False when it could not start (no RAM for a buffer, no task for the
// ingest, a channel its buffers cannot hold): nothing was decoded, and the caller says so rather
// than retrying silently.
bool ft8_audio_pipeline_run(const ft8_audio_pipeline_config_t* cfg);
