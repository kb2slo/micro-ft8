// Host test for the rules of a bench push (sidekick/main/app_push.c): a push is served only on the
// image it names, and a release's version refuses one.

#include "app_push.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0;

static void check(bool ok, const char *what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        g_fail++;
    }
}

static std::string str(const char *s)
{
    return s ? s : "";
}

static const uint32_t SLOT = 0x300000;
static const char *const DEV = "b1322+0c5ccec";

int main()
{
    uint8_t image_a[32], image_b[32];
    for (int i = 0; i < 32; ++i) {
        image_a[i] = (uint8_t)(0xa0 + i);
        image_b[i] = (uint8_t)(0xa0 + i);
    }
    image_b[31] ^= 1;  // two builds of one changed tree: the same version, another ELF

    app_push_head_t head = {};
    head.pack_len = 186869;
    std::memcpy(head.image, image_a, 32);
    for (int i = 0; i < 32; ++i) {
        head.pack_sha256[i] = (uint8_t)i;
    }
    uint8_t rec[APP_PUSH_RECORD_LEN];
    app_push_head_write(&head, rec);

    // ---- served only on the image it names ----
    app_push_head_t got = {};
    check(app_push_head_read(rec, DEV, image_a, SLOT, &got) == nullptr, "a push onto this image is read");
    check(got.pack_len == head.pack_len && std::memcmp(got.pack_sha256, head.pack_sha256, 32) == 0 &&
              std::memcmp(got.image, image_a, 32) == 0,
          "with its length and its pack's hash");
    check(str(app_push_head_read(rec, DEV, image_b, SLOT, &got)) == "image",
          "on another image it is not: a firmware change returns the station to its image's app");

    // ---- never on a release, whatever its slot holds ----
    // The slot has other writers (a main board's image is staged there as plain bytes), so the
    // rule is held where a push is served, not only where one is accepted.
    check(str(app_push_head_read(rec, "0.1.0+0c5ccec", image_a, SLOT, &got)) == "release",
          "a release serves no push, though the header names it");
    check(app_push_head_read(rec, "0c5ccec-dirty", image_a, SLOT, &got) == nullptr, "a bench build serves one");

    // ---- what else a slot's first bytes can be ----
    uint8_t other[APP_PUSH_RECORD_LEN];
    std::memset(other, 0xff, sizeof(other));
    check(str(app_push_head_read(other, DEV, image_a, SLOT, &got)) == "none", "erased flash is no push");
    std::memset(other, 0, sizeof(other));
    other[0] = 0xe9;  // an app image's magic: the firmware to go back to, or a staged one
    check(str(app_push_head_read(other, DEV, image_a, SLOT, &got)) == "none", "a firmware image is no push");
    check(str(app_push_head_read(other, "0.1.0+0c5ccec", image_a, SLOT, &got)) == "none", "on a release too, quietly");
    std::memcpy(other, rec, sizeof(rec));
    other[8] = 2;
    check(str(app_push_head_read(other, DEV, image_a, SLOT, &got)) == "format", "another format");

    head.pack_len = SLOT - APP_PUSH_PACK_AT;
    app_push_head_write(&head, rec);
    check(app_push_head_read(rec, DEV, image_a, SLOT, &got) == nullptr, "a pack that fills the slot behind the header");
    head.pack_len = SLOT - APP_PUSH_PACK_AT + 1;
    app_push_head_write(&head, rec);
    check(str(app_push_head_read(rec, DEV, image_a, SLOT, &got)) == "size", "one byte more does not fit");
    head.pack_len = 0xffffffffu;
    app_push_head_write(&head, rec);
    check(str(app_push_head_read(rec, DEV, image_a, SLOT, &got)) == "size", "a length that would wrap");
    head.pack_len = 10;
    app_push_head_write(&head, rec);
    check(str(app_push_head_read(rec, DEV, image_a, SLOT, &got)) == "size", "shorter than any pack");
    check(str(app_push_head_read(rec, DEV, image_a, 1024, &got)) == "size", "a slot smaller than the header's sector");

    // ---- a release's version, as tools/build_version.py writes versions ----
    for (const char *v : {"0.1.0+0c5ccec", "1.0.0+abcdef0", "10.20.30+0123456"}) {
        check(app_push_is_release(v), v);
    }
    for (const char *v : {"b1322+0c5ccec", "0c5ccec", "0c5ccec-dirty", "", "unknown", "0.1.0", "0.1+0c5ccec", "0.1.0+0c5cce",
                          "0.1.0+0c5ccecf", "0.1.0+0C5CCEC", "0.1.0+0c5ccec-dirty", "v0.1.0+0c5ccec", "0.1.0.0+0c5ccec",
                          ".1.0+0c5ccec", "0..0+0c5ccec"}) {
        check(!app_push_is_release(v), v);
    }

    // ---- whether a push may be written ----
    app_push_gate_t gate = {};
    gate.version = "b1322+0c5ccec";
    gate.slot_bytes = SLOT;
    gate.pack_len = 186869;
    check(app_push_refusal(&gate) == nullptr, "a dev build takes a push");
    gate.version = "0c5ccec-dirty";
    check(app_push_refusal(&gate) == nullptr, "so does a bench build");
    gate.version = "0.1.0+0c5ccec";
    check(str(app_push_refusal(&gate)) == "release", "a release refuses one");
    gate.busy = true;
    gate.pending_verify = true;
    check(str(app_push_refusal(&gate)) == "release", "whatever else is true of it");
    gate.version = "b1322+0c5ccec";
    check(str(app_push_refusal(&gate)) == "busy", "an update is running: its image is staged in the same slot");
    gate.busy = false;
    check(str(app_push_refusal(&gate)) == "confirming", "an image on trial: the slot holds the one to go back to");
    gate.pending_verify = false;
    gate.pack_len = SLOT;
    check(str(app_push_refusal(&gate)) == "size", "a body larger than the slot holds");
    gate.pack_len = 0;
    check(str(app_push_refusal(&gate)) == "size", "an empty body");

    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("ok (app push)\n");
    return 0;
}
