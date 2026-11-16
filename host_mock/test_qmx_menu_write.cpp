// qmx_menu_write: setting the QMX's PTT outputs for an amplifier
// (components/radio_control/include/qmx_menu_write.h), against the two radios read on 2026-11-09.

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "qmx_menu_write.h"

static int g_fails = 0;

static void expect(bool ok, const char* msg)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    }
}

static QmxRadioSettings qmx_plus()
{
    QmxRadioSettings r;
    const int bands[] = {160, 80, 60, 40, 30, 20, 17, 15, 12, 11, 10, 6};
    r.band_count = 12;
    for (int c = 0; c < 12; ++c) {
        r.bands[c].band_m = bands[c];
        r.bands[c].flag[kQmxFlagTransmit] = 1;
        for (int f = kQmxFlagTxPtt5v; f < kQmxFlagCount; ++f) {
            r.bands[c].flag[f] = 0;
        }
    }
    r.serial6_on_ptt = 0;
    return r;
}

// A radio that stores each write and answers reads, optionally echoing a write first.
struct FakeRadio {
    std::map<std::string, std::string> cells;
    bool echo_write = false;
    bool refuse_writes = false;
    bool ignore_writes = false;
    const char* store_as = nullptr;  // stores this in place of what is written, as a radio clamping it would
    uint32_t mm_count = 0;
    uint32_t refused = 0;
    std::string last;

    void reply(const std::string& v)
    {
        last = v;
        ++mm_count;
    }

    // Handles "MM<path>=<value>;MM<path>;".
    void handle(const std::string& cmd)
    {
        const size_t eq = cmd.find('=');
        const size_t semi = cmd.find(';');
        const std::string path = cmd.substr(2, eq - 2);
        const std::string value = cmd.substr(eq + 1, semi - eq - 1);
        if (refuse_writes) {
            ++refused;
            return;
        }
        if (!ignore_writes) {
            cells[path] = store_as ? store_as : value;
        }
        if (echo_write) {
            reply(value);
        }
        reply(cells.count(path) ? cells[path] : "DISABLED");
    }
};

// Runs a planned write against `radio`. Returns the last action other than None and Send.
static QmxMenuWriteAction run(QmxMenuWrite& w, FakeRadio& radio, int* sent)
{
    int64_t now = 0;
    *sent = 0;
    for (int i = 0; i < 2000; ++i) {
        const QmxMenuWriteAction a =
            qmx_menu_write_step(w, true, false, radio.mm_count, radio.last.c_str(), radio.refused, now);
        now += 10;
        if (a == QmxMenuWriteAction::Send) {
            char cmd[128];
            qmx_menu_write_command(w, cmd, sizeof(cmd));
            ++*sent;
            radio.handle(cmd);
        } else if (a == QmxMenuWriteAction::Reload) {
            continue;
        } else if (a != QmxMenuWriteAction::None) {
            return a;
        }
    }
    return QmxMenuWriteAction::None;
}

