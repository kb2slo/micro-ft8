// Device side of the decode benchmark (docs/architecture/decode.md). Decodes the embedded corpus slots under every
// configuration in bench_configs.h and prints one RESULT line per (config, slot) and one MSG line per decode.
// Output format is shared with host_main.c, so the two logs diff directly for the host/device parity check.

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "bench_configs.h"
#include "bench_core.h"

extern const uint8_t slot_0_raw_start[] asm("_binary_slot_0_raw_start");
extern const uint8_t slot_0_raw_end[] asm("_binary_slot_0_raw_end");
extern const uint8_t slot_1_raw_start[] asm("_binary_slot_1_raw_start");
extern const uint8_t slot_1_raw_end[] asm("_binary_slot_1_raw_end");
extern const uint8_t slot_2_raw_start[] asm("_binary_slot_2_raw_start");
extern const uint8_t slot_2_raw_end[] asm("_binary_slot_2_raw_end");
extern const uint8_t slot_3_raw_start[] asm("_binary_slot_3_raw_start");
extern const uint8_t slot_3_raw_end[] asm("_binary_slot_3_raw_end");
extern const uint8_t slot_4_raw_start[] asm("_binary_slot_4_raw_start");
extern const uint8_t slot_4_raw_end[] asm("_binary_slot_4_raw_end");
extern const char slots_txt_start[] asm("_binary_slots_txt_start");

typedef struct {
    const uint8_t* start;
    const uint8_t* end;
} EmbeddedSlot;

static const EmbeddedSlot s_slots[] = {
    { slot_0_raw_start, slot_0_raw_end },
    { slot_1_raw_start, slot_1_raw_end },
    { slot_2_raw_start, slot_2_raw_end },
    { slot_3_raw_start, slot_3_raw_end },
    { slot_4_raw_start, slot_4_raw_end },
};

typedef struct {
    const char* config;
    int slot;
} MessageContext;

static int64_t bench_clock_us(void)
{
    return esp_timer_get_time();
}

static void print_message(void* context, const char* text, float freq_hz, float time_s, int snr)
{
    (void)snr;
    const MessageContext* where = (const MessageContext*)context;
    printf("MSG %s %d %4.0f %+5.2f %s\n", where->config, where->slot, freq_hz, time_s, text);
}

static const char* optimization_name(void)
{
#if defined(CONFIG_COMPILER_OPTIMIZATION_PERF)
    return "-O2";
#elif defined(CONFIG_COMPILER_OPTIMIZATION_SIZE)
    return "-Os";
#elif defined(CONFIG_COMPILER_OPTIMIZATION_NONE)
    return "-O0";
#else
    return "-Og";
#endif
}

static void bench_task(void* argument)
{
    (void)argument;
    // Give the host time to reopen the USB-Serial-JTAG port after the reset, so the header is not lost.
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_chip_info_t chip;
    esp_chip_info(&chip);
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
    const char* wf_place = "psram";
    const unsigned free_spiram = (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#else
    const char* wf_place = "internal";
    const unsigned free_spiram = 0;
#endif
    printf("BENCH target=%s rev=%d.%d cores=%d cpu_mhz=%d opt=%s wf=%s free_internal=%u free_spiram=%u\n",
           CONFIG_IDF_TARGET, chip.revision / 100, chip.revision % 100, chip.cores,
           CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, optimization_name(), wf_place,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), free_spiram);
    printf("SLOTS\n%s\n", slots_txt_start);

    const int slot_count = (int)(sizeof(s_slots) / sizeof(s_slots[0]));
    for (int c = 0; c < BENCH_CONFIG_COUNT; ++c) {
        const BenchConfig* config = &k_bench_configs[c];
        for (int s = 0; s < slot_count; ++s) {
            const int16_t* samples = (const int16_t*)s_slots[s].start;
            size_t count = (size_t)(s_slots[s].end - s_slots[s].start) / sizeof(int16_t);
            MessageContext where = { config->name, s };
            BenchResult result;
            bench_run_slot(config, samples, count, bench_clock_us, print_message, &where, &result);
            printf("RESULT %s %d ok=%d cand=%d dec=%d find_ms=%.1f decode_ms=%.1f total_ms=%.1f process_ms=%.1f\n",
                   config->name, s, result.ok, result.candidates, result.decoded,
                   result.find_us / 1000.0, result.decode_us / 1000.0,
                   (result.find_us + result.decode_us) / 1000.0, result.process_us / 1000.0);
            fflush(stdout);
            vTaskDelay(1);
        }
    }
    printf("BENCH done\n");
    vTaskDelete(NULL);
}

void app_main(void)
{
    // Large stack: 256 candidates plus LDPC scratch; core 0 so the numbers do not depend on scheduling.
    xTaskCreatePinnedToCore(bench_task, "bench", 32768, NULL, 5, NULL, 0);
}
