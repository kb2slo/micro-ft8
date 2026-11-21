#pragma once
#include <cstdint>
#include <string>

// Decode-result data types shared by autoseq and the decode pipeline. Plain
// data, no dependency beyond the standard headers.

// A decode's mode.
enum class RxMode : uint8_t { FT8 = 0, FT4 = 1 };

// The channel a decode was heard on, and so the channel a QSO from it runs on (docs/architecture/autoseq.md,
// The queue): its dial, the RF frequency in Hz its audio offsets count from, and its mode. Two channels are one
// only when both match; band is derived from the dial. A dial of 0 is unknown.
struct RxChannel {
    uint32_t dial_hz = 0;
    RxMode mode = RxMode::FT8;
};
inline bool operator==(const RxChannel& a, const RxChannel& b) { return a.dial_hz == b.dial_hz && a.mode == b.mode; }
inline bool operator!=(const RxChannel& a, const RxChannel& b) { return !(a == b); }

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
    RxChannel channel;
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
