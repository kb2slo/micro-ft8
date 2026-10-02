#include "porta_board.h"

// Unit PoE-P4 (U213) HY2.0-4P: yellow G53, white G54.
// AtomS3 Lite Grove is yellow=G2 / white=G1 (m5-docs). Sidekick TXes on G1
// and RXes on G2 (sidekick/main/main.c), so over a straight Grove cable:
//   white: sidekick TX (G1) -> host RX
//   yellow: sidekick RX (G2) <- host TX
// First P4 mapping had these inverted; no HELLO until this swap.
void porta_board_uart_pins(gpio_num_t *tx, gpio_num_t *rx) {
    if (tx) {
        *tx = GPIO_NUM_53;  // yellow
    }
    if (rx) {
        *rx = GPIO_NUM_54;  // white
    }
}
