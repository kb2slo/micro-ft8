#pragma once

// How many candidates decode_slot() asks ftx_find_candidates() for.
// The recall harness and the decode bench's "today" row take this value, so a
// change here is what CI scores (tools/decode_recall).
#define DECODE_MAX_CANDIDATES 256

// The slot's decode budget in units (decode_core.h; decode.md, The work budget): an LDPC iteration is
// one unit, a search DECODE_COST_SEARCH, a candidate tried DECODE_COST_CANDIDATE beside its
// iterations. The costs are decode_bench's on the P4 (2026-11-06): a search about 370 iterations, a
// candidate nothing the five slots could separate from its iterations. 6000 units is about 1.9 s of
// the 2.36 s guard there, and lost no message on either corpus (tools/decode_recall/budget.py). The
// recall harness scores these values, like the cap above.
#define DECODE_BUDGET_UNITS 6000
#define DECODE_COST_SEARCH 370
#define DECODE_COST_CANDIDATE 0
