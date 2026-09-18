#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "adif_delete.h"

// adif_delete.cpp also defines adif_delete_daily (storage-backed). Stub the
// FS so this host binary links without ESP-IDF storage_service.
bool storage_file_list(std::vector<std::string>&) { return false; }
bool storage_file_remove(const std::string&) { return false; }

static int g_fails = 0;

static void expect_true(bool cond, const char* msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    }
}

static void expect_str(const char* got, const char* want, const char* msg)
{
    const char* g = got ? got : "(null)";
    const char* w = want ? want : "(null)";
    if (std::strcmp(g, w) != 0) {
        fprintf(stderr, "FAIL: %s (got '%s' want '%s')\n", msg, g, w);
        ++g_fails;
    }
}

static bool lock_today(const char* basename)
{
    return basename && std::strcmp(basename, "20260918.adi") == 0;
}

static bool lock_none(const char*) { return false; }

int main()
{
    const std::vector<std::string> all = {
        "Station.txt", "20260917.adi", "20260918.adi", "RT260918.txt", "notes.txt",
    };
    std::vector<std::string> out;

    expect_str(adif_delete_plan("", all, lock_none, &out), nullptr, "clear-all plan ok");
    expect_true(out.size() == 2, "clear-all picks two daily files");
    expect_true(out[0] == "20260918.adi" || out[1] == "20260918.adi", "includes today");

    expect_str(adif_delete_plan("", all, lock_today, &out), nullptr,
               "clear-all skips locked today");
    expect_true(out.size() == 1 && out[0] == "20260917.adi", "only unlocked day");

    expect_str(adif_delete_plan("", {"20260918.adi"}, lock_today, &out),
               "active log protected", "clear-all all-locked NAKs");
    expect_true(out.empty(), "nothing to delete when all locked");

    expect_str(adif_delete_plan("20260917.adi", all, lock_today, &out), nullptr,
               "single unlocked ok");
    expect_true(out.size() == 1 && out[0] == "20260917.adi", "single file");

    expect_str(adif_delete_plan("20260918.adi", all, lock_today, &out),
               "active log protected", "single locked NAKs");
    expect_str(adif_delete_plan("Station.txt", all, lock_none, &out), "not a daily log",
               "station refused");
    expect_str(adif_delete_plan("../x.adi", all, lock_none, &out), "not a daily log",
               "path-ish name refused");
    expect_str(adif_delete_plan("19990101.adi", all, lock_none, &out), "not found",
               "missing daily");

    if (g_fails) {
        fprintf(stderr, "FAILED: %d\n", g_fails);
        return 1;
    }
    printf("PASS: adif_delete plan\n");
    return 0;
}
