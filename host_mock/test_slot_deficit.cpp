#include <cstdio>
#include <cstdint>

#include "ft8_slot_deficit.h"

static int g_fails = 0;

static void expect_i64(int64_t got, int64_t want, const char* msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %lld want %lld)\n", msg,
                (long long)got, (long long)want);
        ++g_fails;
    }
}

int main()
{
    expect_i64(ft8_slot_sample_deficit(90000, 1, 15000, 6000), 0,
               "a full FT8 slot has no deficit");
    expect_i64(ft8_slot_sample_deficit(78000, 1, 15000, 6000), 12000,
               "two seconds unread is 12000 samples");
    expect_i64(ft8_slot_sample_deficit(91000, 1, 15000, 6000), 0,
               "extra samples are not a deficit");
    expect_i64(ft8_slot_sample_deficit(44000, 1, 7500, 6000), 1000,
               "FT4 expects 45000 samples");
    expect_i64(ft8_slot_sample_deficit(90000, 2, 15000, 6000), 90000,
               "a skipped boundary owes a second slot");
    expect_i64(ft8_slot_sample_deficit(100, 0, 15000, 6000), 0,
               "no slot crossed");
    expect_i64(ft8_slot_sample_deficit(-1, 1, 15000, 6000), 0,
               "a negative count is not a deficit");

    // Slot 0 decodes at 12.64 s and returns at 15.70 s, 0.70 s into slot 1. Its
    // last 3.06 s of audio (18,360 samples at 6 kHz) is still queued.
    expect_i64(ft8_block_slot_idx(15700, 18360, 15000, 6000), 0,
               "the queue after an overrun is the last slot's audio");
    expect_i64(ft8_block_slot_idx(15700, 4200, 15000, 6000), 1,
               "the block captured at 15.00 s opens slot 1");
    expect_i64(ft8_block_slot_idx(15700, 4206, 15000, 6000), 0,
               "one sample earlier is still slot 0");
    expect_i64(ft8_block_slot_idx(15000, 0, 15000, 6000), 1,
               "with nothing queued the block is dated now");
    expect_i64(ft8_block_slot_idx(14999, 0, 15000, 6000), 0,
               "one ms before the boundary");
    expect_i64(ft8_block_slot_idx(7600, 0, 7500, 6000), 1,
               "FT4 slots are 7.5 s");
    expect_i64(ft8_block_slot_idx(100, 6000, 15000, 6000), -1,
               "audio from before the clock's zero is the slot before");

    if (g_fails) {
        fprintf(stderr, "%d failed\n", g_fails);
        return 1;
    }
    printf("ok\n");
    return 0;
}
