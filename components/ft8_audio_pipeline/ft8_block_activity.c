#include "ft8_block_activity.h"

#include <string.h>

uint8_t ft8_block_activity(const uint8_t *col, int n_bins)
{
    if (!col || n_bins < 1) return 0;
    uint16_t hist[256];
    memset(hist, 0, sizeof(hist));
    for (int i = 0; i < n_bins; ++i) ++hist[col[i]];
    int median = 0;
    for (int seen = 0; median < 256; ++median) {
        seen += hist[median];
        if (seen * 2 >= n_bins) break;
    }
    const int threshold = median + FT8_ACTIVITY_THRESHOLD_CODES;
    int clusters = 0;
    int last = -(FT8_ACTIVITY_GAP_BINS + 2);
    for (int i = 0; i < n_bins; ++i) {
        if (col[i] < threshold) continue;
        if (i - last > FT8_ACTIVITY_GAP_BINS + 1) ++clusters;
        last = i;
    }
    return (uint8_t)(clusters > 255 ? 255 : clusters);
}

int ft8_block_pos(int64_t now_ms, int64_t queued, int slot_ms, int block_ms, int sample_rate)
{
    if (slot_ms < 1 || block_ms < 1 || sample_rate < 1) return 0;
    if (queued < 0) queued = 0;
    const int64_t end_ms = now_ms - queued * 1000 / sample_rate;
    int64_t into = end_ms % slot_ms;
    if (into < 0) into += slot_ms;
    const int64_t mid = into - block_ms / 2;
    if (mid < 0) return 0;
    const int64_t pos = mid / block_ms;
    return pos > 255 ? 255 : (int)pos;
}

void ft8_activity_batch_init(ft8_activity_batch_t *b, int block_ms)
{
    memset(b, 0, sizeof(*b));
    int cap = block_ms > 0 ? (1000 + block_ms - 1) / block_ms : 1;
    if (cap < 1) cap = 1;
    if (cap > FT8_ACTIVITY_BATCH_MAX) cap = FT8_ACTIVITY_BATCH_MAX;
    b->cap = (uint8_t)cap;
}

bool ft8_activity_batch_take(ft8_activity_batch_t *b, ft8_activity_batch_t *done)
{
    if (b->n == 0) return false;
    *done = *b;
    b->n = 0;
    return true;
}

bool ft8_activity_batch_add(ft8_activity_batch_t *b, int64_t slot_idx, int pos, uint8_t level,
                            ft8_activity_batch_t *done)
{
    bool handed = false;
    // A full batch still open was left by a hand-over in the call before: only one goes per call.
    if (b->n > 0 && (slot_idx != b->slot_idx || pos != b->first + b->n || b->n >= b->cap)) {
        handed = ft8_activity_batch_take(b, done);
    }
    if (b->n == 0) {
        b->slot_idx = slot_idx;
        b->first = (uint8_t)(pos < 0 ? 0 : (pos > 255 ? 255 : pos));
    }
    b->level[b->n++] = level;
    if (b->n >= b->cap && !handed) {
        handed = ft8_activity_batch_take(b, done);
    }
    return handed;
}
