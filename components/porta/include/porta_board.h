#pragma once

#include "driver/gpio.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Per-board seam for components/porta (docs/architecture/main_board.md (P4 (M5Stack Unit PoE-P4, U213)) / B67).
// PoE-P4: Grove G53/G54 (yellow = main RX, white = main TX).

void porta_board_uart_pins(gpio_num_t *tx, gpio_num_t *rx);

#ifdef __cplusplus
}
#endif
