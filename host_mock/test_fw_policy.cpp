// Host test: the decisions of a firmware update (sidekick/main/fw_policy.c;
// docs/architecture/sidekick.md (Firmware updates)).

#include "fw_policy.h"

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

static const char *SHA = "ba9e8126190833eb2c570b5d08c563ae4e04b1ad3977946633935ed7f8f09539";
static const char *URL = "http://micro-ft8.kalikstein.com/download/microft8-sidekick-continuous-ota.bin";

static std::string body(const char *board, const char *url, const char *size, const char *sha, const char *version)
{
    std::string b;
    if (board) b += std::string("board=") + board + "\n";
    if (url) b += std::string("url=") + url + "\n";
    if (size) b += std::string("size=") + size + "\n";
    if (sha) b += std::string("sha256=") + sha + "\n";
    if (version) b += std::string("version=") + version + "\n";
    return b;
}

// The field fw_request_parse() blames, or "" when it accepts the body.
static std::string blames(const std::string &b)
{
    fw_request_t req;
    const char *why = fw_request_parse(b.c_str(), &req);
    return why ? why : "";
}

static void test_parse()
{
    fw_request_t req;
    check(fw_request_parse(body("sidekick", URL, "984736", SHA, "e6e4d88").c_str(), &req) == nullptr, "a full request parses");
    check(req.board == FW_BOARD_SIDEKICK && req.size == 984736u, "board and size");
    check(std::strcmp(req.url, URL) == 0 && std::strcmp(req.sha256_hex, SHA) == 0, "url and hash");
    check(std::strcmp(req.version, "e6e4d88") == 0, "version");

    // Any order, CRLF, blank lines, no final newline.
    const std::string crlf = std::string("version=e6e4d88\r\n\r\nsha256=") + SHA + "\r\nsize=1\r\nurl=" + URL + "\r\nboard=main";
    check(fw_request_parse(crlf.c_str(), &req) == nullptr && req.board == FW_BOARD_MAIN && req.size == 1u, "order and line ends");

    check(blames(body(nullptr, URL, "1", SHA, "v")) == "board", "missing board");
    check(blames(body("sidekick", nullptr, "1", SHA, "v")) == "url", "missing url");
    check(blames(body("sidekick", URL, nullptr, SHA, "v")) == "size", "missing size");
    check(blames(body("sidekick", URL, "1", nullptr, "v")) == "sha256", "missing sha256");
    check(blames(body("sidekick", URL, "1", SHA, nullptr)) == "version", "missing version");
    check(blames("") == "board", "empty body");

    check(blames(body("p4", URL, "1", SHA, "v")) == "board", "unknown board");
    check(blames(body("sidekick", URL, "0", SHA, "v")) == "size", "zero size");
    check(blames(body("sidekick", URL, "-5", SHA, "v")) == "size", "negative size");
    check(blames(body("sidekick", URL, "12x", SHA, "v")) == "size", "size with junk");
    check(blames(body("sidekick", URL, "4294967296", SHA, "v")) == "size", "size past 32 bits");
    check(blames(body("sidekick", URL, "1", "BA9E8126190833eb2c570b5d08c563ae4e04b1ad3977946633935ed7f8f09539", "v")) == "sha256", "uppercase hash");
    check(blames(body("sidekick", URL, "1", "abc", "v")) == "sha256", "short hash");
    check(blames(body("sidekick", URL, "1", SHA, "a\"b")) == "version", "a quote in the version");
    check(blames(body("sidekick", URL, "1", SHA, "0123456789012345678901234567890x")) == "version", "32-character version");
    check(blames(body("sidekick", URL, "1", SHA, "v") + "size=2\n") == "size", "a field twice");
    check(blames(body("sidekick", URL, "1", SHA, "v") + "extra=1\n") == "body", "unknown key");
    check(blames("no equals sign\n") == "body", "not key=value");
    check(blames(std::string("url=http://h/") + std::string(400, 'a') + "\n") == "body", "overlong line");
}

static void test_url()
{
    check(fw_url_ok(URL), "the release host over plain http");
    check(fw_url_ok("http://192.168.1.20:8000/sidekick.bin"), "a bench server by address and port");
    check(!fw_url_ok(nullptr) && !fw_url_ok(""), "null and empty");
    check(!fw_url_ok("https://micro-ft8.kalikstein.com/download/x.bin"), "https refused: no TLS here");
    check(!fw_url_ok("http://"), "no host");
    check(!fw_url_ok("http:///x.bin"), "empty host");
    check(!fw_url_ok("http://host"), "no path");
    check(!fw_url_ok("http://host/"), "empty path");
    check(!fw_url_ok("http://host/a b"), "space");
    check(!fw_url_ok("http://host/a\"b"), "quote");
    check(!fw_url_ok("http://host/a\\b"), "backslash");
    const std::string longest = "http://h/" + std::string(FW_URL_MAX - 10, 'a');
    check(fw_url_ok(longest.c_str()), "the longest that fits");
    check(!fw_url_ok((longest + "a").c_str()), "one past it");
}

