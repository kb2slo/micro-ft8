#include "external_rtc.h"

// This board has no DS3231 (or any external RTC chip) -- not a TODO, a
// permanent fact of the hardware (docs/architecture/main_board.md (P4 (M5Stack Unit PoE-P4, U213))).
// rtc_core.h's rtc_write_external_from_soft() already checks
// external_rtc_available() before touching this, so "always unavailable" is
// the same code path ADV takes when it has the chip but hasn't found it on
// the bus yet -- not a new state.

esp_err_t external_rtc_init(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

bool external_rtc_available(void) {
    return false;
}

esp_err_t external_rtc_read_datetime(external_rtc_datetime_t * /*out_datetime*/) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t external_rtc_write_datetime(const external_rtc_datetime_t * /*datetime*/) {
    return ESP_ERR_NOT_SUPPORTED;
}
