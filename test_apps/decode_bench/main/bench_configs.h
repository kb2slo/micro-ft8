#pragma once

// Configurations timed by the decode benchmark (docs/architecture/decode.md). Shared by the device image and the host
// parity binary so both run exactly the same list. min_score 5 and 25 LDPC iterations are the firmware's.

#include "bench_core.h"
#include "decode_limits.h"
#include "decode_subtract.h"

// "today" is the firmware's decode: two searches with subtraction, under its budget; "today_2w" the same with
// the second worker (decode_workers.h), the output identical. "full" and "full_2w" lift the budget, so each
// slot runs all its work, and "full3" and "full3_2w" a third search too: the cost per unit with one and two
// workers is fitted from their RESULT lines
// (docs/architecture/decode.md, The work budget). The other rows keep subtract_passes 0 and no budget, so
// they time one search alone, as they always have.
static const BenchConfig k_bench_configs[] = {
    { .name = "today", .time_osr = 2, .freq_osr = 2, .max_candidates = DECODE_MAX_CANDIDATES, .max_decodes = 32,
      .min_score = 5, .ldpc_iterations = 25, .subtract_passes = DECODE_SUBTRACT_PASSES,
      .subtract_neighbors = DECODE_SUBTRACT_NEIGHBORS, .subtract_fraction = DECODE_SUBTRACT_FRACTION,
      .budget_units = DECODE_BUDGET_UNITS, .cost_search = DECODE_COST_SEARCH,
      .cost_candidate = DECODE_COST_CANDIDATE },  // firmware as shipped
    { .name = "today_2w", .time_osr = 2, .freq_osr = 2, .max_candidates = DECODE_MAX_CANDIDATES, .max_decodes = 32,
      .min_score = 5, .ldpc_iterations = 25, .subtract_passes = DECODE_SUBTRACT_PASSES,
      .subtract_neighbors = DECODE_SUBTRACT_NEIGHBORS, .subtract_fraction = DECODE_SUBTRACT_FRACTION,
      .budget_units = DECODE_BUDGET_UNITS, .cost_search = DECODE_COST_SEARCH,
      .cost_candidate = DECODE_COST_CANDIDATE, .workers = 2 },
    { .name = "full", .time_osr = 2, .freq_osr = 2, .max_candidates = DECODE_MAX_CANDIDATES, .max_decodes = 32,
      .min_score = 5, .ldpc_iterations = 25, .subtract_passes = DECODE_SUBTRACT_PASSES,
      .subtract_neighbors = DECODE_SUBTRACT_NEIGHBORS, .subtract_fraction = DECODE_SUBTRACT_FRACTION,
      .cost_search = DECODE_COST_SEARCH, .cost_candidate = DECODE_COST_CANDIDATE },
    { .name = "full_2w", .time_osr = 2, .freq_osr = 2, .max_candidates = DECODE_MAX_CANDIDATES, .max_decodes = 32,
      .min_score = 5, .ldpc_iterations = 25, .subtract_passes = DECODE_SUBTRACT_PASSES,
      .subtract_neighbors = DECODE_SUBTRACT_NEIGHBORS, .subtract_fraction = DECODE_SUBTRACT_FRACTION,
      .cost_search = DECODE_COST_SEARCH, .cost_candidate = DECODE_COST_CANDIDATE, .workers = 2 },
    // Three searches, no budget: more work a slot, so the fit spans more units.
    { .name = "full3", .time_osr = 2, .freq_osr = 2, .max_candidates = DECODE_MAX_CANDIDATES, .max_decodes = 32,
      .min_score = 5, .ldpc_iterations = 25, .subtract_passes = 3,
      .subtract_neighbors = DECODE_SUBTRACT_NEIGHBORS, .subtract_fraction = DECODE_SUBTRACT_FRACTION,
      .cost_search = DECODE_COST_SEARCH, .cost_candidate = DECODE_COST_CANDIDATE },
    { .name = "full3_2w", .time_osr = 2, .freq_osr = 2, .max_candidates = DECODE_MAX_CANDIDATES, .max_decodes = 32,
      .min_score = 5, .ldpc_iterations = 25, .subtract_passes = 3,
      .subtract_neighbors = DECODE_SUBTRACT_NEIGHBORS, .subtract_fraction = DECODE_SUBTRACT_FRACTION,
      .cost_search = DECODE_COST_SEARCH, .cost_candidate = DECODE_COST_CANDIDATE, .workers = 2 },
    // name            t  f  candidates              decodes score iterations
    { "cand256",       2, 1, 256,                    50,     5,    25 },   // same candidate cap, DEC_MAX 50
    { "swap_t1f2",     1, 2, 256,        50,     5,    25 },   // same waterfall bytes as today
    { "t2f2",          2, 2, 256,        50,     5,    25 },   // P4 target; S3 only via a 4-bit waterfall
    { "t4f2",          4, 2, 256,        50,     5,    25 },   // P4 only: 322 KB waterfall
};

#define BENCH_CONFIG_COUNT ((int)(sizeof(k_bench_configs) / sizeof(k_bench_configs[0])))
