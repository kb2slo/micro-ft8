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

void log_heap(const char* tag) {
  size_t free_sz = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  size_t min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  ESP_LOGI(tag, "HEAP: free=%u min=%u largest=%u", (unsigned)free_sz, (unsigned)min_free, (unsigned)largest);
}
