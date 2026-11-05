#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "channel_limits.h"
#include "protocol.h"

extern "C" {
#include "common/monitor.h"
}

typedef int (*ft8_audio_read_cb_t)(void* ctx, float* out, int max_samples);
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

typedef struct {
    const char* tag;
    void* ctx;
    ft8_audio_read_cb_t read;
    ft8_audio_should_stop_cb_t should_stop;
    ft8_audio_block_cb_t on_block_processed;
    ft8_audio_activity_cb_t on_activity;  // may be null
    ft8_audio_take_frontend_us_cb_t take_frontend_us;  // may be null
    // The channel these samples are (docs/roadmap/several-channels.md, A channel): which of the
    // RX_MAX_CHANNELS sets of buffers and decode state it uses, its mode, and the waterfall's
    // subdivisions. The buffers are sized at build time for FT8 at t2/f2; a channel that needs more
    // is refused at start.
    int channel;
    const ProtocolConfig* mode;
    int time_osr;
    int freq_osr;
} ft8_audio_pipeline_config_t;

// One channel's finished slot, handed to decode (decode_slot(), decode_pipeline.h) on the capture task.
// The waterfall is the channel's own and decode subtracts from it; it is reset after decode returns.
typedef struct {
    int channel;
    const ProtocolConfig* mode;
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
