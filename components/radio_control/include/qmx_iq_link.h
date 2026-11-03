#pragma once

// Which receive output the QMX is set to, set at each CAT link-up and again whenever what the station
// wants changes (architecture/radio.md, The QMX's receive output; architecture/decode.md, I/Q front
// end; qmx_rx_mode.h decides when). Pure: the caller sends the commands and reads the reply, so the
// host test drives it with made-up times and replies.
//
// Per send: `Q9 n;` then `Q9;` (SEND), wait for the radio to say which it is set to, and compare it
// with what the station wants (MATCHED, MISMATCH), or give up (NO_REPLY). Either way the capture path
// has already been chosen by the schedule, so a mismatch is a warning, not a retry: the radio's reply
// does not change what the P4 decodes.

#include <stdbool.h>
#include <stdint.h>

enum class QmxIqLinkPhase : uint8_t {
    Idle,     // the command has not been sent on this link
    Waiting,  // sent; the reply is awaited
    Done,     // answered or given up on; nothing more until the link drops or the want changes
};

enum class QmxIqLinkAction : uint8_t {
    None,
    Send,      // reset the Q9 reply, send `Q9 <want>;` then `Q9;`
    Matched,   // the radio reads back what was set
    Mismatch,  // the radio reads back the other mode
    NoReply,   // no Q9 reply in time
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
    bool sent_iq = false;  // what the last Send asked for
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

// One step. `ready`: CAT is up. `busy`: a transmission, sweep or prime is running, so no command
// goes out now (a waiting probe starts again afterwards). `want_iq`: the station's setting.
// `reply`: the last Q9 reply, 1 IQ, 0 audio, -1 none since the command was sent. A Done link whose
// `want_iq` is no longer what it last sent starts again with a fresh set of tries.
static inline QmxIqLinkAction qmx_iq_link_step(QmxIqLink& link, bool ready, bool busy, bool want_iq,
                                               int reply, int64_t now_ms)
{
    if (!ready) {
        link = QmxIqLink{};
        return QmxIqLinkAction::None;
    }
    if (link.phase == QmxIqLinkPhase::Done && want_iq != link.sent_iq) {
        link.phase = QmxIqLinkPhase::Idle;
        link.send_failures = 0;
        link.retry_at_ms = 0;
    }
    switch (link.phase) {
        case QmxIqLinkPhase::Idle:
            if (busy || now_ms < link.retry_at_ms) {
                return QmxIqLinkAction::None;
            }
            link.phase = QmxIqLinkPhase::Waiting;
            link.sent_iq = want_iq;
            link.deadline_ms = now_ms + kQmxIqLinkReplyMs;
            return QmxIqLinkAction::Send;
        case QmxIqLinkPhase::Waiting:
            if (busy) {
                link.phase = QmxIqLinkPhase::Idle;  // a transmission clears replies: ask again after it
                return QmxIqLinkAction::None;
            }
            if (want_iq != link.sent_iq) {
                link.phase = QmxIqLinkPhase::Idle;  // the answer would be to a question no longer asked
                return QmxIqLinkAction::None;
            }
            if (reply == 0 || reply == 1) {
                link.phase = QmxIqLinkPhase::Done;
                return (reply == 1) == want_iq ? QmxIqLinkAction::Matched : QmxIqLinkAction::Mismatch;
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
