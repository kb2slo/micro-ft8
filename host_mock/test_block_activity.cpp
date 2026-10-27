// Host test for ft8_block_activity.h: the band-activity level per block, where a block sits in its
// slot, and how levels are batched into ACTIVITY events (docs/architecture/decode.md, Band activity).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "ft8_block_activity.h"

static int g_fails = 0;

static void check(bool ok, const char* msg)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    }
}

// A column of noise around code 100 (-70 dB), spread a few codes either side.
static std::vector<uint8_t> noise_column(int n_bins)
{
    std::vector<uint8_t> col(n_bins);
    uint32_t r = 12345;
    for (int i = 0; i < n_bins; ++i) {
        r = r * 1103515245u + 12345u;
        col[i] = (uint8_t)(96 + (r >> 16) % 9);
    }
    return col;
}

static void test_level()
{
    const int n = 432;  // 200..2900 Hz at 6.25 Hz
    const uint8_t up = 100 + FT8_ACTIVITY_THRESHOLD_CODES + 4;
    std::vector<uint8_t> col = noise_column(n);
    check(ft8_block_activity(col.data(), n, nullptr) == 0, "noise alone reads 0");

    // One station: its tone spread over four bins, with a dip inside the gap allowed.
    col[50] = up;
    col[51] = up + 10;
    col[52 + FT8_ACTIVITY_GAP_BINS] = up;
    check(ft8_block_activity(col.data(), n, nullptr) == 1, "one station's spread tone reads 1");

    // A second and third station, well apart.
    col[200] = up;
    col[201] = up;
    col[300] = 100 + FT8_ACTIVITY_THRESHOLD_CODES + 30;
    check(ft8_block_activity(col.data(), n, nullptr) == 3, "three stations read 3");

    // Just past the gap: two signals.
    col[201 + FT8_ACTIVITY_GAP_BINS + 2] = up;
    check(ft8_block_activity(col.data(), n, nullptr) == 4, "a bin past the gap is another signal");

    col[400] = 100 + FT8_ACTIVITY_THRESHOLD_CODES - 6;
    check(ft8_block_activity(col.data(), n, nullptr) == 4, "a bin under 10 dB up does not count");

    // The AGC moves the whole column: the level does not move with it.
    for (int i = 0; i < n; ++i) col[i] = (uint8_t)(col[i] - 40);
    check(ft8_block_activity(col.data(), n, nullptr) == 4, "a whole column 20 dB lower reads the same");

    // Signals at both edges count.
    std::vector<uint8_t> edges = noise_column(n);
    edges[0] = up;
    edges[n - 1] = up;
    check(ft8_block_activity(edges.data(), n, nullptr) == 2, "signals at both edges count");

    // A signal every GAP + 2 bins: 300 of them, capped.
    std::vector<uint8_t> many(1200, 100);
    for (int i = 0; i < 1200; i += FT8_ACTIVITY_GAP_BINS + 2) many[i] = 200;
    check(ft8_block_activity(many.data(), 1200, nullptr) == 255, "more than 255 signals caps at 255");

    check(ft8_block_activity(nullptr, n, nullptr) == 0, "no column reads 0");
    check(ft8_block_activity(col.data(), 0, nullptr) == 0, "no bins reads 0");
}

static bool cell_on(const uint8_t* mask, int cell)
{
    return (mask[cell / 8] >> (cell % 8)) & 1;
}

static void test_mask()
{
    const int n = 432;  // 4.5 bins a cell
    const uint8_t up = 100 + FT8_ACTIVITY_THRESHOLD_CODES + 4;
    std::vector<uint8_t> col = noise_column(n);
    uint8_t mask[FT8_ACTIVITY_MASK_BYTES];
    std::memset(mask, 0xAA, sizeof(mask));
    ft8_block_activity(col.data(), n, mask);
    bool any = false;
    for (int c = 0; c < FT8_ACTIVITY_CELLS; ++c) any = any || cell_on(mask, c);
    check(!any, "noise alone sets no cell, and the mask is cleared first");

    col[0] = up;          // cell 0
    col[100] = up;        // 100 * 96 / 432 = 22.2: cell 22
    col[101] = up;        // the same cell
    col[n - 1] = up;      // cell 95
    const uint8_t level = ft8_block_activity(col.data(), n, mask);
    check(level == 3, "the level is unchanged by asking for a mask");
    int on = 0;
    for (int c = 0; c < FT8_ACTIVITY_CELLS; ++c) on += cell_on(mask, c);
    check(on == 3 && cell_on(mask, 0) && cell_on(mask, 22) && cell_on(mask, 95),
          "each active bin sets its cell: first, middle and last");
    check(mask[0] == 0x01 && mask[FT8_ACTIVITY_MASK_BYTES - 1] == 0x80,
          "cell 0 is bit 0 of byte 0, cell 95 bit 7 of the last byte");
}

