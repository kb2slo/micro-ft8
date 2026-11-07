#pragma once

// How many candidates decode_slot() asks ftx_find_candidates() for.
// The recall harness and the decode bench's "today" row take this value, so a
// change here is what CI scores (tools/decode_recall).
#define DECODE_MAX_CANDIDATES 256

// The slot's decode budget in units (decode_core.h; decode.md, The work budget): an LDPC iteration is
// one unit, a search DECODE_COST_SEARCH, a candidate tried DECODE_COST_CANDIDATE beside its
// iterations. The costs are decode_bench's on the P4 (2026-11-06): a search about 370 iterations, a
// candidate nothing the five slots could separate from its iterations. With the second worker
// (decode_workers.h) a unit costs 212 us there against 311 us, and 10,800 units fit the 2.36 s guard
// with 2.3 s (decode.md, The work budget); one worker fitted 6,000. The recall harness scores these
// values, like the cap above.
#define DECODE_BUDGET_UNITS 10800
#define DECODE_COST_SEARCH 370
#define DECODE_COST_CANDIDATE 0

// FT4 beside FT8 on one source (docs/architecture/decode.md, FT4). An LDPC iteration costs FT4 what it
// costs FT8, and an FT4 search 0.24 of an FT8 one, on the host (tools/decode_bench/score_ft4_edge.py
// --ours, 2026-11-07): about 90 units on the P4, scaled from DECODE_COST_SEARCH. Estimated, not yet
// timed on the P4.
#define DECODE_COST_SEARCH_FT4 90
// FT4's second period decodes in the window FT8's slot does, both due at the FT8 boundary, so with both
// channels they split DECODE_BUDGET_UNITS: FT8 keeps what the corpora's busiest slot needs (7,580) with
// room, FT4 the rest. Its first period decodes alone and has the whole budget. Sized on one 20 m
// corpus with about 8 FT4 stations a period, where FT4 at 3,000 lost no more than at 4,000; a busy FT4
// band is unmeasured.
#define DECODE_BUDGET_FT8_BESIDE_FT4 7800
#define DECODE_BUDGET_FT4_BESIDE_FT8 3000
