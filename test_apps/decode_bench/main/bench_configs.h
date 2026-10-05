#pragma once

// Configurations timed by the decode benchmark (docs/architecture/decode.md). Shared by the device image and the host
// parity binary so both run exactly the same list. min_score 5 and 25 LDPC iterations are the firmware's.

#include "bench_core.h"
#include "decode_limits.h"

// subtract_passes stays 0 in every row: these time the search without subtraction.
// The firmware searches up to twice (DECODE_SUBTRACT_PASSES in decode_subtract.h).
static const BenchConfig k_bench_configs[] = {
    // name            t  f  candidates              decodes score iterations
    { "today",         2, 1,  DECODE_MAX_CANDIDATES, 32,     5,    25 },   // firmware as shipped
    { "cand256",       2, 1, 256,                    50,     5,    25 },   // same candidate cap, DEC_MAX 50
    { "swap_t1f2",     1, 2, 256,        50,     5,    25 },   // same waterfall bytes as today
    { "t2f2",          2, 2, 256,        50,     5,    25 },   // P4 target; S3 only via a 4-bit waterfall
    { "t4f2",          4, 2, 256,        50,     5,    25 },   // P4 only: 322 KB waterfall
};

#define BENCH_CONFIG_COUNT ((int)(sizeof(k_bench_configs) / sizeof(k_bench_configs[0])))
