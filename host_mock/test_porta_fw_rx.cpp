// Host test: a main board's side of a firmware update over Port A
// (components/porta/porta_fw_rx.c; docs/architecture/main_board.md (Firmware updates)).
// The flash is a vector here, with switches to make each call fail.

#include "porta_fw_rx.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;

static void check(bool ok, const char *what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        g_fail++;
    }
}

struct Slot {
    std::vector<uint8_t> bytes;
    bool open = false;
    int begins = 0, aborts = 0, commits = 0, finishes = 0, writes = 0;
    bool fail_begin = false, fail_write = false, fail_hash = false, fail_commit = false;
    bool image_ok = true;
    std::string image_version = "d18f9b2";
};

// Not SHA-256: any function of the bytes does, since the receiver only compares.
static void fake_hash(const uint8_t *p, size_t n, uint8_t out[32])
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    for (int i = 0; i < 32; ++i) out[i] = (uint8_t)(h >> ((i % 4) * 8)) ^ (uint8_t)i;
}

static bool op_begin(void *ctx, uint32_t size)
{
    Slot *s = (Slot *)ctx;
    s->begins++;
    if (s->fail_begin) return false;
    s->bytes.assign(size, 0xFF);
    s->open = true;
    return true;
}
static bool op_write(void *ctx, uint32_t offset, const uint8_t *data, size_t len)
{
    Slot *s = (Slot *)ctx;
    s->writes++;
    if (s->fail_write || !s->open || offset + len > s->bytes.size()) return false;
    std::memcpy(&s->bytes[offset], data, len);
    return true;
}
static bool op_hash(void *ctx, uint32_t size, uint8_t out[32])
{
    Slot *s = (Slot *)ctx;
    if (s->fail_hash || size > s->bytes.size()) return false;
    fake_hash(s->bytes.data(), size, out);
    return true;
}
static bool op_finish(void *ctx, char version_out[PORTA_FW_VERSION_LEN + 1])
{
    Slot *s = (Slot *)ctx;
    s->finishes++;
    s->open = false;
    std::snprintf(version_out, PORTA_FW_VERSION_LEN + 1, "%s", s->image_version.c_str());
    return s->image_ok;
}
static bool op_commit(void *ctx)
{
    Slot *s = (Slot *)ctx;
    s->commits++;
    return !s->fail_commit;
}
static void op_abort(void *ctx)
{
    Slot *s = (Slot *)ctx;
    s->aborts++;
    s->open = false;
}

static porta_fw_ops_t ops(uint32_t slot_bytes)
{
    return {slot_bytes, op_begin, op_write, op_hash, op_finish, op_commit, op_abort};
}

static std::vector<uint8_t> image(size_t n, uint8_t seed = 1)
{
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)(i * 31 + seed);
    return v;
}

static porta_fw_begin_t begin_for(const std::vector<uint8_t> &img, const char *version = "d18f9b2")
{
    porta_fw_begin_t b = {};
    b.size = (uint32_t)img.size();
    fake_hash(img.data(), img.size(), b.sha256);
    std::snprintf(b.version, sizeof(b.version), "%s", version);
    return b;
}

static bool is(porta_fw_reply_t r, uint8_t status, uint32_t offset)
{
    return r.status == status && r.offset == offset;
}

// Sends img[from..to) in chunks of `step`; true if every reply was OK with the running offset.
static bool send(porta_fw_rx_t *rx, const std::vector<uint8_t> &img, size_t from, size_t to, size_t step = PORTA_FW_CHUNK_MAX)
{
    for (size_t at = from; at < to;) {
        const size_t n = std::min(step, to - at);
        if (!is(porta_fw_rx_chunk(rx, (uint32_t)at, &img[at], n), PORTA_FW_OK, (uint32_t)(at + n))) return false;
        at += n;
    }
    return true;
}

