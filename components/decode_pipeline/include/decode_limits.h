#pragma once

// How many candidates decode_slot() asks ftx_find_candidates() for.
// The recall harness and the decode bench's "today" row take this value, so a
// change here is what CI scores (tools/decode_recall).
#define DECODE_MAX_CANDIDATES 256

// Where each mode's decode starts, in blocks of its waterfall (decode.md, Decoding later in the slot): later
// sees more of each signal, and leaves less time. With two workers FT8 decodes most at 83 blocks (13.28 s)
// and FT4 at 120 (5.76 s into its period), on the host (tools/decode_recall/sweep_start.py, 2026-11-07).
// Beside FT8, FT4's second period keeps 105 blocks (12.54 s into FT8's slot) and finishes before FT8's
// starts, so the two never share the cores.
#define DECODE_TRIGGER_FT8 83
#define DECODE_TRIGGER_FT4 120
#define DECODE_TRIGGER_FT4_BESIDE_FT8 105

// A slot's decode budget in units (decode_core.h; decode.md, The work budget): an LDPC iteration is one unit,
// a search DECODE_COST_SEARCH, a candidate tried DECODE_COST_CANDIDATE beside its iterations. The costs are
// decode_bench's on the P4 (2026-11-06): a search about 370 iterations, a candidate nothing the five slots
// could separate from its iterations. Each budget is what the time from its start to its deadline holds at
// the rate two workers run on the air, about 0.20 ms a unit for FT8 alone and 0.23 beside FT4 (the SLOT
// lines of 2026-11-07), with about 100 ms to spare: FT8 alone 7,800 in 1.72 s; FT8 beside FT4 7,000; FT4's
// first period 7,000 in its 1.74 s; its second 2,800 in the 0.74 s before FT8 starts. On the corpora and
// the captures none of them costs a message against no budget (decode.md). The recall harness scores these
// values, like the cap above.
#define DECODE_BUDGET_UNITS 7800
#define DECODE_BUDGET_FT8_BESIDE_FT4 7000
#define DECODE_BUDGET_FT4 7000
#define DECODE_BUDGET_FT4_BESIDE_FT8 2800
#define DECODE_COST_SEARCH 370
#define DECODE_COST_CANDIDATE 0

// An LDPC iteration costs FT4 what it costs FT8, and an FT4 search 0.24 of an FT8 one, on the host
// (tools/decode_bench/score_ft4_edge.py --ours --costs, 2026-11-07): about 90 units on the P4, scaled from
// DECODE_COST_SEARCH. Estimated, not yet timed on the P4.
#define DECODE_COST_SEARCH_FT4 90
