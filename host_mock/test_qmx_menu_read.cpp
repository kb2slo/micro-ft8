// qmx_menu_read: the Menu Manager read of each band's Transmit flag and PTT outputs at link-up
// (components/radio_control/include/qmx_menu_read.h). The fake radio answers as the QMX+ and the
// mid-band QMX did to tools/qmx_menu_probe on 2026-11-09.

#include <cstdio>
#include <cstring>
#include <string>

#include "qmx_menu_read.h"

static int g_fails = 0;

static void expect(bool ok, const char* msg)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    }
}

// A radio's Band config. table and System config items, answering one MM command.
struct FakeRadio {
    int bands[kQmxMenuBandColumns] = {};
    const char* flag[kQmxFlagCount][kQmxMenuBandColumns] = {};
    const char* band_version = "";
    const char* serial6 = "DISABLED";
    bool refuse_serial6 = false;

    // The reply's value, or nullptr for `?;`.
    std::string answer(const std::string& cmd, bool* refused) const
    {
        *refused = false;
        int col = -1;
        const size_t open = cmd.find('[');
        if (open != std::string::npos) {
            col = atoi(cmd.c_str() + open + 1);
        }
        if (cmd.rfind("MMBand config.|Band name (m)[", 0) == 0) {
            return std::to_string(bands[col]);
        }
        for (int f = 0; f < kQmxFlagCount; ++f) {
            const std::string row = std::string("MMBand config.|") + kQmxBandFlagRow[f] + "[";
            if (cmd.rfind(row, 0) == 0) {
                return flag[f][col] ? flag[f][col] : "DISABLED";
            }
        }
        if (cmd == "MMSystem config|Band version;") {
            return band_version;
        }
        if (cmd == "MMSystem config|GPS & Ser. ports|Serial 6 on PTT;" && !refuse_serial6) {
            return serial6;
        }
        *refused = true;
        return "";
    }
};

// Runs a read against `radio`, answering each command on the next step. Returns the last action.
static QmxMenuReadAction run(QmxMenuRead& read, const FakeRadio& radio, int* sent, int64_t* now)
{
    uint32_t mm_count = 0, refused = 0;
    std::string value;
    *sent = 0;
    for (int i = 0; i < 1000; ++i) {
        const QmxMenuReadAction a = qmx_menu_read_step(read, true, false, mm_count, value.c_str(), refused, *now);
        *now += 10;
        if (a == QmxMenuReadAction::Send) {
            char cmd[64];
            qmx_menu_read_command(read, cmd, sizeof(cmd));
            ++*sent;
            bool was_refused = false;
            const std::string v = radio.answer(cmd, &was_refused);
            if (was_refused) {
                ++refused;
            } else {
                value = v;
                ++mm_count;
            }
        } else if (a != QmxMenuReadAction::None) {
            return a;
        }
    }
    return QmxMenuReadAction::None;
}

static FakeRadio qmx_plus()
{
    FakeRadio r;
    const int bands[] = {160, 80, 60, 40, 30, 20, 17, 15, 12, 11, 10, 6};
    for (int c = 0; c < 12; ++c) {
        r.bands[c] = bands[c];
        r.flag[kQmxFlagTransmit][c] = "ENABLED";
    }
    r.band_version = "QMX+ 160-6m";
    return r;
}

