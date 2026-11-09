#pragma once

// Reads, at each CAT link-up, the QMX settings the station depends on, with the Menu Manager
// (architecture/radio.md, Menu Manager): each band's name, Transmit flag and four PTT outputs from
// the Band config. table, then System config's Band version and Serial 6 on PTT. Pure: the caller
// sends each command and hands back the replies, so the host test drives it with made-up replies.
//
// One query at a time: `MM<path>;` then wait for its `MM<value>;`, or `?;` for an item the radio
// refused (read as unknown, and the read goes on). The band names are read from column 0 until a
// column reads 0 (no band) or the table's 16 columns end; only those columns' flags are read. No
// reply in time ends the read (NoReply): a radio that has stopped answering is not asked 70 times.
// A transmission pauses it, and the query in flight is asked again after. A link that drops forgets
// everything, since the next radio plugged in may be another.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static constexpr int kQmxMenuBandColumns = 16;  // Band config.[16] on QMX 1_04_010
static constexpr int64_t kQmxMenuReplyMs = 1000;

// A Band config. flag: 1 ENABLED, 0 DISABLED, -1 not read (refused, or not a known value).
enum QmxBandFlag : uint8_t {
    kQmxFlagTransmit,
    kQmxFlagTxPtt5v,
    kQmxFlagTxPttGround,
    kQmxFlagRxPtt5v,
    kQmxFlagRxPttGround,
    kQmxFlagCount,
};

// The Band config. row each flag is read from, by the name the radio gives it.
static constexpr const char* kQmxBandFlagRow[kQmxFlagCount] = {
    "Transmit", "TX PTT +5V", "TX PTT grounded", "RX PTT +5V", "RX PTT grounded",
};

struct QmxBandSettings {
    int band_m = 0;  // Band name (m): 160, 80, ... 6
    int8_t flag[kQmxFlagCount] = {-1, -1, -1, -1, -1};
};

struct QmxRadioSettings {
    int band_count = 0;  // columns 0 .. band_count-1 hold a band
    QmxBandSettings bands[kQmxMenuBandColumns];
    char band_version[24] = {};  // "QMX+ 160-6m"; empty when not read
    int8_t serial6_on_ptt = -1;  // 1 ENABLED (the PTT jack is a serial port), 0 DISABLED, -1 not read
};

enum class QmxMenuReadPhase : uint8_t {
    Idle,     // the current query has not been sent
    Waiting,  // sent; its reply is awaited
    Done,     // read, or given up on; nothing more until the link drops
};

enum class QmxMenuReadAction : uint8_t {
    None,
    Send,     // send qmx_menu_read_command()
    Done,     // every item read: result holds the radio's settings
    NoReply,  // a query went unanswered: the read stopped, result is partial
};

// What is being read: the band names, then each band's flags, then the two System config items.
enum class QmxMenuStage : uint8_t { BandName, BandFlag, BandVersion, Serial6, Finished };

struct QmxMenuRead {
    QmxMenuReadPhase phase = QmxMenuReadPhase::Idle;
    QmxMenuStage stage = QmxMenuStage::BandName;
    int column = 0;
    int flag = 0;
    int64_t deadline_ms = 0;
    uint32_t mm_seen = 0;       // the caller's MM reply count when the query went out
    uint32_t refused_seen = 0;  // and its `?;` count
    QmxRadioSettings result;
};

// The command for the current query, such as `MMBand config.|TX PTT grounded[3];`.
static inline void qmx_menu_read_command(const QmxMenuRead& read, char* out, size_t cap)
{
    switch (read.stage) {
        case QmxMenuStage::BandName:
            snprintf(out, cap, "MMBand config.|Band name (m)[%d];", read.column);
            return;
        case QmxMenuStage::BandFlag:
            snprintf(out, cap, "MMBand config.|%s[%d];", kQmxBandFlagRow[read.flag], read.column);
            return;
        case QmxMenuStage::BandVersion:
            snprintf(out, cap, "MMSystem config|Band version;");
            return;
        case QmxMenuStage::Serial6:
            snprintf(out, cap, "MMSystem config|GPS & Ser. ports|Serial 6 on PTT;");
            return;
        case QmxMenuStage::Finished:
            break;
    }
    if (cap > 0) {
        out[0] = 0;
    }
}

