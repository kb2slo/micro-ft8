#include "decode_budget.h"

#include "decode_limits.h"

// A negative index (a clock before 1970) is still a slot: its parity is its low bit.
static int second_half(int64_t period_idx)
{
    return (int)(period_idx & 1);
}

int decode_budget_units(int ft4, int64_t period_idx, int beside_other)
{
    if (!ft4) return beside_other ? DECODE_BUDGET_FT8_BESIDE_FT4 : DECODE_BUDGET_UNITS;
    return (beside_other && second_half(period_idx)) ? DECODE_BUDGET_FT4_BESIDE_FT8 : DECODE_BUDGET_FT4;
}

int decode_trigger_blocks(int ft4, int64_t period_idx, int beside_other)
{
    if (!ft4) return DECODE_TRIGGER_FT8;
    return (beside_other && second_half(period_idx)) ? DECODE_TRIGGER_FT4_BESIDE_FT8 : DECODE_TRIGGER_FT4;
}

int decode_cost_search(int ft4)
{
    return ft4 ? DECODE_COST_SEARCH_FT4 : DECODE_COST_SEARCH;
}