int main()
{
    // The QMX+ as found, keyed grounded: TX PTT grounded on each of its 12 bands, nothing else.
    {
        QmxMenuWrite w;
        expect(qmx_menu_write_plan(qmx_plus(), QmxPttKeying::Ground, w) == 12, "QMX+: 12 cells to write");
        char cmd[128];
        qmx_menu_write_command(w, cmd, sizeof(cmd));
        expect(strcmp(cmd, "MMBand config.|TX PTT grounded[0]=ENABLED;MMBand config.|TX PTT grounded[0];") == 0,
               "the write and its read-back, in one send");
        FakeRadio radio;
        int sent = 0;
        expect(run(w, radio, &sent) == QmxMenuWriteAction::Done, "QMX+: written");
        expect(sent == 12, "QMX+: one send a cell");
        expect(radio.cells["Band config.|TX PTT grounded[11]"] == "ENABLED", "6 m grounded on");
        expect(radio.cells.count("Band config.|TX PTT +5V[0]") == 0, "a cell already right is not written");
    }

    // A radio that echoes a write before the read-back: the same.
    {
        QmxMenuWrite w;
        qmx_menu_write_plan(qmx_plus(), QmxPttKeying::Ground, w);
        FakeRadio radio;
        radio.echo_write = true;
        int sent = 0;
        expect(run(w, radio, &sent) == QmxMenuWriteAction::Done && sent == 12, "an echoed write: still done");
    }

    // The mid-band QMX, already right for grounded: nothing to write.
    {
        QmxRadioSettings r = qmx_plus();
        r.band_count = 6;
        for (int c = 0; c < 6; ++c) {
            r.bands[c].flag[kQmxFlagTxPttGround] = 1;
        }
        QmxMenuWrite w;
        expect(qmx_menu_write_plan(r, QmxPttKeying::Ground, w) == 0, "already right: no write");
        expect(qmx_menu_write_step(w, true, false, 0, "", 0, 0) == QmxMenuWriteAction::None, "and nothing sent");

        // The same radio for +5V: grounded off and +5V on, each band.
        expect(qmx_menu_write_plan(r, QmxPttKeying::Plus5v, w) == 12, "+5V: two cells a band");
        FakeRadio radio;
        int sent = 0;
        expect(run(w, radio, &sent) == QmxMenuWriteAction::Done, "+5V: written");
        expect(radio.cells["Band config.|TX PTT +5V[5]"] == "ENABLED" &&
                   radio.cells["Band config.|TX PTT grounded[5]"] == "DISABLED",
               "+5V: 15 m swapped");
    }

    // Transmit DISABLED: the operator's choice, left alone; an RX output on is turned off.
    {
        QmxRadioSettings r = qmx_plus();
        r.bands[2].flag[kQmxFlagTransmit] = 0;
        r.bands[3].flag[kQmxFlagRxPttGround] = 1;
        QmxMenuWrite w;
        expect(qmx_menu_write_plan(r, QmxPttKeying::Ground, w) == 12, "11 bands grounded, 40 m RX off");
        bool col2 = false, rx_off = false;
        for (int i = 0; i < w.count; ++i) {
            col2 = col2 || w.items[i].column == 2;
            rx_off = rx_off || (w.items[i].column == 3 && w.items[i].flag == kQmxFlagRxPttGround && !w.items[i].enable);
        }
        expect(!col2, "a band with Transmit DISABLED is not written");
        expect(rx_off, "RX PTT grounded turned off");
    }

    // Serial 6 on PTT on: turned off, after the bands.
    {
        QmxRadioSettings r = qmx_plus();
        r.serial6_on_ptt = 1;
        QmxMenuWrite w;
        expect(qmx_menu_write_plan(r, QmxPttKeying::Ground, w) == 13, "12 bands and Serial 6");
        w.next = 12;
        char cmd[128];
        qmx_menu_write_command(w, cmd, sizeof(cmd));
        expect(strcmp(cmd, "MMSystem config|GPS & Ser. ports|Serial 6 on PTT=DISABLED;"
                           "MMSystem config|GPS & Ser. ports|Serial 6 on PTT;") == 0,
               "Serial 6 off");
    }

    // An unread flag is written, so the radio ends as wanted.
    {
        QmxRadioSettings r = qmx_plus();
        r.band_count = 1;
        r.bands[0].flag[kQmxFlagRxPtt5v] = -1;
        QmxMenuWrite w;
        expect(qmx_menu_write_plan(r, QmxPttKeying::Ground, w) == 2, "unread RX +5V written DISABLED");
    }

    // A refused write stops it.
    {
        QmxMenuWrite w;
        qmx_menu_write_plan(qmx_plus(), QmxPttKeying::Ground, w);
        FakeRadio radio;
        radio.refuse_writes = true;
        int sent = 0;
        expect(run(w, radio, &sent) == QmxMenuWriteAction::Failed && w.refused && sent == 1, "refused: stops at once");
    }

    // A write that does not take (the read-back keeps the old value) stops it in time.
    {
        QmxMenuWrite w;
        qmx_menu_write_plan(qmx_plus(), QmxPttKeying::Ground, w);
        FakeRadio radio;
        radio.ignore_writes = true;
        int sent = 0;
        expect(run(w, radio, &sent) == QmxMenuWriteAction::Failed && !w.refused && sent == 1,
               "not taken: stops on the timeout");
    }

    // Busy pauses it; a dropped link fails it.
    {
        QmxMenuWrite w;
        qmx_menu_write_plan(qmx_plus(), QmxPttKeying::Ground, w);
        expect(qmx_menu_write_step(w, true, true, 0, "", 0, 0) == QmxMenuWriteAction::None, "busy: nothing sent");
        expect(qmx_menu_write_step(w, true, false, 0, "", 0, 10) == QmxMenuWriteAction::Send, "then sent");
        expect(qmx_menu_write_step(w, true, true, 0, "", 0, 20) == QmxMenuWriteAction::None, "busy mid-cell");
        expect(qmx_menu_write_step(w, true, false, 0, "", 0, 5000) == QmxMenuWriteAction::Send, "asked again");
        expect(qmx_menu_write_step(w, false, false, 0, "", 0, 5010) == QmxMenuWriteAction::Failed, "link drops");
        expect(qmx_menu_write_step(w, true, false, 0, "", 0, 5020) == QmxMenuWriteAction::None, "and stays done");
    }

    // A setting: one cell, its write and read-back, matched as a number.
    {
        QmxRadioSettings r = qmx_plus();
        snprintf(r.setting[0], sizeof(r.setting[0]), "%s", "50");
        QmxMenuWrite w;
        expect(qmx_menu_setting_plan(r, 0, "50.0", w) == 0, "setting: the radio holds it already, no write");
        expect(qmx_menu_setting_plan(r, 0, "40", w) == 1, "setting: one cell");
        char cmd[128];
        qmx_menu_write_command(w, cmd, sizeof(cmd));
        expect(strcmp(cmd, "MMProtection|Tune %=40;MMProtection|Tune %;") == 0, "setting: its write and read-back");
        FakeRadio radio;
        radio.store_as = "40.0";
        int sent = 0;
        expect(run(w, radio, &sent) == QmxMenuWriteAction::Done && sent == 1, "setting: 40.0 reads back as 40");
    }

    // Max. PA voltage, a decimal: 11.5 to 9, read back as the radio gives it.
    {
        QmxRadioSettings r = qmx_plus();
        snprintf(r.setting[1], sizeof(r.setting[1]), "%s", "11.5");
        QmxMenuWrite w;
        expect(qmx_menu_setting_plan(r, 1, "11.50", w) == 0, "PA voltage: 11.50 is what it holds");
        expect(qmx_menu_setting_plan(r, 1, "9", w) == 1, "PA voltage: one cell");
        char cmd[128];
        qmx_menu_write_command(w, cmd, sizeof(cmd));
        expect(strcmp(cmd, "MMProtection|Max. PA voltage=9;MMProtection|Max. PA voltage;") == 0,
               "PA voltage: its write and read-back");
        FakeRadio radio;
        radio.store_as = "9.0";
        int sent = 0;
        expect(run(w, radio, &sent) == QmxMenuWriteAction::Done, "PA voltage: 9.0 reads back as 9");
    }

    // A radio that holds something else after the write (clamped) fails it, as not read back.
    {
        QmxRadioSettings r = qmx_plus();
        QmxMenuWrite w;
        qmx_menu_setting_plan(r, 0, "250", w);
        FakeRadio radio;
        radio.store_as = "100";
        int sent = 0;
        expect(run(w, radio, &sent) == QmxMenuWriteAction::Failed && !w.refused, "setting clamped: not read back");
    }

    // A refused setting write fails, as refused.
    {
        QmxRadioSettings r = qmx_plus();
        QmxMenuWrite w;
        qmx_menu_setting_plan(r, 0, "30", w);
        FakeRadio radio;
        radio.refuse_writes = true;
        int sent = 0;
        expect(run(w, radio, &sent) == QmxMenuWriteAction::Failed && w.refused, "setting refused");
    }

    // Only a plain number goes to the radio.
    expect(qmx_setting_value_ok("40") && qmx_setting_value_ok("12.5"), "plain numbers");
    expect(!qmx_setting_value_ok("") && !qmx_setting_value_ok("-5") && !qmx_setting_value_ok("4;MU"),
           "empty, signed, or a command smuggled in");
    expect(!qmx_setting_value_ok("1.2.3") && !qmx_setting_value_ok(".5") && !qmx_setting_value_ok("5.") &&
               !qmx_setting_value_ok("12345678"),
           "two dots, a bare dot, too long");
    expect(qmx_setting_values_match("40", "40.0") && !qmx_setting_values_match("40", "41") &&
               !qmx_setting_values_match("", "40"),
           "values matched as numbers");

    if (g_fails) {
        fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    printf("qmx_menu_write: ok\n");
    return 0;
}
