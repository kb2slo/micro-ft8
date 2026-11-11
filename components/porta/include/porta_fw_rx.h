#pragma once

// porta_fw_rx.h -- a main board's side of a firmware update over Port A
// (docs/architecture/main_board.md (Firmware updates)): what to answer each
// FW_BEGIN, FW_CHUNK and FW_END (porta_proto.h, Firmware update), and when
// the image may be made the one the board boots. No ESP-IDF: the flash is
// behind porta_fw_ops_t, which porta.cpp fills with the OTA calls, so any
// main board shares this and host_mock tests it (host_test_porta_fw_rx).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "porta_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Size of the app slot an image is written to; 0 when the board has none
    // to spare (one app partition: it needs its partition table flashed over USB).
    uint32_t slot_bytes;
    // Opens the slot for an image of `size` bytes, dropping whatever was there.
    bool (*begin)(void *ctx, uint32_t size);
    // Writes the next bytes. Offsets arrive in order with no gaps.
    bool (*write)(void *ctx, uint32_t offset, const uint8_t *data, size_t len);
    // SHA-256 of the first `size` bytes now in the slot, read back from it.
    bool (*hash)(void *ctx, uint32_t size, uint8_t out[PORTA_FW_SHA256_LEN]);
    // Closes the image. True if it is an app image this board can run, with
    // the version built into it in `version_out`.
    bool (*finish)(void *ctx, char version_out[PORTA_FW_VERSION_LEN + 1]);
    // Makes it the image the next restart boots.
    bool (*commit)(void *ctx);
    // Gives up an open image. Not called once finish() has run.
    void (*abort)(void *ctx);
} porta_fw_ops_t;

typedef struct {
    const porta_fw_ops_t *ops;
    void                 *ctx;
    bool                  active;     // a transfer is open
    bool                  committed;  // the new image boots next: a restart is due
    porta_fw_begin_t      begin;
    uint32_t              written;
} porta_fw_rx_t;

void porta_fw_rx_init(porta_fw_rx_t *rx, const porta_fw_ops_t *ops, void *ctx);

// Each returns the reply to send.
porta_fw_reply_t porta_fw_rx_begin(porta_fw_rx_t *rx, const porta_fw_begin_t *begin);
porta_fw_reply_t porta_fw_rx_chunk(porta_fw_rx_t *rx, uint32_t offset, const uint8_t *data, size_t len);
porta_fw_reply_t porta_fw_rx_end(porta_fw_rx_t *rx, bool abort);

// FW_END was answered OK: restart, after the reply has gone out.
bool porta_fw_rx_committed(const porta_fw_rx_t *rx);

// An image installed this way boots on trial: the bootloader goes back to the
// one before it at the next restart unless it is marked valid. It is kept
// once the sidekick's HELLO has arrived, which is what the next update needs;
// with none by the deadline the board restarts itself, which is the roll back.
#define PORTA_FW_CONFIRM_TIMEOUT_S 120u

typedef enum {
    PORTA_FW_CONFIRM_WAIT = 0,
    PORTA_FW_CONFIRM_KEEP,
    PORTA_FW_CONFIRM_ROLL_BACK,
} porta_fw_confirm_t;

porta_fw_confirm_t porta_fw_confirm_decide(uint32_t uptime_s, bool sidekick_hello_seen);

#ifdef __cplusplus
}
#endif
