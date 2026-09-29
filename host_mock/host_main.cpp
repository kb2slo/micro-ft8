/*
 * host_main.cpp — scenario runner.
 *
 *   ./host_test <test_file.json>
 *   ./host_test --dump-events <test_file.json>
 *
 * Each period's expect.tx / expect.log / expect.cabrillo is checked.
 * When <name>.events sits beside the JSON, the Port A event rows are
 * checked against it. --dump-events rewrites that file from this run.
 */

#include "scenario.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static std::string events_path_for(const std::string& json_path)
{
    const auto dot = json_path.rfind('.');
    if (dot == std::string::npos) {
        return json_path + ".events";
    }
    return json_path.substr(0, dot) + ".events";
}

static std::vector<std::string> read_lines(const std::string& path, bool* found)
{
    std::ifstream in(path);
    *found = in.is_open();
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

static int check_events(const Scenario& s, const std::string& path)
{
    bool found = false;
    const std::vector<std::string> want = read_lines(path, &found);
    if (!found) {
        return 0;
    }
    if (want.size() != s.events.size()) {
        fprintf(stderr, "events: expected %d rows, got %d (%s)\n",
                (int)want.size(), (int)s.events.size(), path.c_str());
        return 1;
    }
    for (size_t i = 0; i < want.size(); ++i) {
        if (want[i] != s.events[i]) {
            fprintf(stderr, "events row %d:\n  expected %s\n  got      %s\n",
                    (int)i, want[i].c_str(), s.events[i].c_str());
            return 1;
        }
    }
    return 0;
}

static void write_events(const Scenario& s, const std::string& path)
{
    std::ofstream out(path);
    if (!out) {
        fprintf(stderr, "cannot write %s\n", path.c_str());
        return;
    }
    for (const auto& row : s.events) {
        out << row << "\n";
    }
    printf("wrote %d event rows to %s\n", (int)s.events.size(), path.c_str());
}

int main(int argc, char* argv[])
{
    const char* test_file = nullptr;
    bool dump = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-events") == 0) {
            dump = true;
        } else if (!test_file) {
            test_file = argv[i];
        }
    }
    if (!test_file) {
        test_file = "test_qso.json";
    }

    TestData td;
    if (!load_test_data(test_file, td)) {
        fprintf(stderr, "Failed to load test data from '%s'\n", test_file);
        return 1;
    }

    Scenario s;
    scenario_begin(s, td);
    while (scenario_step(s)) {
    }
    printf("\n========== Test complete ==========\n");

    const std::string path = events_path_for(test_file);
    int events_fail = 0;
    if (dump) {
        write_events(s, path);
    } else {
        events_fail = check_events(s, path);
    }
    if (s.failures || events_fail) {
        fprintf(stderr, "FAILED: %d exchange mismatch(es)%s\n",
                s.failures, events_fail ? ", events differ" : "");
        return 1;
    }
    return 0;
}
