#pragma once

#include "driver/gpio.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Per-board seam for components/porta (docs/architecture/main_board.md (P4 (M5Stack Unit PoE-P4, U213)) / B67).
// ADV: Grove G1/G2. PoE-P4: Grove G53/G54. Same HY2.0-4P cable roles
// (yellow = main RX, white = main TX) so the existing PORTA cable works.

void porta_board_uart_pins(gpio_num_t *tx, gpio_num_t *rx);

// ADV compares HELLO to the embedded sidekick image via sidekick_flasher.
// Boards that do not ship an embedded image return false (version mismatch
// is logged; the protocol still runs).
bool porta_board_embedded_sidekick_version(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