static void test_url_split()
{
    char host[64];
    uint16_t port = 0;
    const char *path = nullptr;
    check(fw_url_split(URL, host, sizeof(host), &port, &path) && std::strcmp(host, "micro-ft8.kalikstein.com") == 0 &&
              port == 80 && std::strcmp(path, "/download/microft8-sidekick-continuous-ota.bin") == 0, "host, default port, path");
    check(fw_url_split("http://192.168.1.20:8000/sidekick.bin", host, sizeof(host), &port, &path) &&
              std::strcmp(host, "192.168.1.20") == 0 && port == 8000 && std::strcmp(path, "/sidekick.bin") == 0, "a named port");
    check(fw_url_split("http://h/a:b", host, sizeof(host), &port, &path) && port == 80 && std::strcmp(path, "/a:b") == 0,
          "a colon in the path is not a port");
    check(!fw_url_split("http://h:/x", host, sizeof(host), &port, &path), "empty port");
    check(!fw_url_split("http://h:0/x", host, sizeof(host), &port, &path), "port 0");
    check(!fw_url_split("http://h:65536/x", host, sizeof(host), &port, &path), "port past 65535");
    check(!fw_url_split("http://h:80a/x", host, sizeof(host), &port, &path), "port with a letter");
    check(!fw_url_split("http://:80/x", host, sizeof(host), &port, &path), "port and no host");
    check(!fw_url_split("https://h/x", host, sizeof(host), &port, &path), "not a URL we fetch");
    char tiny[4];
    check(!fw_url_split("http://host/x", tiny, sizeof(tiny), &port, &path), "a host that does not fit");
    check(fw_url_split("http://abc/x", tiny, sizeof(tiny), &port, &path) && std::strcmp(tiny, "abc") == 0, "a host that just fits");
}

static int head(const std::string &s, int *status, int64_t *clen)
{
    *status = 0;
    *clen = 0;
    return fw_http_head_parse(s.data(), s.size(), status, clen);
}

static void test_http_head()
{
    int status;
    int64_t clen;
    // What the release host answers, cut down.
    const std::string cf = "HTTP/1.1 200 OK\r\nDate: Tue, 10 Nov 2026 16:31:27 GMT\r\nContent-Type: application/octet-stream\r\n"
                           "Content-Length: 1115808\r\nConnection: close\r\nServer: cloudflare\r\n\r\n";
    check(head(cf, &status, &clen) == (int)cf.size() && status == 200 && clen == 1115808, "the release host's head");
    check(head(cf + "\xE9\x01\r\n\r\n", &status, &clen) == (int)cf.size(), "body bytes after the head are not read");
    for (size_t n = 0; n < cf.size(); ++n) {
        if (head(cf.substr(0, n), &status, &clen) != 0) {
            check(false, "a head still arriving asks for more");
            break;
        }
    }
    check(head("HTTP/1.0 404 Not Found\r\ncontent-length:9\r\n\r\n", &status, &clen) > 0 && status == 404 && clen == 9,
          "status, lowercase name, no space");
    check(head("HTTP/1.0 200 OK\r\nServer: x\r\n\r\n", &status, &clen) > 0 && clen == -1, "no length given");
    check(head("HTTP/1.0 200 OK\r\n\r\n", &status, &clen) == 19 && status == 200 && clen == -1, "no headers at all");
    check(head("HTTP/1.1 200 OK\r\nX-Content-Length: 5\r\n\r\n", &status, &clen) > 0 && clen == -1, "a name that only ends the same");
    check(head("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", &status, &clen) == -1, "chunked refused");
    check(head("HTTP/1.1 200 OK\r\nContent-Length: 12x\r\n\r\n", &status, &clen) == -1, "a length with junk");
    check(head("HTTP/1.1 200 OK\r\nContent-Length: \r\n\r\n", &status, &clen) == -1, "an empty length");
    check(head("HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999\r\n\r\n", &status, &clen) == -1, "an absurd length");
    check(head("HTTP/2 200\r\n\r\nxxxxxxxx", &status, &clen) == -1, "not HTTP/1");
    check(head("HTTP/1.1 2x0 OK\r\n\r\n", &status, &clen) == -1, "a status that is not a number");
    check(head("<html>captive portal</html>\r\n\r\n", &status, &clen) == -1, "not a response");
}