static void test_whole_transfer()
{
    Slot slot;
    const porta_fw_ops_t o = ops(4096);
    porta_fw_rx_t rx;
    porta_fw_rx_init(&rx, &o, &slot);
    const auto img = image(1000);  // not a multiple of the chunk size
    const auto b = begin_for(img);

    check(is(porta_fw_rx_begin(&rx, &b, true), PORTA_FW_OK, 0), "begin: send from 0");
    check(send(&rx, img, 0, img.size()), "every chunk acknowledged with the next offset");
    check(!porta_fw_rx_committed(&rx), "not committed before END");
    check(is(porta_fw_rx_end(&rx, false), PORTA_FW_OK, 1000), "end: OK");
    check(porta_fw_rx_committed(&rx) && slot.commits == 1 && slot.finishes == 1 && slot.aborts == 0, "committed once, nothing aborted");
    check(slot.bytes == img, "the slot holds the image");
    // The reply to END was lost: asked again, the answer is the same and nothing is redone.
    check(is(porta_fw_rx_end(&rx, false), PORTA_FW_OK, 1000) && slot.commits == 1, "end again");
    check(is(porta_fw_rx_begin(&rx, &b, true), PORTA_FW_OK, 1000) && slot.begins == 1, "begin again after commit: nothing to send");
    check(is(porta_fw_rx_chunk(&rx, 0, img.data(), 10), PORTA_FW_NO_TRANSFER, 0), "no chunk after commit");
}

static void test_refusals_before_a_transfer()
{
    Slot slot;
    porta_fw_ops_t o = ops(4096);
    porta_fw_rx_t rx;
    porta_fw_rx_init(&rx, &o, &slot);
    const auto img = image(500);
    auto b = begin_for(img);

    check(is(porta_fw_rx_begin(&rx, &b, false), PORTA_FW_NOT_UPDATE_MODE, 0) && slot.begins == 0, "not outside update mode");
    check(is(porta_fw_rx_chunk(&rx, 0, img.data(), 10), PORTA_FW_NO_TRANSFER, 0), "chunk with no begin");
    check(is(porta_fw_rx_end(&rx, false), PORTA_FW_NO_TRANSFER, 0), "end with no begin");
    check(is(porta_fw_rx_end(&rx, true), PORTA_FW_OK, 0), "aborting nothing is fine");

    b.size = 4097;
    check(is(porta_fw_rx_begin(&rx, &b, true), PORTA_FW_TOO_BIG, 0), "one byte over the slot");
    b.size = 0;
    check(is(porta_fw_rx_begin(&rx, &b, true), PORTA_FW_TOO_BIG, 0), "an empty image");
    b.size = 4096;
    check(is(porta_fw_rx_begin(&rx, &b, true), PORTA_FW_OK, 0), "exactly the slot");

    Slot none;
    const porta_fw_ops_t single = ops(0);
    porta_fw_rx_init(&rx, &single, &none);
    b = begin_for(img);
    check(is(porta_fw_rx_begin(&rx, &b, true), PORTA_FW_NO_SLOT, 0) && none.begins == 0, "a board with one app partition");

    Slot bad;
    bad.fail_begin = true;
    porta_fw_rx_init(&rx, &o, &bad);
    check(is(porta_fw_rx_begin(&rx, &b, true), PORTA_FW_FLASH, 0), "the slot would not open");
    check(is(porta_fw_rx_chunk(&rx, 0, img.data(), 10), PORTA_FW_NO_TRANSFER, 0), "and no transfer is open after that");
}