static void test_pos()
{
    // FT8: 15 s slots, 160 ms blocks, 6 kHz.
    const int64_t slot = 15000;
    const int64_t t0 = 1000 * slot;  // a slot start
    check(ft8_block_pos(t0 + 160, 0, 15000, 160, 6000) == 0, "the first block is 0");
    check(ft8_block_pos(t0 + 320, 0, 15000, 160, 6000) == 1, "the second block is 1");
    check(ft8_block_pos(t0 + 12640, 0, 15000, 160, 6000) == 78, "the 79th block is 78");
    check(ft8_block_pos(t0 + 100, 0, 15000, 160, 6000) == 0,
          "a block straddling the boundary, ending in this slot, is 0");
    check(ft8_block_pos(t0 + 2000 + 160, 6000 * 2, 15000, 160, 6000) == 0,
          "dated by its audio: two seconds queued behind it put it at the slot's start");
    check(ft8_block_pos(t0 + 14990, 0, 15000, 160, 6000) == 93, "the slot's last block is 93");
    // FT4: 7.5 s slots, 48 ms blocks.
    check(ft8_block_pos(7500 * 3 + 48 * 10, 0, 7500, 48, 6000) == 9, "FT4's tenth block is 9");
    check(ft8_block_pos(t0 + 160, 0, 0, 160, 6000) == 0, "a zero slot length reads 0");
}

// A mask whose every byte is `v`.
static const uint8_t* mk(int v)
{
    static uint8_t m[FT8_ACTIVITY_MASK_BYTES];
    std::memset(m, v, sizeof(m));
    return m;
}

static void test_batch()
{
    ft8_activity_batch_t b, out;
    ft8_activity_batch_init(&b, 160);
    check(b.cap == 7, "FT8: about a second is 7 blocks of 160 ms");
    ft8_activity_batch_init(&b, 48);
    check(b.cap == FT8_ACTIVITY_BATCH_MAX, "FT4: a second of 48 ms blocks is more than a batch holds");
    ft8_activity_batch_init(&b, 10);
    check(b.cap == FT8_ACTIVITY_BATCH_MAX, "never more than FT8_ACTIVITY_BATCH_MAX");
    check(FT8_ACTIVITY_BATCH_MAX == 12, "12 blocks: what the /api/events row is sized for");

    ft8_activity_batch_init(&b, 160);
    int sent = 0;
    for (int pos = 0; pos < 7; ++pos) {
        if (ft8_activity_batch_add(&b, 50, pos, (uint8_t)(pos + 1), mk(pos), &out)) ++sent;
    }
    check(sent == 1 && out.slot_idx == 50 && out.first == 0 && out.n == 7 &&
              out.level[0] == 1 && out.level[6] == 7,
          "seven blocks in a row are one batch");
    check(out.mask[0][0] == 0 && out.mask[6][0] == 6 && out.mask[6][FT8_ACTIVITY_MASK_BYTES - 1] == 6,
          "each block's mask travels with its level");

    check(!ft8_activity_batch_add(&b, 50, 7, 9, nullptr, &out), "the next block opens a batch");
    check(ft8_activity_batch_add(&b, 50, 9, 4, nullptr, &out) && out.first == 7 && out.n == 1 &&
              out.level[0] == 9,
          "a skipped position hands over the open batch");
    check(ft8_activity_batch_add(&b, 51, 0, 2, nullptr, &out) && out.first == 9 && out.n == 1,
          "a new slot hands over the open batch");
    check(ft8_activity_batch_take(&b, &out) && out.slot_idx == 51 && out.first == 0 &&
              out.n == 1 && out.level[0] == 2,
          "take hands over what is open");
    check(!ft8_activity_batch_take(&b, &out), "take with nothing open is false");

    // One block a batch: a hand-over leaves a full batch open, which the next block sends.
    ft8_activity_batch_init(&b, 2000);
    check(b.cap == 1, "a block longer than a second is a batch of one");
    check(ft8_activity_batch_add(&b, 7, 0, 1, nullptr, &out) && out.first == 0, "a batch of one sends");
    check(ft8_activity_batch_add(&b, 7, 1, 2, nullptr, &out) && out.first == 1, "and the next");
    check(!ft8_activity_batch_take(&b, &out), "nothing left open");

    // No batch ever overruns its array, whatever positions arrive.
    ft8_activity_batch_init(&b, 48);
    for (int i = 0; i < 1000; ++i) {
        ft8_activity_batch_add(&b, i / 150, (i * 7) % 160, (uint8_t)i, nullptr, &out);
        check(b.n <= b.cap && out.n <= FT8_ACTIVITY_BATCH_MAX, "a batch stays inside its array");
    }
}

int main()
{
    test_level();
    test_mask();
    test_pos();
    test_batch();
    if (g_fails) {
        fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    printf("block activity OK\n");
    return 0;
}
