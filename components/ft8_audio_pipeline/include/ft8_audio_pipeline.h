#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef int (*ft8_audio_read_cb_t)(void* ctx, float* out, int max_samples);
typedef bool (*ft8_audio_should_stop_cb_t)(void* ctx);
typedef void (*ft8_audio_block_cb_t)(void* ctx);
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
} ft8_audio_pipeline_config_t;

// Runs until should_stop(). False when it could not start (no RAM for the waterfall or a buffer, no
// task for the ingest): nothing was decoded, and the caller says so rather than retrying silently.
bool ft8_audio_pipeline_run(const ft8_audio_pipeline_config_t* cfg);
