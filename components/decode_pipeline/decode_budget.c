#include "decode_budget.h"

#include "decode_limits.h"

int decode_budget_units(int ft4, int64_t period_idx, int beside_other)
{
    if (!beside_other) return DECODE_BUDGET_UNITS;
    if (!ft4) return DECODE_BUDGET_FT8_BESIDE_FT4;
    // A negative index (a clock before 1970) is still a slot: its parity is its low bit.
    return (period_idx & 1) ? DECODE_BUDGET_FT4_BESIDE_FT8 : DECODE_BUDGET_UNITS;
}

int decode_cost_search(int ft4)
{
    return ft4 ? DECODE_COST_SEARCH_FT4 : DECODE_COST_SEARCH;
}
