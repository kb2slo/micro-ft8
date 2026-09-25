#include "board_power.h"

#include <cstring>

// This board has no battery/pack monitoring circuit -- ADV's GPIO10 divider
// is board-specific hardware, not a general ESP32 feature (docs/architecture/main_board.md (P4 (M5Stack Unit PoE-P4, U213))
// amendment, 2026-09-17). Permanently "no pack, never halted," not a TODO.

esp_err_t board_power_init(void) {
    return ESP_OK;
}

esp_err_t board_power_read(board_power_status_t *out_status) {
    if (!out_status) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_status = board_power_status_t{};
    return ESP_OK;
}

void board_power_format_charge_stripe(const board_power_status_t * /*status*/, char *buf, size_t buf_len) {
    if (buf && buf_len) {
        buf[0] = '\0';
    }
}

bool board_power_halted(void) {
    return false;
}

bool board_power_writes_blocked(void) {
    return false;
}
