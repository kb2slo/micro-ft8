#pragma once

// Puts the QMX in IQ mode at each CAT link-up and reads it back (architecture/radio.md, The QMX's
// receive output). The P4 decodes the QMX only from its I/Q, and the radio forgets IQ mode at power-off,
// so every link-up asks again. Pure: the caller sends the commands and reads the reply, so the host
// test drives it with made-up times and replies.
//
// Per link-up: `Q9 1;` then `Q9;` (Send), wait for the radio to say which mode it is in, and report
// it (Matched, Mismatch), or give up (NoReply). Neither is retried: a radio that refuses IQ mode
// refuses it again, and the caller says so once. A write that fails is retried after a pause, a few
// times (qmx_iq_link_send_failed).

#include <stdbool.h>
#include <stdint.h>

enum class QmxIqLinkPhase : uint8_t {
    Idle,     // the command has not been sent on this link
    Waiting,  // sent; the reply is awaited
    Done,     // answered or given up on; nothing more until the link drops
};

enum class QmxIqLinkAction : uint8_t {
    None,
    Send,      // reset the Q9 reply, send `Q9 1;` then `Q9;`
    Matched,   // the radio reads back IQ mode
    Mismatch,  // the radio reads back audio mode: nothing will decode
    NoReply,   // no Q9 reply in time: a firmware without IQ mode answers `?;`
};

static constexpr int64_t kQmxIqLinkReplyMs = 1000;
// A send that fails (the write blocks up to 200 ms) is retried after this, up to kQmxIqLinkSendTries
// in all; then the link gives up on this link-up.
static constexpr int64_t kQmxIqLinkRetryMs = 5000;
static constexpr int kQmxIqLinkSendTries = 3;

struct QmxIqLink {
    QmxIqLinkPhase phase = QmxIqLinkPhase::Idle;
    int64_t deadline_ms = 0;
    int64_t retry_at_ms = 0;
    int send_failures = 0;
};

// The caller's write of the Send failed. Returns true when this was the last try: the link is Done
// and the caller says so once. Otherwise the next Send comes kQmxIqLinkRetryMs later.
static inline bool qmx_iq_link_send_failed(QmxIqLink& link, int64_t now_ms)
{
    if (++link.send_failures >= kQmxIqLinkSendTries) {
        link.phase = QmxIqLinkPhase::Done;
        return true;
    }
    link.phase = QmxIqLinkPhase::Idle;
    link.retry_at_ms = now_ms + kQmxIqLinkRetryMs;
    return false;
}

// One step. `ready`: CAT is up. `busy`: a transmission, Tune, sweep or prime is running, so no command
// goes out now (a waiting probe starts again afterwards). `reply`: the last Q9 reply, 1 IQ, 0 audio,
// -1 none since the command was sent.
static inline QmxIqLinkAction qmx_iq_link_step(QmxIqLink& link, bool ready, bool busy, int reply, int64_t now_ms)
{
    if (!ready) {
        link = QmxIqLink{};
        return QmxIqLinkAction::None;
    }
    switch (link.phase) {
        case QmxIqLinkPhase::Idle:
            if (busy || now_ms < link.retry_at_ms) {
                return QmxIqLinkAction::None;
            }
            link.phase = QmxIqLinkPhase::Waiting;
            link.deadline_ms = now_ms + kQmxIqLinkReplyMs;
            return QmxIqLinkAction::Send;
        case QmxIqLinkPhase::Waiting:
            if (busy) {
                link.phase = QmxIqLinkPhase::Idle;  // a transmission clears replies: ask again after it
                return QmxIqLinkAction::None;
            }
            if (reply == 0 || reply == 1) {
                link.phase = QmxIqLinkPhase::Done;
                return reply == 1 ? QmxIqLinkAction::Matched : QmxIqLinkAction::Mismatch;
            }
            if (now_ms >= link.deadline_ms) {
                link.phase = QmxIqLinkPhase::Done;
                return QmxIqLinkAction::NoReply;
            }
            return QmxIqLinkAction::None;
        case QmxIqLinkPhase::Done:
            return QmxIqLinkAction::None;
    }
    return QmxIqLinkAction::None;
}
