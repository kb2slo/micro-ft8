#include <cstdio>
#include <string>
#include <vector>

#include "file_list.h"

static int g_fails = 0;

static void fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    ++g_fails;
}

static void expect_true(bool cond, const char* msg)
{
    if (!cond) {
        fail(msg);
    }
}

static void expect_int(int got, int want, const char* msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %d want %d)\n", msg, got, want);
        ++g_fails;
    }
}

static void expect_str(const std::string& got, const char* want, const char* msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got '%s' want '%s')\n", msg, got.c_str(), want);
        ++g_fails;
    }
}

int main()
{
    {
        expect_str(file_list_fail_text(FileListFail::Busy), "Storage busy", "busy text");
        expect_str(file_list_fail_text(FileListFail::Unavailable), "Storage unavailable",
                   "unavailable text");
        expect_str(file_list_fail_text(FileListFail::Failed), "List failed", "failed text");
    }

    {
        std::vector<std::string> files;
        std::vector<std::string> lines;
        file_list_apply(FileListKind::QsoDaily, FileListFail::Busy, {"20260819.adi"}, &files,
                        &lines);
        expect_true(files.empty(), "qso fail no files");
        expect_int(static_cast<int>(lines.size()), 1, "qso fail one line");
        expect_str(lines[0], "Storage busy", "qso busy line");
    }

    {
        std::vector<std::string> files;
        std::vector<std::string> lines;
        file_list_apply(FileListKind::QsoDaily, FileListFail::None,
                        {"Station.txt", "20260818.adi", "notes.txt", "20260819.adi"}, &files,
                        &lines);
        expect_int(static_cast<int>(files.size()), 2, "two daily");
        expect_str(files[0], "20260819.adi", "newest first");
        expect_str(lines[0], "20260819.adi", "qso line is name");
    }

    {
        std::vector<std::string> files;
        std::vector<std::string> lines;
        file_list_apply(FileListKind::QsoDaily, FileListFail::None, {"Station.txt"}, &files,
                        &lines);
        expect_true(files.empty(), "no daily");
        expect_str(lines[0], "No YYYYMMDD.adi", "empty daily message");
    }

    {
        std::vector<std::string> files;
        std::vector<std::string> lines;
        file_list_apply(FileListKind::Delete, FileListFail::Busy, {"20260819.adi"}, &files,
                        &lines);
        expect_true(files.empty(), "delete fail no files");
        expect_str(lines[0], "No storage files", "delete fail hides reason");
    }

    {
        std::vector<std::string> files;
        std::vector<std::string> lines;
        file_list_apply(FileListKind::Delete, FileListFail::None,
                        {"a.adi", "Station.txt", "b.adi"}, &files, &lines);
        expect_int(static_cast<int>(files.size()), 2, "station dropped");
        expect_str(files[0], "b.adi", "delete newest first");
        expect_str(files[1], "a.adi", "delete older last");
        expect_str(lines[0], "DEL b.adi", "delete prefix");
        expect_str(lines[1], "DEL a.adi", "delete prefix 2");
    }

    if (g_fails != 0) {
        fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    printf("PASS: file list\n");
    return 0;
}