static void test_lost_frames_and_resume()
{
    Slot slot;
    const porta_fw_ops_t o = ops(4096);
    porta_fw_rx_t rx;
    porta_fw_rx_init(&rx, &o, &slot);
    const auto img = image(1000);
    const auto b = begin_for(img);
    porta_fw_rx_begin(&rx, &b, true);
    check(send(&rx, img, 0, 480), "the first two chunks");

    // The reply to the second chunk was lost, so it comes again: not written twice.
    const int writes = slot.writes;
    check(is(porta_fw_rx_chunk(&rx, 240, &img[240], 240), PORTA_FW_OFFSET, 480) && slot.writes == writes, "a repeated chunk");
    // A chunk was lost, so the next one is ahead.
    check(is(porta_fw_rx_chunk(&rx, 720, &img[720], 240), PORTA_FW_OFFSET, 480) && slot.writes == writes, "a chunk ahead");
    check(is(porta_fw_rx_chunk(&rx, 480, &img[480], 0), PORTA_FW_OFFSET, 480), "an empty chunk");
    // More than the image holds.
    check(send(&rx, img, 480, 960), "on to 960");
    check(is(porta_fw_rx_chunk(&rx, 960, &img[0], 41), PORTA_FW_TOO_BIG, 960) && slot.writes == writes + 2, "a chunk past the end");
    check(is(porta_fw_rx_end(&rx, false), PORTA_FW_SHORT, 960), "end before the last bytes: still open");

    // The sidekick restarted and begins the same image again: go on from here.
    check(is(porta_fw_rx_begin(&rx, &b, true), PORTA_FW_OK, 960) && slot.begins == 1 && slot.aborts == 0, "same image resumes");
    check(send(&rx, img, 960, 1000), "the rest");
    check(is(porta_fw_rx_end(&rx, false), PORTA_FW_OK, 1000) && slot.bytes == img, "resumed transfer installs");
}

static void test_a_different_image_starts_over()
{
    Slot slot;
    const porta_fw_ops_t o = ops(4096);
    porta_fw_rx_t rx;
    porta_fw_rx_init(&rx, &o, &slot);
    const auto first = image(1000), second = image(1000, 9);
    const auto b1 = begin_for(first);
    porta_fw_rx_begin(&rx, &b1, true);
    send(&rx, first, 0, 480);

    const auto b2 = begin_for(second);  // same size, another hash
    check(is(porta_fw_rx_begin(&rx, &b2, true), PORTA_FW_OK, 0) && slot.aborts == 1 && slot.begins == 2, "another hash: from 0");
    auto b3 = b2;
    std::snprintf(b3.version, sizeof(b3.version), "7e61488");
    send(&rx, second, 0, 240);
    check(is(porta_fw_rx_begin(&rx, &b3, true), PORTA_FW_OK, 0) && slot.begins == 3, "another version: from 0");
    auto b4 = begin_for(image(999, 9));
    check(is(porta_fw_rx_begin(&rx, &b4, true), PORTA_FW_OK, 0) && slot.begins == 4, "another size: from 0");

    // Update mode was left with a transfer open.
    porta_fw_rx_reset(&rx);
    check(slot.aborts == 4 && is(porta_fw_rx_chunk(&rx, 0, second.data(), 10), PORTA_FW_NO_TRANSFER, 0), "reset forgets it");
    porta_fw_rx_reset(&rx);
    check(slot.aborts == 4, "reset with nothing open aborts nothing");

    // An explicit abort.
    porta_fw_rx_begin(&rx, &b1, true);
    send(&rx, first, 0, 240);
    check(is(porta_fw_rx_end(&rx, true), PORTA_FW_OK, 0) && slot.aborts == 5, "abort");
    check(is(porta_fw_rx_begin(&rx, &b1, true), PORTA_FW_OK, 0), "after an abort the same image starts at 0");
}

// Runs a whole transfer of `img` announced as `b` on a slot set up by the caller; returns END's reply.
static porta_fw_reply_t run_to_end(Slot *slot, const std::vector<uint8_t> &img, const porta_fw_begin_t &b, porta_fw_rx_t *rx)
{
    static porta_fw_ops_t o;
    o = ops(4096);
    porta_fw_rx_init(rx, &o, slot);
    porta_fw_rx_begin(rx, &b, true);
    send(rx, img, 0, img.size());
    return porta_fw_rx_end(rx, false);
}