static void test_gate()
{
    fw_request_t req;
    fw_request_parse(body("sidekick", URL, "984736", SHA, "e6e4d88").c_str(), &req);
    const fw_gate_t open = {true, false, false, 0x300000};
    check(fw_start_refusal(&req, &open) == nullptr, "online, idle, confirmed, fits: may start");

    fw_gate_t g = open;
    g.online = false;
    check(std::strcmp(fw_start_refusal(&req, &g), "offline") == 0, "refused on its own AP");
    g = open, g.busy = true;
    check(std::strcmp(fw_start_refusal(&req, &g), "busy") == 0, "refused while one runs");
    g = open, g.pending_verify = true;
    check(std::strcmp(fw_start_refusal(&req, &g), "confirming") == 0, "refused until the last one is kept");
    g = open, g.slot_bytes = 984735;
    check(std::strcmp(fw_start_refusal(&req, &g), "size") == 0, "refused when it cannot fit the slot");
    g.slot_bytes = 984736;
    check(fw_start_refusal(&req, &g) == nullptr, "exactly the slot fits");
    // Busy is reported ahead of anything about the request.
    g = open, g.busy = true, g.online = false;
    check(std::strcmp(fw_start_refusal(&req, &g), "busy") == 0, "busy first");

    req.board = FW_BOARD_MAIN;
    check(std::strcmp(fw_start_refusal(&req, &open), "unsupported") == 0, "main-board images not yet");
}

static void test_record_and_result()
{
    fw_record_t rec = {FW_BOARD_SIDEKICK, "d18f9b2", "7e61488"};
    char text[FW_RECORD_MAX];
    check(fw_record_format(&rec, text, sizeof(text)) && std::strcmp(text, "sidekick\td18f9b2\t7e61488") == 0, "record text");
    fw_record_t back;
    check(fw_record_parse(text, &back) && back.board == FW_BOARD_SIDEKICK &&
              std::strcmp(back.target, "d18f9b2") == 0 && std::strcmp(back.from, "7e61488") == 0, "record round trip");
    char small[10];
    check(!fw_record_format(&rec, small, sizeof(small)), "a record that does not fit");
    // The longest record fits FW_RECORD_MAX.
    fw_record_t longest = {FW_BOARD_SIDEKICK, "0123456789012345678901234567890", "0123456789012345678901234567890"};
    check(fw_record_format(&longest, text, sizeof(text)) && fw_record_parse(text, &back), "longest record");
    check(!fw_record_parse("sidekick\td18f9b2", &back), "two fields");
    check(!fw_record_parse("sidekick\ta\tb\tc", &back), "four fields");
    check(!fw_record_parse("p4\ta\tb", &back), "unknown board");
    check(!fw_record_parse("sidekick\t\tb", &back), "empty version");
    check(!fw_record_parse(nullptr, &back) && !fw_record_parse("", &back), "null and empty");

    check(fw_last_result(&rec, "d18f9b2", true) == FW_LAST_CONFIRMING, "new image on trial");
    check(fw_last_result(&rec, "d18f9b2", false) == FW_LAST_OK, "new image kept");
    check(fw_last_result(&rec, "7e61488", false) == FW_LAST_ROLLED_BACK, "old image back");
    check(fw_last_result(&rec, "7e61488-dirty", false) == FW_LAST_ROLLED_BACK, "anything but the target");
    check(std::strcmp(fw_last_name(FW_LAST_OK), "ok") == 0 && std::strcmp(fw_last_name(FW_LAST_ROLLED_BACK), "rolled_back") == 0 &&
              std::strcmp(fw_last_name(FW_LAST_CONFIRMING), "confirming") == 0, "result names");
}

static void test_confirm()
{
    check(fw_confirm_decide(1, 0, false) == FW_CONFIRM_WAIT, "just booted");
    check(fw_confirm_decide(10, 3, false) == FW_CONFIRM_WAIT, "beaconing, not on the network yet");
    check(fw_confirm_decide(2, 2, true) == FW_CONFIRM_WAIT, "on the network, beacons still short");
    check(fw_confirm_decide(10, 3, true) == FW_CONFIRM_KEEP, "beaconing and reachable: kept");
    check(fw_confirm_decide(FW_CONFIRM_TIMEOUT_S - 1, 100, false) == FW_CONFIRM_WAIT, "a second before the deadline");
    check(fw_confirm_decide(FW_CONFIRM_TIMEOUT_S, 100, false) == FW_CONFIRM_ROLL_BACK, "never reachable: back");
    check(fw_confirm_decide(FW_CONFIRM_TIMEOUT_S, 0, true) == FW_CONFIRM_ROLL_BACK, "never beaconed: back");
    check(fw_confirm_decide(FW_CONFIRM_TIMEOUT_S + 50, 100, true) == FW_CONFIRM_KEEP, "reachable wins over the clock");
}

int main()
{
    test_parse();
    test_url();
    test_url_split();
    test_http_head();
    test_gate();
    test_record_and_result();
    test_confirm();
    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
