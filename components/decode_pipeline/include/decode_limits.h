#pragma once

// How many candidates decode_monitor_results() asks ftx_find_candidates() for.
// The recall harness and the decode bench's "today" row take this value, so a
// change here is what CI scores (tools/decode_recall).
#define DECODE_MAX_CANDIDATES 256
