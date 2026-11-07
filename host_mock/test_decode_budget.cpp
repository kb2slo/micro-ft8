// decode_budget.h: where a slot's decode starts and what it may spend, by mode and by what shares its window.
#include <cstdio>

extern "C" {
#include "decode_budget.h"
#include "decode_limits.h"
}

static int g_fails = 0;

static void expect_int(long long got, long long want, const char* msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %lld want %lld)\n", msg, got, want);
        ++g_fails;
    }
}

static void expect(bool ok, const char* msg)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    }
}

int main()
{
    // FT8: its start whatever is beside it, and its budget smaller beside FT4.
    expect_int(decode_trigger_blocks(0, 119604723, 0), DECODE_TRIGGER_FT8, "FT8 alone starts at its blocks");
    expect_int(decode_trigger_blocks(0, 119604723, 1), DECODE_TRIGGER_FT8, "and beside FT4");
    expect_int(decode_budget_units(0, 119604723, 0), DECODE_BUDGET_UNITS, "FT8 alone");
    expect_int(decode_budget_units(0, 119604723, 1), DECODE_BUDGET_FT8_BESIDE_FT4, "FT8 beside FT4");
    // FT4: alone, or a first period beside FT8, starts late with its own budget; a second period beside FT8
    // starts early with a small one.
    expect_int(decode_trigger_blocks(1, 239209447, 0), DECODE_TRIGGER_FT4, "FT4 alone, any period");
    expect_int(decode_budget_units(1, 239209447, 0), DECODE_BUDGET_FT4, "FT4 alone, any period");
    expect_int(decode_trigger_blocks(1, 239209446, 1), DECODE_TRIGGER_FT4, "FT4 first period beside FT8");
    expect_int(decode_budget_units(1, 239209446, 1), DECODE_BUDGET_FT4, "FT4 first period beside FT8");
    expect_int(decode_trigger_blocks(1, 239209447, 1), DECODE_TRIGGER_FT4_BESIDE_FT8, "FT4 second period beside FT8");
    expect_int(decode_budget_units(1, 239209447, 1), DECODE_BUDGET_FT4_BESIDE_FT8, "FT4 second period beside FT8");
    expect_int(decode_budget_units(1, -1, 1), DECODE_BUDGET_FT4_BESIDE_FT8, "a negative index keeps its parity");
    // An FT4 period index 2k and 2k+1 lie in FT8 slot k: the second half starts at the FT8 slot's middle.
    expect_int((239209447LL * 7500) / 15000, 119604723, "FT4 period 2k+1 is in FT8 slot k");
    // The timing the design rests on, at the rates two workers run on the air (decode_limits.h): every start
    // leaves its deadline, and FT4's second period is done before FT8 starts, so they never share the cores.
    const double ft8_start_ms = DECODE_TRIGGER_FT8 * 160.0;
    const double ft4_second_start_ms = 7500.0 + DECODE_TRIGGER_FT4_BESIDE_FT8 * 48.0;
    const double ft4_first_start_ms = DECODE_TRIGGER_FT4 * 48.0;
    expect(ft8_start_ms + 14 + DECODE_BUDGET_UNITS * 0.20 < 15000.0, "FT8 alone ends before its boundary");
    expect(ft8_start_ms + 14 + DECODE_BUDGET_FT8_BESIDE_FT4 * 0.23 < 15000.0, "FT8 beside FT4 ends before it");
    expect(ft4_first_start_ms + 14 + DECODE_BUDGET_FT4 * 0.23 < 7500.0, "FT4's first period ends before its own");
    expect(ft4_second_start_ms + 14 + DECODE_BUDGET_FT4_BESIDE_FT8 * 0.23 < ft8_start_ms,
           "FT4's second period ends before FT8 starts");
    // Each start fits its waterfall: FT8 93 blocks a slot, FT4 156 a period.
    expect(DECODE_TRIGGER_FT8 < 93 && DECODE_TRIGGER_FT4 < 156, "starts fit the waterfalls");
    expect_int(decode_cost_search(0), DECODE_COST_SEARCH, "FT8's search");
    expect_int(decode_cost_search(1), DECODE_COST_SEARCH_FT4, "FT4's search");
    if (g_fails == 0) printf("decode_budget: all checks passed\n");
    return g_fails ? 1 : 0;
}