static void test_what_end_refuses()
{
    const auto img = image(1000);
    porta_fw_rx_t rx;

    {   // The bytes that arrived are not the ones the hash was made from.
        Slot slot;
        auto b = begin_for(img);
        b.sha256[5] ^= 1;
        check(is(run_to_end(&slot, img, b, &rx), PORTA_FW_HASH, 0), "wrong hash");
        check(slot.commits == 0 && slot.finishes == 0 && slot.aborts == 1 && !porta_fw_rx_committed(&rx), "wrong hash: aborted, not committed");
        check(is(porta_fw_rx_begin(&rx, &b, true), PORTA_FW_OK, 0), "and the same image starts over at 0");
    }
    {   // The slot could not be read back.
        Slot slot;
        slot.fail_hash = true;
        check(is(run_to_end(&slot, img, begin_for(img), &rx), PORTA_FW_FLASH, 0) && slot.commits == 0 && slot.aborts == 1, "hash read failed");
    }
    {   // Right bytes, but not an image this board runs (another chip's, or truncated).
        Slot slot;
        slot.image_ok = false;
        check(is(run_to_end(&slot, img, begin_for(img), &rx), PORTA_FW_IMAGE, 0), "not an app image for this board");
        check(slot.commits == 0 && slot.aborts == 0 && !porta_fw_rx_committed(&rx), "not committed; finish closed it, so no abort");
    }
    {   // The image says it is some other version than the one announced.
        Slot slot;
        slot.image_version = "7e61488";
        check(is(run_to_end(&slot, img, begin_for(img), &rx), PORTA_FW_VERSION, 0) && slot.commits == 0, "version is not BEGIN's");
        slot.image_version = "d18f9b2-dirty";
        check(is(run_to_end(&slot, img, begin_for(img), &rx), PORTA_FW_VERSION, 0), "a longer version that starts the same");
    }
    {   // The boot partition could not be set.
        Slot slot;
        slot.fail_commit = true;
        check(is(run_to_end(&slot, img, begin_for(img), &rx), PORTA_FW_FLASH, 0) && !porta_fw_rx_committed(&rx), "commit failed");
    }
    {   // A write fails partway: the transfer is dropped, and a new BEGIN starts over.
        Slot slot;
        const porta_fw_ops_t o = ops(4096);
        porta_fw_rx_init(&rx, &o, &slot);
        const auto b = begin_for(img);
        porta_fw_rx_begin(&rx, &b, true);
        send(&rx, img, 0, 240);
        slot.fail_write = true;
        check(is(porta_fw_rx_chunk(&rx, 240, &img[240], 240), PORTA_FW_FLASH, 0) && slot.aborts == 1, "write failed");
        check(is(porta_fw_rx_chunk(&rx, 240, &img[240], 240), PORTA_FW_NO_TRANSFER, 0), "the transfer is gone");
        slot.fail_write = false;
        check(is(porta_fw_rx_begin(&rx, &b, true), PORTA_FW_OK, 0) && slot.begins == 2, "begin again from 0");
    }
}

static void test_confirm()
{
    check(porta_fw_confirm_decide(1, false) == PORTA_FW_CONFIRM_WAIT, "just booted");
    check(porta_fw_confirm_decide(3, true) == PORTA_FW_CONFIRM_KEEP, "the sidekick's HELLO arrived");
    check(porta_fw_confirm_decide(PORTA_FW_CONFIRM_TIMEOUT_S - 1, false) == PORTA_FW_CONFIRM_WAIT, "a second before the deadline");
    check(porta_fw_confirm_decide(PORTA_FW_CONFIRM_TIMEOUT_S, false) == PORTA_FW_CONFIRM_ROLL_BACK, "no sidekick: back");
    check(porta_fw_confirm_decide(PORTA_FW_CONFIRM_TIMEOUT_S + 9, true) == PORTA_FW_CONFIRM_KEEP, "a HELLO wins over the clock");
}

int main()
{
    test_whole_transfer();
    test_refusals_before_a_transfer();
    test_lost_frames_and_resume();
    test_a_different_image_starts_over();
    test_what_end_refuses();
    test_confirm();
    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
