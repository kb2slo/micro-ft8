// decode_budget.h: what a slot may spend, by mode and by what shares its window.
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

int main()
{
    // One channel: today's budget, whatever the mode or period.
    expect_int(decode_budget_units(0, 119604723, 0), DECODE_BUDGET_UNITS, "FT8 alone");
    expect_int(decode_budget_units(1, 239209447, 0), DECODE_BUDGET_UNITS, "FT4 alone, a second period");
    // Beside each other: FT8 and FT4's second period split the shared window; FT4's first has it all.
    expect_int(decode_budget_units(0, 119604723, 1), DECODE_BUDGET_FT8_BESIDE_FT4, "FT8 beside FT4");
    expect_int(decode_budget_units(1, 239209446, 1), DECODE_BUDGET_UNITS, "FT4 first period, alone in its window");
    expect_int(decode_budget_units(1, 239209447, 1), DECODE_BUDGET_FT4_BESIDE_FT8, "FT4 second period, FT8's window");
    expect_int(decode_budget_units(1, -1, 1), DECODE_BUDGET_FT4_BESIDE_FT8, "a negative index keeps its parity");
    // The shared window holds what two workers fit, and no more.
    expect_int(DECODE_BUDGET_FT8_BESIDE_FT4 + DECODE_BUDGET_FT4_BESIDE_FT8, DECODE_BUDGET_UNITS,
               "the split fills the window's budget");
    // An FT4 period index 2k and 2k+1 lie in FT8 slot k: the second half starts at the FT8 slot's middle.
    expect_int((239209447LL * 7500) / 15000, 119604723, "FT4 period 2k+1 is in FT8 slot k");
    expect_int(decode_cost_search(0), DECODE_COST_SEARCH, "FT8's search");
    expect_int(decode_cost_search(1), DECODE_COST_SEARCH_FT4, "FT4's search");
    if (g_fails == 0) printf("decode_budget: all checks passed\n");
    return g_fails ? 1 : 0;
}
