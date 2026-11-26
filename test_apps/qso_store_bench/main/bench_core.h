#pragma once

#include <cstddef>
#include <cstdint>

#include "qso_store.h"

// The QSO store's read costs (README.md). The same code runs on the host and the P4; each side supplies the
// buffers and a clock.
struct BenchMemory {
    uint8_t* image;
    size_t image_bytes;
    QsoStoreEntry* entries;
    size_t entry_count;
    uint32_t* call_slots;
    size_t call_slot_count;  // a power of two, at least twice entry_count
};

using BenchClockFn = int64_t (*)();  // microseconds, monotonic

// Prints `BENCH begin`, one `RESULT` line per log size and operation, a `CHECK` line per size, and `BENCH done`.
// Returns false when a check failed.
bool bench_run(const BenchMemory& memory, BenchClockFn now_us, const char* where);

// The same reads over an operator's own log, `text` being a whole ADIF file (header, then records). Prints
// `BENCH begin`, `RESULT` lines, one `CHECK` and `BENCH done`. Returns false when a check failed.
bool bench_run_log(const BenchMemory& memory, BenchClockFn now_us, const char* where, const char* text, size_t text_len);
