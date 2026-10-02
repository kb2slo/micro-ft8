#pragma once
#include <string>

// Decode-result data types shared by autoseq and the decode pipeline. Plain
// data, no dependency beyond <string>.

// A lightweight RX line format you can fill from your decoder
struct UiRxLine {
    std::string text;  // already formatted for display
    int snr = 0;
    int offset_hz = 0; // audio-bin offset in Hz relative to passband center
    int slot_id = 0;   // 0 = even slot (0/30s), 1 = odd slot (15/45s)
    std::string field1; // parsed token 1 (call/CQ marker)
    std::string field2; // parsed token 2
    std::string field3; // parsed token 3 (grid/report/etc)
    bool is_cq = false;
    bool is_to_me = false;
    bool is_recent_qso = false;
};

// Plain-C RX entry used for zero-heap decode/display pipeline.
// Fixed-size char arrays avoid std::string heap allocations.
#define RX_MAX_DECODES  32
#define RX_TEXT_MAX     64
#define RX_FIELD_MAX    20

struct RxDecodeEntry {
    char text[RX_TEXT_MAX];
    char field1[RX_FIELD_MAX];
    char field2[RX_FIELD_MAX];
    char field3[RX_FIELD_MAX];
    int  snr;
    int  offset_hz;
    int  slot_id;
    float time_s;
    bool is_cq;
    bool is_to_me;
    bool is_recent_qso;
};