// ENABLED 1, DISABLED 0, anything else -1.
static inline int8_t qmx_menu_flag_value(const char* value)
{
    if (strcmp(value, "ENABLED") == 0) {
        return 1;
    }
    if (strcmp(value, "DISABLED") == 0) {
        return 0;
    }
    return -1;
}

// A band name: digits only, else 0 (no band).
static inline int qmx_menu_band_value(const char* value)
{
    int m = 0;
    if (value[0] == 0) {
        return 0;
    }
    for (const char* p = value; *p; ++p) {
        if (*p < '0' || *p > '9' || m > 100000) {
            return 0;
        }
        m = m * 10 + (*p - '0');
    }
    return m;
}

// Stores one reply (`value` null: refused) and moves to the next query.
static inline void qmx_menu_read_take(QmxMenuRead& read, const char* value)
{
    QmxRadioSettings& r = read.result;
    switch (read.stage) {
        case QmxMenuStage::BandName: {
            const int m = value ? qmx_menu_band_value(value) : 0;
            if (m > 0) {
                r.bands[read.column].band_m = m;
                r.band_count = read.column + 1;
                if (++read.column < kQmxMenuBandColumns) {
                    return;
                }
            }
            read.column = 0;
            read.flag = 0;
            read.stage = r.band_count > 0 ? QmxMenuStage::BandFlag : QmxMenuStage::BandVersion;
            return;
        }
        case QmxMenuStage::BandFlag:
            r.bands[read.column].flag[read.flag] = value ? qmx_menu_flag_value(value) : -1;
            if (++read.flag < kQmxFlagCount) {
                return;
            }
            read.flag = 0;
            if (++read.column < r.band_count) {
                return;
            }
            read.stage = QmxMenuStage::BandVersion;
            return;
        case QmxMenuStage::BandVersion:
            if (value) {
                snprintf(r.band_version, sizeof(r.band_version), "%s", value);
            }
            read.stage = QmxMenuStage::Serial6;
            return;
        case QmxMenuStage::Serial6:
            r.serial6_on_ptt = value ? qmx_menu_flag_value(value) : -1;
            read.stage = QmxMenuStage::Finished;
            return;
        case QmxMenuStage::Finished:
            return;
    }
}

// One step. `ready`: CAT is up. `busy`: a transmission, Tune, sweep or prime is running, so nothing
// goes out (a query in flight is asked again after). `mm_count` and `refused_count`: how many MM
// replies and `?;` replies the radio has sent since boot, never reset; `mm_value`: the last MM
// reply's value, without `MM` and `;`.
static inline QmxMenuReadAction qmx_menu_read_step(QmxMenuRead& read, bool ready, bool busy, uint32_t mm_count,
                                                   const char* mm_value, uint32_t refused_count, int64_t now_ms)
{
    if (!ready) {
        read = QmxMenuRead{};
        return QmxMenuReadAction::None;
    }
    switch (read.phase) {
        case QmxMenuReadPhase::Idle:
            if (busy) {
                return QmxMenuReadAction::None;
            }
            read.phase = QmxMenuReadPhase::Waiting;
            read.deadline_ms = now_ms + kQmxMenuReplyMs;
            read.mm_seen = mm_count;
            read.refused_seen = refused_count;
            return QmxMenuReadAction::Send;
        case QmxMenuReadPhase::Waiting:
            if (busy) {
                read.phase = QmxMenuReadPhase::Idle;
                return QmxMenuReadAction::None;
            }
            if (mm_count != read.mm_seen) {
                qmx_menu_read_take(read, mm_value);
            } else if (refused_count != read.refused_seen) {
                qmx_menu_read_take(read, nullptr);
            } else if (now_ms >= read.deadline_ms) {
                read.phase = QmxMenuReadPhase::Done;
                return QmxMenuReadAction::NoReply;
            } else {
                return QmxMenuReadAction::None;
            }
            if (read.stage == QmxMenuStage::Finished) {
                read.phase = QmxMenuReadPhase::Done;
                return QmxMenuReadAction::Done;
            }
            // The next query goes out at once, as Idle would send it.
            read.deadline_ms = now_ms + kQmxMenuReplyMs;
            read.mm_seen = mm_count;
            read.refused_seen = refused_count;
            return QmxMenuReadAction::Send;
        case QmxMenuReadPhase::Done:
            return QmxMenuReadAction::None;
    }
    return QmxMenuReadAction::None;
}
