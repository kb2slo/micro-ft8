#pragma once

// A slot's decode budget and its search cost, by mode and by what shares its window (decode_limits.h;
// docs/architecture/decode.md, FT4). One rule the firmware and its host test both read.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The units one slot of `ft4` (0 FT8, 1 FT4) may spend. `period_idx` is the slot's index in its own mode's
// periods; `beside_other` is 1 when the source carries the other mode too. An FT4 period with an odd index
// is the second half of an FT8 slot: beside FT8 it decodes first and must be done before FT8 starts.
int decode_budget_units(int ft4, int64_t period_idx, int beside_other);

// The blocks of a slot's waterfall its decode starts after (decode_limits.h, DECODE_TRIGGER_*): FT8's start,
// FT4's, or FT4's earlier one for a second period beside FT8. Same arguments as above.
int decode_trigger_blocks(int ft4, int64_t period_idx, int beside_other);

// DECODE_COST_SEARCH for FT8, DECODE_COST_SEARCH_FT4 for FT4.
int decode_cost_search(int ft4);

#ifdef __cplusplus
}
#endif
