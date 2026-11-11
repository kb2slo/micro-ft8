// A main board's side of a firmware update over Port A. See porta_fw_rx.h.

#include "porta_fw_rx.h"

#include <string.h>

static porta_fw_reply_t reply(uint8_t status, uint32_t offset)
{
    const porta_fw_reply_t r = {status, offset};
    return r;
}

void porta_fw_rx_init(porta_fw_rx_t *rx, const porta_fw_ops_t *ops, void *ctx)
{
    memset(rx, 0, sizeof(*rx));
    rx->ops = ops;
    rx->ctx = ctx;
}

static bool same_image(const porta_fw_begin_t *a, const porta_fw_begin_t *b)
{
    return a->size == b->size && memcmp(a->sha256, b->sha256, PORTA_FW_SHA256_LEN) == 0 &&
           strncmp(a->version, b->version, PORTA_FW_VERSION_LEN) == 0;
}

// Gives up the open transfer after a fault that leaves the slot's contents wrong.
static void drop(porta_fw_rx_t *rx)
{
    if (rx->active) {
        rx->ops->abort(rx->ctx);
    }
    rx->active = false;
    rx->written = 0;
}

porta_fw_reply_t porta_fw_rx_begin(porta_fw_rx_t *rx, const porta_fw_begin_t *begin)
{
    if (rx->committed) {
        // Already installed and about to restart: nothing more to send, and no other image is taken.
        return reply(same_image(&rx->begin, begin) ? PORTA_FW_OK : PORTA_FW_NO_TRANSFER, rx->begin.size);
    }
    if (rx->ops->slot_bytes == 0) {
        return reply(PORTA_FW_NO_SLOT, 0);
    }
    if (begin->size == 0 || begin->size > rx->ops->slot_bytes) {
        return reply(PORTA_FW_TOO_BIG, 0);
    }
    // The same image again: the sender lost its place, so tell it ours.
    if (rx->active && same_image(&rx->begin, begin)) {
        return reply(PORTA_FW_OK, rx->written);
    }
    drop(rx);
    if (!rx->ops->begin(rx->ctx, begin->size)) {
        return reply(PORTA_FW_FLASH, 0);
    }
    rx->begin = *begin;
    rx->begin.version[PORTA_FW_VERSION_LEN] = '\0';
    rx->active = true;
    rx->written = 0;
    return reply(PORTA_FW_OK, 0);
}

porta_fw_reply_t porta_fw_rx_chunk(porta_fw_rx_t *rx, uint32_t offset, const uint8_t *data, size_t len)
{
    if (!rx->active) {
        return reply(PORTA_FW_NO_TRANSFER, 0);
    }
    // A chunk again (its reply was lost), or one ahead of a chunk that was:
    // either way the sender is told where to go on from, and nothing is written.
    if (offset != rx->written || len == 0) {
        return reply(PORTA_FW_OFFSET, rx->written);
    }
    if (len > rx->begin.size - rx->written) {
        return reply(PORTA_FW_TOO_BIG, rx->written);
    }
    if (!rx->ops->write(rx->ctx, offset, data, len)) {
        drop(rx);
        return reply(PORTA_FW_FLASH, 0);
    }
    rx->written += (uint32_t)len;
    return reply(PORTA_FW_OK, rx->written);
}

porta_fw_reply_t porta_fw_rx_end(porta_fw_rx_t *rx, bool abort)
{
    if (rx->committed) {
        return reply(abort ? PORTA_FW_NO_TRANSFER : PORTA_FW_OK, rx->begin.size);
    }
    if (!rx->active) {
        return reply(abort ? PORTA_FW_OK : PORTA_FW_NO_TRANSFER, 0);
    }
    if (abort) {
        drop(rx);
        return reply(PORTA_FW_OK, 0);
    }
    if (rx->written != rx->begin.size) {
        // Still open: the sender can go on from here.
        return reply(PORTA_FW_SHORT, rx->written);
    }
    const uint32_t size = rx->begin.size;
    uint8_t digest[PORTA_FW_SHA256_LEN];
    if (!rx->ops->hash(rx->ctx, size, digest)) {
        drop(rx);
        return reply(PORTA_FW_FLASH, 0);
    }
    if (memcmp(digest, rx->begin.sha256, PORTA_FW_SHA256_LEN) != 0) {
        drop(rx);
        return reply(PORTA_FW_HASH, 0);
    }
    // finish() closes the image whatever it answers, so there is nothing to abort after it.
    char version[PORTA_FW_VERSION_LEN + 1] = {0};
    const bool image_ok = rx->ops->finish(rx->ctx, version);
    rx->active = false;
    rx->written = 0;
    if (!image_ok) {
        return reply(PORTA_FW_IMAGE, 0);
    }
    if (strncmp(version, rx->begin.version, PORTA_FW_VERSION_LEN) != 0) {
        return reply(PORTA_FW_VERSION, 0);
    }
    if (!rx->ops->commit(rx->ctx)) {
        return reply(PORTA_FW_FLASH, 0);
    }
    rx->committed = true;
    return reply(PORTA_FW_OK, size);
}

bool porta_fw_rx_committed(const porta_fw_rx_t *rx)
{
    return rx->committed;
}

porta_fw_confirm_t porta_fw_confirm_decide(uint32_t uptime_s, bool sidekick_hello_seen)
{
    if (sidekick_hello_seen) {
        return PORTA_FW_CONFIRM_KEEP;
    }
    return uptime_s >= PORTA_FW_CONFIRM_TIMEOUT_S ? PORTA_FW_CONFIRM_ROLL_BACK : PORTA_FW_CONFIRM_WAIT;
}
