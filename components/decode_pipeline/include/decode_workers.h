#pragma once

// A search's candidates, run in candidate order by the caller's thread, with an optional helper thread
// that decodes the next candidate ahead (docs/architecture/decode.md, Channels). The output is the
// serial order's byte for byte whatever the timing: every skip, budget check and commit runs on the
// caller's thread, in order, and a decode run ahead that a commit before it invalidated is decoded
// again (decode_core.h). The only copy of the loop: decode_slot() and the decode bench both call it.
//
// Portable C on pthreads: the host runs it threaded, and on the P4 ESP-IDF maps a pthread onto a
// FreeRTOS task pinned where decode_helper_start() asks.

#include "decode_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct decode_helper decode_helper_t;

// Starts the helper thread, once: a second call returns the same helper. `core` is the core it is pinned
// to on the P4 (ignored on the host), `priority` its FreeRTOS priority. NULL when it could not start; the
// caller then runs serially.
decode_helper_t* decode_helper_start(int core, int priority);

// Called on the caller's thread for each DECODE_CORE_NEW, in candidate order. Returns 0 to stop the
// search before the next candidate (a list is full), 1 to go on.
typedef int (*decode_new_fn)(void* context, const decode_core_message_t* message);

// Runs candidates 0..found-1 of the search decode_core_search() just ran: skip, afford, decode, commit.
// `helper` NULL is the serial order. Returns 0 if on_new stopped it, else 1; core->cut says whether the
// budget did. Adds to *wasted the decodes run ahead and never committed. The helper is idle on return.
int decode_drive_search(decode_core_t* core, int found, decode_helper_t* helper, decode_new_fn on_new,
                        void* context, int* wasted);

#ifdef __cplusplus
}
#endif