int main()
{
    // The commands, as the probe sent them.
    {
        QmxMenuRead read;
        char cmd[64];
        qmx_menu_read_command(read, cmd, sizeof(cmd));
        expect(strcmp(cmd, "MMBand config.|Band name (m)[0];") == 0, "first: band name, column 0");
        read.stage = QmxMenuStage::BandFlag;
        read.flag = kQmxFlagTxPttGround;
        read.column = 3;
        qmx_menu_read_command(read, cmd, sizeof(cmd));
        expect(strcmp(cmd, "MMBand config.|TX PTT grounded[3];") == 0, "a flag: row name and column");
    }

    // The QMX+ as found: 12 bands, Transmit on all, no PTT output on any.
    {
        QmxMenuRead read;
        int sent = 0;
        int64_t now = 0;
        const QmxMenuReadAction a = run(read, qmx_plus(), &sent, &now);
        expect(a == QmxMenuReadAction::Done, "QMX+: read to the end");
        const QmxRadioSettings& r = read.result;
        expect(r.band_count == 12, "QMX+: 12 bands, stopping at the first 0");
        expect(r.bands[0].band_m == 160 && r.bands[9].band_m == 11 && r.bands[11].band_m == 6, "QMX+: band names");
        expect(r.bands[11].flag[kQmxFlagTransmit] == 1, "QMX+: 6 m Transmit on");
        expect(r.bands[0].flag[kQmxFlagTxPttGround] == 0, "QMX+: 160 m TX PTT grounded off");
        expect(strcmp(r.band_version, "QMX+ 160-6m") == 0, "QMX+: band version");
        expect(r.serial6_on_ptt == 0, "QMX+: Serial 6 on PTT off");
        expect(sent == 13 + 12 * kQmxFlagCount + 2, "QMX+: 13 names (the 13th reads 0), 5 flags a band, 2 items");
        expect(qmx_menu_read_step(read, true, false, 0, "", 0, now) == QmxMenuReadAction::None, "QMX+: then quiet");
    }

    // The mid-band QMX: six bands, TX PTT grounded on each.
    {
        FakeRadio radio;
        const int bands[] = {60, 40, 30, 20, 17, 15};
        for (int c = 0; c < 6; ++c) {
            radio.bands[c] = bands[c];
            radio.flag[kQmxFlagTransmit][c] = "ENABLED";
            radio.flag[kQmxFlagTxPttGround][c] = "ENABLED";
        }
        radio.band_version = "QMX 60-15m";
        QmxMenuRead read;
        int sent = 0;
        int64_t now = 0;
        expect(run(read, radio, &sent, &now) == QmxMenuReadAction::Done, "QMX: read to the end");
        expect(read.result.band_count == 6, "QMX: six bands");
        expect(read.result.bands[5].band_m == 15 && read.result.bands[5].flag[kQmxFlagTxPttGround] == 1,
               "QMX: 15 m TX PTT grounded on");
        expect(read.result.bands[5].flag[kQmxFlagRxPttGround] == 0, "QMX: 15 m RX PTT grounded off");
    }

    // Sixteen bands: the names stop at the table's end, not past it.
    {
        FakeRadio radio;
        for (int c = 0; c < kQmxMenuBandColumns; ++c) {
            radio.bands[c] = 100 + c;
        }
        QmxMenuRead read;
        int sent = 0;
        int64_t now = 0;
        expect(run(read, radio, &sent, &now) == QmxMenuReadAction::Done, "16 bands: read to the end");
        expect(read.result.band_count == kQmxMenuBandColumns, "16 bands: all of them");
        expect(sent == kQmxMenuBandColumns * (1 + kQmxFlagCount) + 2, "16 bands: no 17th name asked");
    }

    // A refused item reads as unknown, and the read goes on.
    {
        FakeRadio radio = qmx_plus();
        radio.refuse_serial6 = true;
        QmxMenuRead read;
        int sent = 0;
        int64_t now = 0;
        expect(run(read, radio, &sent, &now) == QmxMenuReadAction::Done, "refused: still done");
        expect(read.result.serial6_on_ptt == -1, "refused: Serial 6 unknown");
        expect(read.result.band_count == 12, "refused: the rest read");
    }

    // A value that is neither ENABLED nor DISABLED reads as unknown.
    expect(qmx_menu_flag_value("ENABLED") == 1 && qmx_menu_flag_value("DISABLED") == 0, "flag values");
    expect(qmx_menu_flag_value("YES") == -1 && qmx_menu_flag_value("") == -1, "an unknown flag value");
    expect(qmx_menu_band_value("160") == 160 && qmx_menu_band_value("0") == 0, "band values");
    expect(qmx_menu_band_value("4x") == 0 && qmx_menu_band_value("") == 0, "a band value that is not a number");

    // No reply in time ends the read.
    {
        QmxMenuRead read;
        expect(qmx_menu_read_step(read, true, false, 0, "", 0, 0) == QmxMenuReadAction::Send, "silent: sends");
        expect(qmx_menu_read_step(read, true, false, 0, "", 0, kQmxMenuReplyMs - 1) == QmxMenuReadAction::None,
               "silent: waits");
        expect(qmx_menu_read_step(read, true, false, 0, "", 0, kQmxMenuReplyMs) == QmxMenuReadAction::NoReply,
               "silent: gives up");
        expect(qmx_menu_read_step(read, true, false, 0, "", 0, kQmxMenuReplyMs + 5000) == QmxMenuReadAction::None,
               "silent: and stays quiet");
    }

    // A transmission pauses the read; the query in flight is asked again after.
    {
        QmxMenuRead read;
        expect(qmx_menu_read_step(read, true, true, 0, "", 0, 0) == QmxMenuReadAction::None, "busy: nothing sent");
        expect(qmx_menu_read_step(read, true, false, 0, "", 0, 10) == QmxMenuReadAction::Send, "then sent");
        expect(qmx_menu_read_step(read, true, true, 0, "", 0, 20) == QmxMenuReadAction::None, "busy mid-query");
        expect(qmx_menu_read_step(read, true, true, 0, "", 0, 5000) == QmxMenuReadAction::None,
               "a long transmission is no timeout");
        expect(qmx_menu_read_step(read, true, false, 0, "", 0, 5010) == QmxMenuReadAction::Send, "asked again");
        char cmd[64];
        qmx_menu_read_command(read, cmd, sizeof(cmd));
        expect(strcmp(cmd, "MMBand config.|Band name (m)[0];") == 0, "the same query");
    }

    // A link that drops forgets the radio: the next one is read afresh.
    {
        QmxMenuRead read;
        int sent = 0;
        int64_t now = 0;
        run(read, qmx_plus(), &sent, &now);
        expect(qmx_menu_read_step(read, false, false, 0, "", 0, now) == QmxMenuReadAction::None, "link drops");
        expect(read.result.band_count == 0, "drop: settings forgotten");
        expect(qmx_menu_read_step(read, true, false, 0, "", 0, now + 10) == QmxMenuReadAction::Send,
               "next link: read again");
    }

    if (g_fails) {
        fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    printf("qmx_menu_read: ok\n");
    return 0;
}
