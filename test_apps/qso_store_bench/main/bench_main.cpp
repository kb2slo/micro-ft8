// Device side of the QSO store bench (README.md). The store's buffers are static in PSRAM, as the firmware's
// would be (docs/architecture/memory.md), and sized for the largest log the bench fills.
#include <cstdint>
#include <cstdio>

#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bench_core.h"

namespace {

constexpr size_t kImageBytes = 6u << 20;
constexpr size_t kEntries = 20480;
constexpr size_t kCallSlots = 65536;

EXT_RAM_BSS_ATTR uint8_t s_image[kImageBytes];
EXT_RAM_BSS_ATTR QsoStoreEntry s_entries[kEntries];
EXT_RAM_BSS_ATTR uint32_t s_call_slots[kCallSlots];

int64_t now_us() { return esp_timer_get_time(); }

}  // namespace

extern "C" void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1000));  // let the runner's port open before the first line
    const BenchMemory memory{s_image, kImageBytes, s_entries, kEntries, s_call_slots, kCallSlots};
    bench_run(memory, now_us, "p4");
    fflush(stdout);
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}
