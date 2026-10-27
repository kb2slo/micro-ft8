#pragma once

// Band activity during a receive slot (docs/architecture/decode.md, Band activity): one number per
// block, read from the waterfall column the block just added, sent in batches of about a second.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A bin counts as active this far above the column's median: 10 dB in the waterfall's 0.5 dB
// codes (monitor_process()). Noise alone passes about 0.1% of the time. A tone 10 dB up in its
// 6.25 Hz bin is about -16 dB SNR in 2500 Hz.
#define FT8_ACTIVITY_THRESHOLD_CODES 20

// Active bins this close (inactive bins between them, at most) are one signal: a station's tone
// spreads over about four bins, more when it is strong.
#define FT8_ACTIVITY_GAP_BINS 2

// Frequency cells per block for the slot bar's strip: about 28 Hz each over 200-2900 Hz, half an
// FT8 signal's width. A cell is active when any bin in it is. Sent as a bitmask, cell 0 in bit 0 of
// byte 0.
#define FT8_ACTIVITY_CELLS 96
#define FT8_ACTIVITY_MASK_BYTES (FT8_ACTIVITY_CELLS / 8)

// Most blocks in one batch: a level and a mask each, so 12 keeps a batch in one Port A frame and
// its /api/events row under the sidekick's row buffer. FT4 sends about two batches a second.
#define FT8_ACTIVITY_BATCH_MAX 12

// About how many signals `col` (one waterfall column, `n_bins` codes) holds: clusters of bins at
// least FT8_ACTIVITY_THRESHOLD_CODES above its median, joined across gaps of up to
// FT8_ACTIVITY_GAP_BINS. Capped at 255. The AGC scales a whole block, so a level against the median
// does not move with it. `mask`, if not null, gets which of FT8_ACTIVITY_CELLS equal cells across
// the column hold an active bin.
uint8_t ft8_block_activity(const uint8_t *col, int n_bins, uint8_t mask[FT8_ACTIVITY_MASK_BYTES]);

// The position of a block in its slot, counted in blocks from the slot's start: the block whose
// middle falls in [k, k + 1) block periods is k. Dated like ft8_block_slot_idx(): now_ms as the
// block completes, `queued` the samples captured after it. A block straddling the boundary dates
// to the slot its end is in, at 0.
int ft8_block_pos(int64_t now_ms, int64_t queued, int slot_ms, int block_ms, int sample_rate);

typedef struct {
    int64_t slot_idx;
    uint8_t first;  // position of level[0] in the slot (ft8_block_pos())
    uint8_t n;
    uint8_t cap;    // blocks per batch, at most FT8_ACTIVITY_BATCH_MAX
    uint8_t level[FT8_ACTIVITY_BATCH_MAX];
    uint8_t mask[FT8_ACTIVITY_BATCH_MAX][FT8_ACTIVITY_MASK_BYTES];
} ft8_activity_batch_t;

// Blocks per batch for a block period: about one second's worth, so the app's 1 Hz poll sees about
// one event a poll, and never more than FT8_ACTIVITY_BATCH_MAX.
void ft8_activity_batch_init(ft8_activity_batch_t *b, int block_ms);

// Adds one block. True when `done` now holds a batch to send: this block filled the batch, or it
// could not join the open one (another slot, or not the next position), which then went to `done`
// and this block starts the next.
bool ft8_activity_batch_add(ft8_activity_batch_t *b, int64_t slot_idx, int pos, uint8_t level,
                            const uint8_t mask[FT8_ACTIVITY_MASK_BYTES], ft8_activity_batch_t *done);

// Hands over whatever is open, so a decode that holds the task does not hold the slot's last
// blocks too. False when nothing is open.
bool ft8_activity_batch_take(ft8_activity_batch_t *b, ft8_activity_batch_t *done);

#ifdef __cplusplus
}
#endif
