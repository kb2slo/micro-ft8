#include "porta_board.h"

#include "sidekick_flasher.h"

// Cardputer ADV PORTA Grove: yellow G1 = RX, white G2 = TX (existing cable).
void porta_board_uart_pins(gpio_num_t *tx, gpio_num_t *rx) {
    if (tx) {
        *tx = GPIO_NUM_2;
    }
    if (rx) {
        *rx = GPIO_NUM_1;
    }
}

bool porta_board_embedded_sidekick_version(char *out, size_t out_size) {
    return sidekick_flasher_embedded_version(out, out_size);
}
