// Host side of the QSO store bench (README.md): the same run as the P4's, on this machine's heap and clock.
// With no arguments, the synthetic logs; given ADIF files, each of those instead.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#include "bench_core.h"

static int64_t now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv)
{
    static std::vector<uint8_t> image(6u << 20);
    static std::vector<QsoStoreEntry> entries(20480);
    static std::vector<uint32_t> slots(65536);
    const BenchMemory memory{image.data(), image.size(), entries.data(), entries.size(), slots.data(), slots.size()};
    if (argc < 2) return bench_run(memory, now_us, "host") ? 0 : 1;
    bool ok = true;
    for (int i = 1; i < argc; ++i) {
        std::ifstream in(argv[i], std::ios::binary);
        if (!in) {
            fprintf(stderr, "cannot read %s\n", argv[i]);
            return 2;
        }
        std::stringstream text;
        text << in.rdbuf();
        const std::string s = text.str();
        ok = bench_run_log(memory, now_us, "host", s.data(), s.size()) && ok;
    }
    return ok ? 0 : 1;
}
