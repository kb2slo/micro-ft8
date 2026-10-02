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

    if (g_fails) {
        fprintf(stderr, "%d failed\n", g_fails);
        return 1;
    }
    printf("ok\n");
    return 0;
}
