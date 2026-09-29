#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "adif_logger.h"
#include "board_power.h"  // extern "C" linkage -- included so the stub below matches it

// adif_logger.cpp also calls board_power_writes_blocked(), storage_service_
// firmware_available() and storage_file_append() (ESP-IDF storage), and
// rtc_now_ms() (host_mocks.o, linked below) -- same stubbing pattern
// test_adif_delete.cpp already uses, so this host binary links without
// ESP-IDF. storage_file_append() records what it was asked to write so the
// test can inspect the actual record line.
static std::vector<std::string> g_appended;
bool board_power_writes_blocked() { return false; }
bool storage_service_firmware_available() { return true; }
bool storage_file_append(const std::string&, const std::string& content, const std::string&,
                         bool) {
    g_appended.push_back(content);
    return true;
}

static int g_fails = 0;

static void expect_true(bool cond, const char* msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    }
}

static void expect_contains(const std::string& haystack, const char* needle, const char* msg) {
    if (haystack.find(needle) == std::string::npos) {
        fprintf(stderr, "FAIL: %s (wanted '%s' in '%s')\n", msg, needle, haystack.c_str());
        ++g_fails;
    }
}

int main() {
    // adif_log_entry() is what replaces both boards' separate log_adif_entry()
    // (main/main.cpp, poe_p4/main/porta_host_stubs.cpp): it must expand the
    // "/Radio"/"/Grid" comment macros on *both* boards now -- previously the
    // P4 skipped this entirely (a confirmed drift, row j4xh).
    {
        g_appended.clear();
        AdifStationSnapshot station;
        station.call = "KB2SLO";
        station.my_gridsquare = "FN30";
        station.mode = "FT8";
        station.freq_mhz = "14.074";
        station.raw_comment = "TNX /Radio /Grid";
        station.radio_name = "QMX";
        station.grid_for_macro = "FN31";
        AdifLoggerDedupe dedupe;
        expect_true(adif_log_entry("K1ABC", "FN42", -8, -12, station, &dedupe),
                    "adif_log_entry succeeds");
        expect_true(g_appended.size() == 1, "one record appended");
        expect_contains(g_appended[0], "<comment:12>TNX QMX FN31 ", "both macros expanded");
    }

    // A comment with no macros passes through unchanged -- the substitution
    // is a no-op find, not something that corrupts ordinary text.
    {
        g_appended.clear();
        AdifStationSnapshot station;
        station.call = "KB2SLO";
        station.my_gridsquare = "FN30";
        station.mode = "FT8";
        station.freq_mhz = "14.074";
        station.raw_comment = "MicroFT8";
        station.radio_name = "QMX";
        station.grid_for_macro = "FN31";
        AdifLoggerDedupe dedupe;
        expect_true(adif_log_entry("N0CALL", "EM12", -5, -9, station, &dedupe),
                    "adif_log_entry succeeds with a plain comment");
        expect_contains(g_appended[0], "<comment:8>MicroFT8 ", "plain comment unchanged");
    }

    if (g_fails) {
        fprintf(stderr, "%d FAIL(s)\n", g_fails);
        return 1;
    }
    printf("PASS: adif_log_entry expands comment macros the same way on both boards\n");
    return 0;
}
