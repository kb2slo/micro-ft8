// url_query: what the sidekick makes of a value a browser encoded (sidekick/main/url_query.h). Found on the
// bench 2026-11-27: a card asked for HC1MD/2 as `call=HC1MD%2F2`, the sidekick checked it undecoded, and
// the card's QSOs answered 400 for every call with a `/`.

#include <cstdio>
#include <cstring>
#include <string>

#include "url_query.h"

static int g_fails = 0;
static void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_fails;
    }
}

static std::string decoded(const char* in)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", in);
    url_decode(buf);
    return buf;
}

static bool call_of(const char* raw, std::string* out)
{
    char buf[17];
    const bool ok = url_query_callsign(raw, buf, sizeof(buf), 16);
    *out = buf;
    return ok;
}

int main()
{
    check(decoded("HC1MD%2F2") == "HC1MD/2", "%2F is a slash");
    check(decoded("vk2%2fw1abc") == "vk2/w1abc", "lower-case hex too");
    check(decoded("my+net%20work") == "my net work", "plus and %20 are spaces");
    check(decoded("100%") == "100%" && decoded("a%G1b") == "a%G1b" && decoded("%4") == "%4",
          "an escape that is not two hex digits is kept whole");
    check(decoded("") == "", "empty stays empty");

    std::string c;
    check(call_of("HC1MD%2F2", &c) && c == "HC1MD/2", "an encoded portable call");
    check(call_of("HC1MD/2", &c) && c == "HC1MD/2", "the same call unencoded");
    check(call_of("VK2%2FW1ABC%2FP", &c) && c == "VK2/W1ABC/P", "two slashes");
    check(call_of("PJ4%2FVK2%2FW1ABCD", &c) && c == "PJ4/VK2/W1ABCD", "the longest call, encoded past 16 bytes");
    check(!call_of("K1ABC%20", &c) && c.empty(), "a space is no call");
    check(!call_of("K1%3CABC", &c), "nor a <");
    check(!call_of("K1ABC%", &c), "nor a stray %");
    check(!call_of("", &c), "nor nothing");
    check(!call_of("ABCDEFGHIJKLMNOPQ", &c), "nor 17 characters");
    check(!call_of(nullptr, &c), "nor no value");

    if (g_fails) {
        std::fprintf(stderr, "url_query: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("url_query: all passed\n");
    return 0;
}
