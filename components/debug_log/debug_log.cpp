#include "debug_log.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

std::vector<std::string> g_debug_lines;
int debug_page = 0;
const size_t DEBUG_MAX_LINES = 18;
const size_t DEBUG_HUD_LINES = 2;

static void (*s_relay)(const char* text) = nullptr;

void debug_log_set_relay(void (*relay)(const char* text)) {
  s_relay = relay;
}

void debug_ensure_hud_lines() {
  while (g_debug_lines.size() < DEBUG_HUD_LINES) {
    g_debug_lines.emplace_back();
  }
}

void debug_log_line(const std::string& msg) {
  if (s_relay) s_relay(msg.c_str());

  debug_ensure_hud_lines();
  if (g_debug_lines.size() >= DEBUG_MAX_LINES) {
    if (g_debug_lines.size() > DEBUG_HUD_LINES) {
      g_debug_lines.erase(g_debug_lines.begin() + DEBUG_HUD_LINES);
    } else {
      return;
    }
  }
  g_debug_lines.push_back(msg);
  debug_page = (int)((g_debug_lines.size() - 1) / 6);
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
