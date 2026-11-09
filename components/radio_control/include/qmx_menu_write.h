#pragma once

// Sets the QMX's PTT outputs for an amplifier with the Menu Manager (architecture/radio.md, Setting
// the PTT outputs): on every band whose Transmit is ENABLED, the keying output ENABLED and the other
// three DISABLED, and Serial 6 on PTT DISABLED; then `MU;` so the radio takes them at once. Which
// bands transmit stays the operator's choice in the radio, so the write follows the Transmit flags
// the read found and changes none of them. Only the cells that differ are written: a radio already
// right gets no write, since each lands in its EEPROM. Pure, as qmx_menu_read.h.
//
// Each cell goes out as its write and a read of it (`MM<path>=<value>;MM<path>;`), and the cell is
// done when an MM reply carries the new value: the manual does not say what a write answers, so
// this holds whether or not it echoes, and proves every cell. `?;` or no such reply in time stops
// the write (Failed); what was written stays written, and the re-read after shows the radio as it is.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "qmx_menu_read.h"

// How the amplifier is keyed: which TX PTT output the write turns on.
enum class QmxPttKeying : uint8_t { Ground = 1, Plus5v = 2 };

static constexpr int kQmxMenuWriteMax = kQmxMenuBandColumns * kQmxFlagCount + 1;
static constexpr int kQmxSerial6Item = -1;  // QmxMenuWriteItem::flag for Serial 6 on PTT

struct QmxMenuWriteItem {
    int8_t column = 0;
    int8_t flag = 0;  // a QmxBandFlag, or kQmxSerial6Item
    bool enable = false;
};

enum class QmxMenuWritePhase : uint8_t { Idle, Waiting, Reload, Done };

enum class QmxMenuWriteAction : uint8_t {
    None,
    Send,    // send qmx_menu_write_command()
    Reload,  // send `MU;`: every cell is written
    Done,    // finished: read the radio again
    Failed,  // stopped at the current item (refused, or not read back in time)
};

static constexpr int64_t kQmxMenuWriteReplyMs = 1000;

struct QmxMenuWrite {
    QmxMenuWriteItem items[kQmxMenuWriteMax];
    int count = 0;
    int next = 0;
    QmxMenuWritePhase phase = QmxMenuWritePhase::Done;
    int64_t deadline_ms = 0;
    uint32_t mm_seen = 0;
    uint32_t refused_seen = 0;
    bool refused = false;  // why a Failed stopped: refused, else not read back in time
};

// The flags the keying wants on a band: the chosen TX output alone.
static inline bool qmx_ptt_wanted(QmxPttKeying keying, int flag)
{
    return (flag == kQmxFlagTxPttGround && keying == QmxPttKeying::Ground) ||
           (flag == kQmxFlagTxPtt5v && keying == QmxPttKeying::Plus5v);
}

// Plans the write from a finished read. Returns the number of cells to write (0: already right);
// the write is then ready for its first step.
static inline int qmx_menu_write_plan(const QmxRadioSettings& radio, QmxPttKeying keying, QmxMenuWrite& w)
{
    w = QmxMenuWrite{};
    for (int c = 0; c < radio.band_count && c < kQmxMenuBandColumns; ++c) {
        const QmxBandSettings& b = radio.bands[c];
        if (b.flag[kQmxFlagTransmit] != 1) {
            continue;
        }
        for (int f = kQmxFlagTxPtt5v; f < kQmxFlagCount; ++f) {
            const bool want = qmx_ptt_wanted(keying, f);
            if (b.flag[f] != (want ? 1 : 0)) {
                w.items[w.count++] = QmxMenuWriteItem{static_cast<int8_t>(c), static_cast<int8_t>(f), want};
            }
        }
    }
    if (radio.serial6_on_ptt != 0) {
        w.items[w.count++] = QmxMenuWriteItem{0, kQmxSerial6Item, false};
    }
    w.phase = w.count > 0 ? QmxMenuWritePhase::Idle : QmxMenuWritePhase::Done;
    return w.count;
}

static inline const char* qmx_menu_write_value(const QmxMenuWriteItem& item)
{
    return item.enable ? "ENABLED" : "DISABLED";
}

// The item's path, such as `Band config.|TX PTT grounded[3]`.
static inline void qmx_menu_write_path(const QmxMenuWriteItem& item, char* out, size_t cap)
{
    if (item.flag == kQmxSerial6Item) {
        snprintf(out, cap, "System config|GPS & Ser. ports|Serial 6 on PTT");
    } else {
        snprintf(out, cap, "Band config.|%s[%d]", kQmxBandFlagRow[item.flag], item.column);
    }
}

// The current item's write and its read-back, in one send.
static inline void qmx_menu_write_command(const QmxMenuWrite& w, char* out, size_t cap)
{
    if (w.next >= w.count) {
        if (cap > 0) {
            out[0] = 0;
        }
        return;
    }
    char path[64];
    qmx_menu_write_path(w.items[w.next], path, sizeof(path));
    snprintf(out, cap, "MM%s=%s;MM%s;", path, qmx_menu_write_value(w.items[w.next]), path);
}

// One step, as qmx_menu_read_step(). A link that drops ends the write as Failed: the radio may be
// another when it comes back.
static inline QmxMenuWriteAction qmx_menu_write_step(QmxMenuWrite& w, bool ready, bool busy, uint32_t mm_count,
                                                     const char* mm_value, uint32_t refused_count, int64_t now_ms)
{
    if (w.phase == QmxMenuWritePhase::Done) {
        return QmxMenuWriteAction::None;
    }
    if (!ready) {
        w.phase = QmxMenuWritePhase::Done;
        return QmxMenuWriteAction::Failed;
    }
    switch (w.phase) {
        case QmxMenuWritePhase::Idle:
            if (busy) {
                return QmxMenuWriteAction::None;
            }
            w.phase = QmxMenuWritePhase::Waiting;
            w.deadline_ms = now_ms + kQmxMenuWriteReplyMs;
            w.mm_seen = mm_count;
            w.refused_seen = refused_count;
            return QmxMenuWriteAction::Send;
        case QmxMenuWritePhase::Waiting:
            if (busy) {
                w.phase = QmxMenuWritePhase::Idle;  // asked again after; a write is the same twice
                return QmxMenuWriteAction::None;
            }
            if (refused_count != w.refused_seen) {
                w.phase = QmxMenuWritePhase::Done;
                w.refused = true;
                return QmxMenuWriteAction::Failed;
            }
            if (mm_count != w.mm_seen && strcmp(mm_value, qmx_menu_write_value(w.items[w.next])) == 0) {
                if (++w.next >= w.count) {
                    w.phase = QmxMenuWritePhase::Reload;
                    return QmxMenuWriteAction::Reload;
                }
                w.phase = QmxMenuWritePhase::Idle;
                return qmx_menu_write_step(w, ready, busy, mm_count, mm_value, refused_count, now_ms);
            }
            if (now_ms >= w.deadline_ms) {
                w.phase = QmxMenuWritePhase::Done;
                return QmxMenuWriteAction::Failed;
            }
            return QmxMenuWriteAction::None;
        case QmxMenuWritePhase::Reload:
            w.phase = QmxMenuWritePhase::Done;
            return QmxMenuWriteAction::Done;
        case QmxMenuWritePhase::Done:
            break;
    }
    return QmxMenuWriteAction::None;
}
