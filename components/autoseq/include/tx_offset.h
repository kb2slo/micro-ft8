#pragma once

#include <cstdint>

// The audio offset a transmission goes out on (docs/architecture/autoseq.md,
// TX offset). One policy, header only, so host_mock tests it; the main board
// supplies the random number and keeps the held offset on the queue entry
// (QsoContext::tx_offset_hz, autoseq_hold_tx_offset()).
//
// A QSO keeps one offset from its first transmission to its end. WSJT-X
// decodes a QSO partner's messages with the expected bits filled in only
// within about 50 Hz of where it last heard them (the QEX paper's a-priori
// decoding), so a station that moves between its report and its R-report gives
// that gain away on the weak contacts that need it. An unanswered CQ rolls
// every time, to step off a frequency that may be occupied where we cannot
// hear it.

constexpr int kTxOffsetSrcRandom = 0;
constexpr int kTxOffsetSrcCursor = 1;
constexpr int kTxOffsetSrcRx = 2;

struct TxOffsetIn {
    int src = kTxOffsetSrcRandom;  // the Station key offset_src
    int cursor_hz = 0;             // the Station key offset
    int rx_offset_hz = 0;          // where DX was heard (AutoseqTxEntry::offset_hz)
    bool is_cq = false;            // our CQ, not a QSO message
    int held_hz = 0;               // the entry's held offset, 0 if none yet
    uint32_t random = 0;           // any 32 random bits
};

struct TxOffsetOut {
    int offset_hz = 1500;
    bool hold = false;  // a first roll for a QSO: keep offset_hz on its entry
};

inline TxOffsetOut tx_offset_resolve(const TxOffsetIn& in) {
    TxOffsetOut out;
    if (in.src == kTxOffsetSrcCursor) {
        out.offset_hz = in.cursor_hz > 0 ? in.cursor_hz : 1500;
        return out;
    }
    if (in.src == kTxOffsetSrcRx && in.rx_offset_hz > 0 && !in.is_cq) {
        out.offset_hz = in.rx_offset_hz;
        return out;
    }
    if (!in.is_cq && in.held_hz > 0) {
        out.offset_hz = in.held_hz;
        return out;
    }
    out.offset_hz = 500 + (int)(in.random % 2001);  // 500 to 2500 Hz
    out.hold = !in.is_cq;
    return out;
}
