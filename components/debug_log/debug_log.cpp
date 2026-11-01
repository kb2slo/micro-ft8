#include "debug_log.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

static void (*s_relay)(const char* text) = nullptr;

void debug_log_set_relay(void (*relay)(const char* text)) {
  s_relay = relay;
}

void debug_log_line(const std::string& msg) {
  if (s_relay) s_relay(msg.c_str());
}

void debug_log_line_public(const std::string& msg) {
  debug_log_line(msg);
}

// Internal RAM and PSRAM apart: with CONFIG_SPIRAM_USE_CAPS_ALLOC, MALLOC_CAP_8BIT
// spans both, and a figure that mixes them cannot show a leak in either.
void log_heap(const char* tag) {
  ESP_LOGI(tag, "HEAP: int free=%u min=%u largest=%u psram free=%u largest=%u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}
