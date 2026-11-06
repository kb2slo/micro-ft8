#pragma once

// One slot's decode over a filled waterfall: the candidate search, LDPC, the SNR, subtraction and the
// retry skip (docs/architecture/decode.md). Portable C, and the only copy: the P4's
// decode_slot() and the decode bench and recall harness's bench_run_slot() both drive it, so
// the recall floors score the code the P4 runs.
//
// The work is in steps so a scheduler can order and share it (docs/roadmap/several-channels.md, The
// scheduler). decode_core_decode() reads the waterfall and nothing else, so it may run ahead of the
// commits before it; every other step writes the state and runs in candidate order. Driven in order:
//
//     decode_core_begin(&core, monitor, block_gain, &config);
//     do {
//         int found = decode_core_search(&core);
//         for (int i = 0; i < found; ++i) {
//             if (decode_core_skip(&core, i)) continue;
//             if (!decode_core_afford(&core)) break;
//             decode_core_decode(&core, i, &attempt);
//             decode_core_commit(&core, i, &attempt, &message);
//         }
//     } while (decode_core_next_pass(&core));
//
// The work is counted in units (decode.md, The work budget): each search costs cost_search, each
// candidate tried cost_candidate plus the LDPC iterations it ran. A slot stops at the first candidate
// whose worst case would take it past budget_units, so where it stops depends on the waterfall alone:
// the host replays it exactly, whatever its speed.
//
// One decode_core_t per channel: it is a slot's decode in progress, not a worker's scratch.

#include <stdint.h>

#include "common/monitor.h"
#include "decode_limits.h"
#include "decode_retry.h"
#include "decode_snr.h"
#include "ft8/decode.h"
#include "ft8/message.h"

#ifdef __cplusplus
extern "C" {
#endif

// Searches at most: the third already overruns the air guard on the P4 (decode.md, Subtraction).
#define DECODE_CORE_MAX_PASSES 3
// Payloads remembered per slot, to list a station heard twice once. Open addressing: kept well above the
// most a slot lists (RX_MAX_DECODES), so it does not fill.
#define DECODE_CORE_HASH_SLOTS 128

typedef struct {
    int max_candidates;      // the first search asks for this many (at most DECODE_MAX_CANDIDATES)
    int min_score;           // ftx_find_candidates()
    int ldpc_iterations;     // ftx_decode_candidate()
    // 0: one search, no subtraction. 1..DECODE_CORE_MAX_PASSES: that many searches, each decode
    // subtracted, search p asking for max_candidates / (p + 1).
    int passes;
    int subtract_neighbors;  // decode_subtract_message()
    float subtract_fraction; // decode_subtract_message()
    ftx_callsign_hash_interface_t* hash_if;  // unpacks hashed calls, and saves them: not owned
    // The slot's budget in units; 0 is no limit. The first search always runs; everything after it,
    // the first search's own candidates included, runs only while its worst case fits.
    int budget_units;
    int cost_search;     // units a search costs
    int cost_candidate;  // units a candidate tried costs, beside its LDPC iterations
} decode_core_config_t;

typedef struct {
    const decode_core_config_t* config;  // not owned; outlives the slot
    monitor_t* monitor;                  // its waterfall is searched and subtracted; not owned
    int n_tones;
    int subtract;                        // config->passes > 0
    int passes;                          // searches allowed, 1..DECODE_CORE_MAX_PASSES
    int pass;                            // the search in progress, from 0
    int found;                           // candidates the search in progress returned
    ftx_candidate_t candidates[DECODE_MAX_CANDIDATES];
    float seen_hz[DECODE_MAX_CANDIDATES];  // this search's subtracted frequencies
    int seen;
    int spent;                           // units charged this slot
    int tried;                           // candidates decoded this slot, every search
    int cut;                             // 1 once the budget stopped work the slot would have done
    decode_retry_t retry;
    decode_snr_slot_t snr;
    ftx_message_t decoded[DECODE_CORE_HASH_SLOTS];
    int16_t decoded_used[DECODE_CORE_HASH_SLOTS];  // 1 where decoded[] holds a payload
} decode_core_t;

// What decode_core_decode() found for one candidate, before it is committed.
typedef struct {
    int ok;
    int iterations;  // LDPC iterations run
    ftx_message_t message;
} decode_core_attempt_t;

typedef enum {
    DECODE_CORE_FAILED = 0,  // LDPC or CRC failed
    DECODE_CORE_UNREADABLE,  // decoded, but did not unpack to text: not subtracted
    DECODE_CORE_REPEAT,      // a payload already decoded this slot: subtracted again, not new
    DECODE_CORE_NEW,         // a new message
} decode_core_result_t;

typedef struct {
    char text[FTX_MAX_MESSAGE_LENGTH];
    float freq_hz;
    float time_s;
    int snr;       // decode_snr_message(), read before this message is subtracted
    int standard;  // FTX_MESSAGE_TYPE_STANDARD
} decode_core_message_t;

// Starts a slot. block_gain holds the AGC gain per waterfall block and outlives the slot (decode_snr.h).
// Returns 0 when the SNR could not be prepared for this waterfall's shape; every SNR then reads the floor.
int decode_core_begin(decode_core_t* core, monitor_t* monitor, const float* block_gain,
                      const decode_core_config_t* config);

// Runs the current search. Returns the candidates found, indices 0..found-1 for the steps below.
int decode_core_search(decode_core_t* core);

// 1 when candidate i need not be tried: the other frequency subdivision of a signal this search
// subtracted, or a failure no subtraction has touched since (decode_retry.h).
int decode_core_skip(const decode_core_t* core, int i);

// 1 when the budget has room for one more candidate at its worst case: cost_candidate plus every LDPC
// iteration. 0 marks the slot cut; try no more of this search's candidates.
int decode_core_afford(decode_core_t* core);

// LDPC on candidate i. Reads the waterfall only.
void decode_core_decode(const decode_core_t* core, int i, decode_core_attempt_t* attempt);

// Commits candidate i's attempt and charges what it cost: notes a failure, or unpacks, measures the
// SNR, subtracts. Fills
// *message for DECODE_CORE_REPEAT and DECODE_CORE_NEW.
decode_core_result_t decode_core_commit(decode_core_t* core, int i, const decode_core_attempt_t* attempt,
                                        decode_core_message_t* message);

// After a search's commits. 1, and the next search is current, when another search can find anything
// (subtraction is on, a search remains, and this one subtracted something) and the budget has room
// for it and one candidate; with no room the slot is marked cut. A cut slot searches no more.
int decode_core_next_pass(decode_core_t* core);

#ifdef __cplusplus
}
#endif
