#pragma once
// JSON test-data parser for Micro-FT8 host mock.
// Adapted from dxft8-tests — uses C++ UiRxLine with std::string fields.

#include <string>
#include <vector>
#include "decode_types.h"    // UiRxLine

// ---------- Test data structures ----------

struct TestConfig {
    std::string my_callsign;
    std::string my_grid;
    std::string dx_callsign;
    std::string dx_grid;
    bool        tx_on_even = false;
    std::string cq_type;       // "CQ", "FD", "SOTA", etc.
    std::string free_text;     // FD exchange or freetext (e.g. "1A SCV")
    int         tries = 6;     // autoseq_set_tries (default AUTOSEQ_TRIES)
    bool        skip_tx1 = false; // autoseq_set_skip_tx1: a touch starts at the report
};

// Call CQ (the CQ row), or drop the queued CQ (its x).
struct CqCall {
    float time_offset = 0.0f;
    int   tries       = 0;       // the CQ's own count; 0 = the station default
    int   parity      = -1;      // -1 = TX_ON_EVEN's
};

struct CqDrop {
    float time_offset = 0.0f;
};

struct TouchEvent {
    float time_offset    = 0.0f;
    int   message_index  = 0;
};

struct FreeTextEvent {
    float       time_offset = 0.0f;
    std::string text;
};

struct TestPeriod {
    std::vector<UiRxLine> messages;     // RX messages for this period
    bool         has_cq_call = false;
    CqCall       cq_call;
    bool         has_cq_drop = false;
    CqDrop       cq_drop;
    bool         has_touch_event = false;
    TouchEvent   touch_event;
    bool          has_freetext_event = false;
    FreeTextEvent freetext_event;
    // expect.tx is required. Empty means this period transmits nothing.
    // log and cabrillo are optional; absent means that line must not appear.
    bool        has_expect_tx = false;
    std::string expect_tx;
    bool        has_expect_log = false;
    std::string expect_log;
    bool        has_expect_cabrillo = false;
    std::string expect_cabrillo;
};

struct TestData {
    TestConfig              config;
    std::vector<TestPeriod> periods;
};

// ---------- API ----------

bool load_test_data(const std::string& filename, TestData& out);
