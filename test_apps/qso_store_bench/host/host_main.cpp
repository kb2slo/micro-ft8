// Host side of the QSO store bench (README.md): the same run as the P4's, on this machine's heap and clock.
#include <chrono>
#include <cstdint>
#include <vector>

#include "bench_core.h"

static int64_t now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main()
{
    static std::vector<uint8_t> image(6u << 20);
    static std::vector<QsoStoreEntry> entries(20480);
    static std::vector<uint32_t> slots(65536);
    const BenchMemory memory{image.data(), image.size(), entries.data(), entries.size(), slots.data(), slots.size()};
    return bench_run(memory, now_us, "host") ? 0 : 1;
}
