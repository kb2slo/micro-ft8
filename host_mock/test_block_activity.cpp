// Host test for ft8_block_activity.h: the band-activity level per block, where a block sits in its
// slot, and how levels are batched into ACTIVITY events (docs/architecture/decode.md, Band activity).

#include <cstdint>
#include <cstdio>
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
    check(ft8_block_activity(col.data(), n) == 0, "noise alone reads 0");

    // One station: its tone spread over four bins, with a dip inside the gap allowed.
    col[50] = up;
    col[51] = up + 10;
    col[52 + FT8_ACTIVITY_GAP_BINS] = up;
    check(ft8_block_activity(col.data(), n) == 1, "one station's spread tone reads 1");

    // A second and third station, well apart.
    col[200] = up;
    col[201] = up;
    col[300] = 100 + FT8_ACTIVITY_THRESHOLD_CODES + 30;
    check(ft8_block_activity(col.data(), n) == 3, "three stations read 3");

    // Just past the gap: two signals.
    col[201 + FT8_ACTIVITY_GAP_BINS + 2] = up;
    check(ft8_block_activity(col.data(), n) == 4, "a bin past the gap is another signal");

    col[400] = 100 + FT8_ACTIVITY_THRESHOLD_CODES - 6;
    check(ft8_block_activity(col.data(), n) == 4, "a bin under 10 dB up does not count");

    // The AGC moves the whole column: the level does not move with it.
    for (int i = 0; i < n; ++i) col[i] = (uint8_t)(col[i] - 40);
    check(ft8_block_activity(col.data(), n) == 4, "a whole column 20 dB lower reads the same");

    // Signals at both edges count.
    std::vector<uint8_t> edges = noise_column(n);
    edges[0] = up;
    edges[n - 1] = up;
    check(ft8_block_activity(edges.data(), n) == 2, "signals at both edges count");

    // A signal every GAP + 2 bins: 300 of them, capped.
    std::vector<uint8_t> many(1200, 100);
    for (int i = 0; i < 1200; i += FT8_ACTIVITY_GAP_BINS + 2) many[i] = 200;
    check(ft8_block_activity(many.data(), 1200) == 255, "more than 255 signals caps at 255");

    check(ft8_block_activity(nullptr, n) == 0, "no column reads 0");
    check(ft8_block_activity(col.data(), 0) == 0, "no bins reads 0");
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

static void test_batch()
{
    ft8_activity_batch_t b, out;
    ft8_activity_batch_init(&b, 160);
    check(b.cap == 7, "FT8: about a second is 7 blocks of 160 ms");
    ft8_activity_batch_init(&b, 48);
    check(b.cap == 21, "FT4: about a second is 21 blocks of 48 ms");
    ft8_activity_batch_init(&b, 10);
    check(b.cap == FT8_ACTIVITY_BATCH_MAX, "never more than FT8_ACTIVITY_BATCH_MAX");

    ft8_activity_batch_init(&b, 160);
    int sent = 0;
    for (int pos = 0; pos < 7; ++pos) {
        if (ft8_activity_batch_add(&b, 50, pos, (uint8_t)(pos + 1), &out)) ++sent;
    }
    check(sent == 1 && out.slot_idx == 50 && out.first == 0 && out.n == 7 &&
              out.level[0] == 1 && out.level[6] == 7,
          "seven blocks in a row are one batch");

    check(!ft8_activity_batch_add(&b, 50, 7, 9, &out), "the next block opens a batch");
    check(ft8_activity_batch_add(&b, 50, 9, 4, &out) && out.first == 7 && out.n == 1 &&
              out.level[0] == 9,
          "a skipped position hands over the open batch");
    check(ft8_activity_batch_add(&b, 51, 0, 2, &out) && out.first == 9 && out.n == 1,
          "a new slot hands over the open batch");
    check(ft8_activity_batch_take(&b, &out) && out.slot_idx == 51 && out.first == 0 &&
              out.n == 1 && out.level[0] == 2,
          "take hands over what is open");
    check(!ft8_activity_batch_take(&b, &out), "take with nothing open is false");

    // One block a batch: a hand-over leaves a full batch open, which the next block sends.
    ft8_activity_batch_init(&b, 2000);
    check(b.cap == 1, "a block longer than a second is a batch of one");
    check(ft8_activity_batch_add(&b, 7, 0, 1, &out) && out.first == 0, "a batch of one sends");
    check(ft8_activity_batch_add(&b, 7, 1, 2, &out) && out.first == 1, "and the next");
    check(!ft8_activity_batch_take(&b, &out), "nothing left open");

    // No batch ever overruns its array, whatever positions arrive.
    ft8_activity_batch_init(&b, 48);
    for (int i = 0; i < 1000; ++i) {
        ft8_activity_batch_add(&b, i / 150, (i * 7) % 160, (uint8_t)i, &out);
        check(b.n <= b.cap && out.n <= FT8_ACTIVITY_BATCH_MAX, "a batch stays inside its array");
    }
}

int main()
{
    test_level();
    test_pos();
    test_batch();
    if (g_fails) {
        fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    printf("block activity OK\n");
    return 0;
}
