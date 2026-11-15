// Host test for the web app's pack reader (sidekick/main/app_pack.c) against the builder's own
// output: app_pack_sample.bin, which the Makefile has webapp/tools/app_pack.py --sample write. So
// the two sides of the format are held together by a pack one wrote and the other reads, not by
// two descriptions of it. The sample's names, sizes and bytes below are app_pack.py's SAMPLE.
//
// Then every way a pack can lie about itself: this reader is what stands between a pushed file and
// a pointer served out of flash.

#include "app_pack.h"

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

static std::vector<uint8_t> read_file(const char *path)
{
    std::vector<uint8_t> out;
    FILE *f = std::fopen(path, "rb");
    if (!f) {
        return out;
    }
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.insert(out.end(), buf, buf + n);
    }
    std::fclose(f);
    return out;
}

static void put_u32(std::vector<uint8_t> &v, size_t at, uint32_t x)
{
    for (int i = 0; i < 4; ++i) {
        v[at + i] = (uint8_t)(x >> (8 * i));
    }
}

static std::string why(const std::vector<uint8_t> &v)
{
    app_pack_t p;
    const char *w = app_pack_open(v.data(), v.size(), &p);
    return w ? w : "";
}

static const size_t ENTRY0 = APP_PACK_HEAD_LEN;
static const size_t OFFSET_AT = APP_PACK_NAME_MAX;
static const size_t SIZE_AT = APP_PACK_NAME_MAX + 4;

int main()
{
    const std::vector<uint8_t> sample = read_file("app_pack_sample.bin");
    check(!sample.empty(), "app_pack_sample.bin is there (the Makefile writes it with app_pack.py --sample)");
    if (sample.empty()) {
        return 1;
    }

    // ---- what the builder wrote, read back ----
    app_pack_t pack;
    const char *bad = app_pack_open(sample.data(), sample.size(), &pack);
    check(bad == nullptr, "the builder's pack opens");
    if (bad) {
        std::printf("  app_pack_open: %s\n", bad);
        return 1;
    }
    check(std::strcmp(pack.build, "b1322+d18f9b2") == 0, "its build id");
    check(pack.count == 3, "three assets");
    check(pack.len == sample.size(), "its length is the file's");
    check(app_pack_claimed_len(sample.data()) == sample.size(), "the header alone gives the length");

    const uint8_t *bytes = nullptr;
    size_t size = 0;
    check(app_pack_find(&pack, "app.html", &bytes, &size) && size == 2 + 768, "app.html: its size");
    bool same = size == 770 && bytes[0] == 0x1f && bytes[1] == 0x8b;
    for (size_t i = 0; same && i < 768; ++i) {
        same = bytes[2 + i] == (uint8_t)(i & 0xff);
    }
    check(same, "app.html: every byte, the gzip magic first");
    check(bytes >= sample.data() && bytes + size <= sample.data() + sample.size(), "served from the pack's own bytes");
    check(app_pack_find(&pack, "bands.js", &bytes, &size) && size == 13 && std::memcmp(bytes, "var BANDS=1;\n", 13) == 0,
          "bands.js: its bytes");
    check(app_pack_find(&pack, "empty.json", &bytes, &size) && size == 0, "an empty asset is found, with no bytes");
    check(!app_pack_find(&pack, "missing.js", &bytes, &size), "a name not in the pack is not found");
    check(!app_pack_find(&pack, "app.htm", &bytes, &size), "nor a prefix of one that is");
    check(!app_pack_find(&pack, "", &bytes, &size), "nor the empty name");

    const char *name = nullptr;
    check(app_pack_entry(&pack, 0, &name, &bytes, &size) && std::strcmp(name, "app.html") == 0, "entries in name order: 0");
    check(app_pack_entry(&pack, 2, &name, &bytes, &size) && std::strcmp(name, "empty.json") == 0, "entries in name order: 2");
    check(!app_pack_entry(&pack, 3, &name, &bytes, &size), "no entry past the count");

    // A pack in a flash slot: more bytes may be readable than the pack is long.
    std::vector<uint8_t> v = sample;
    v.resize(sample.size() + 4096, 0xff);
    check(why(v).empty(), "a pack followed by other bytes opens");
    check(app_pack_open(v.data(), v.size(), &pack) == nullptr && pack.len == sample.size(), "and its length is its own");

    // ---- a pack that lies ----
    v = sample;
    v.resize(sample.size() - 1);
    check(why(v) == "length", "one byte short of its claimed length");
    v = sample;
    v.resize(APP_PACK_HEAD_LEN - 1);
    check(why(v) == "short", "shorter than a header");
    check(app_pack_open(nullptr, 100, &pack) != nullptr, "no bytes at all");
    v = sample;
    v[0] ^= 0x20;
    check(why(v) == "magic", "another magic");
    check(app_pack_claimed_len(v.data()) == 0, "and its header claims no length");
    v = sample;
    v[8] = 2;
    check(why(v) == "format", "another format");
    v = sample;
    std::memset(&v[16], 'x', APP_PACK_BUILD_MAX);
    check(why(v) == "build", "a build id with no end");
    v = sample;
    v[16] = '\0';
    check(why(v) == "build", "an empty build id");
    v = sample;
    v[17] = '"';
    check(why(v) == "build", "a build id that would break out of JSON");
    v = sample;
    v[10] = 0;
    v[11] = 0;
    check(why(v) == "index", "no assets");
    v = sample;
    v[10] = 0xff;
    v[11] = 0xff;
    check(why(v) == "index", "an index longer than the pack");
    v = sample;
    std::memset(&v[ENTRY0], 'a', APP_PACK_NAME_MAX);
    check(why(v) == "name", "a name with no end");
    v = sample;
    v[ENTRY0] = '.';
    check(why(v) == "name", "a dotfile");
    v = sample;
    v[ENTRY0 + 1] = '/';
    check(why(v) == "name", "a path");
    v = sample;
    v[ENTRY0] = '\0';
    check(why(v) == "name", "an empty name");
    v = sample;
    put_u32(v, ENTRY0 + OFFSET_AT, 16);
    check(why(v) == "bounds", "an asset inside the header");
    v = sample;
    put_u32(v, ENTRY0 + OFFSET_AT, (uint32_t)sample.size() + 1);
    check(why(v) == "bounds", "an asset that starts past the end");
    v = sample;
    put_u32(v, ENTRY0 + SIZE_AT, (uint32_t)sample.size());
    check(why(v) == "bounds", "an asset that runs past the end");
    v = sample;
    put_u32(v, ENTRY0 + OFFSET_AT, 0xfffffff0u);
    put_u32(v, ENTRY0 + SIZE_AT, 0x20u);
    check(why(v) == "bounds", "an offset and size that wrap");
    // The last asset may end exactly at the end, and be empty there.
    v = sample;
    put_u32(v, ENTRY0 + 2 * APP_PACK_ENTRY_LEN + OFFSET_AT, (uint32_t)sample.size());
    check(why(v).empty(), "an empty asset at the very end");

    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("ok (app pack)\n");
    return 0;
}
