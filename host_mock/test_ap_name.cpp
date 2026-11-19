// Host test: the sidekick's default AP name (sidekick/main/ap_name.c;
// docs/architecture/sidekick.md (The sidekick's own network)).

#include "ap_name.h"

#include <cstdio>
#include <cstring>

static int g_fail = 0;

static void check(bool ok, const char *what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        g_fail++;
    }
}

int main()
{
    const uint8_t mac[6] = {0x24, 0x58, 0x7c, 0xd1, 0x3f, 0xa2};
    char out[33];

    ap_name_default(mac, out, sizeof(out));
    check(std::strcmp(out, "micro-ft8.local 3FA2") == 0, "the last two MAC bytes, uppercase, after a space");
    check(std::strlen(out) == AP_NAME_DEFAULT_LEN, "AP_NAME_DEFAULT_LEN is the name's length");
    check(std::strlen(out) <= 32, "fits an SSID's 32 bytes");
    check(std::strncmp(out, "micro-ft8.local", 15) == 0, "starts with the address to open");

    // Two units differ in the suffix, and a leading zero is kept.
    const uint8_t other[6] = {0x24, 0x58, 0x7c, 0xd1, 0x00, 0x0b};
    char out2[33];
    ap_name_default(other, out2, sizeof(out2));
    check(std::strcmp(out2, "micro-ft8.local 000B") == 0, "leading zeros kept");
    check(std::strcmp(out, out2) != 0, "two MACs, two names");

    // A buffer that cannot hold the whole name gets none of it, not a truncated one.
    char small[AP_NAME_DEFAULT_LEN];
    std::memset(small, 'x', sizeof(small));
    ap_name_default(mac, small, sizeof(small));
    check(small[0] == '\0', "too small: empty, not truncated");
    char exact[AP_NAME_DEFAULT_LEN + 1];
    ap_name_default(mac, exact, sizeof(exact));
    check(std::strcmp(exact, "micro-ft8.local 3FA2") == 0, "exactly large enough");

    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("ap_name: all passed\n");
    return 0;
}
