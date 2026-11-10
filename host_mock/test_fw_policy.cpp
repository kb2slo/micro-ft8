// Host test: the decisions of a firmware update (sidekick/main/fw_policy.c;
// docs/architecture/sidekick.md (Firmware updates)).

#include "fw_policy.h"
#include "porta_fw_rx.h"
#include "porta_proto.h"

#include <vector>

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
    const fw_gate_t open = {true, false, false, 0x300000, true};
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

    // The sidekick's own image needs no main board; a main board's needs one answering.
    g = open, g.host_linked = false;
    check(fw_start_refusal(&req, &g) == nullptr, "sidekick image with no main board");
    req.board = FW_BOARD_MAIN;
    check(fw_start_refusal(&req, &open) == nullptr, "main-board image, main board linked");
    check(std::strcmp(fw_start_refusal(&req, &g), "no_host") == 0, "main-board image, nothing to send it to");
}

static void test_main_board_image()
{
    uint8_t dig[32];
    check(fw_sha256_bytes(SHA, dig) && dig[0] == 0xba && dig[1] == 0x9e && dig[31] == 0x39, "hash to bytes");
    check(!fw_sha256_bytes("abc", dig) && !fw_sha256_bytes(nullptr, dig), "not a hash");
    check(!fw_sha256_bytes("BA9E8126190833eb2c570b5d08c563ae4e04b1ad3977946633935ed7f8f09539", dig), "uppercase is not ours");

    // The start of an app image, as tools/make_manifest.py reads it too.
    uint8_t head[FW_IMAGE_HEAD] = {0};
    head[0] = 0xE9;
    head[32] = 0x32, head[33] = 0x54, head[34] = 0xCD, head[35] = 0xAB;
    std::memcpy(&head[48], "d18f9b2", 7);
    std::memcpy(&head[80], "poe_p4", 6);
    char version[FW_VERSION_MAX];
    check(fw_image_version(head, sizeof(head), version) && std::strcmp(version, "d18f9b2") == 0, "version from an app image");
    check(!fw_image_version(head, sizeof(head) - 1, version), "too few bytes");
    head[0] = 0xFF;
    check(!fw_image_version(head, sizeof(head), version), "not an image: erased flash");
    head[0] = 0xE9, head[35] = 0;
    check(!fw_image_version(head, sizeof(head), version), "no app description: a bootloader, or a merged image");
    head[35] = 0xAB;
    std::memset(&head[48], 0, 32);
    check(!fw_image_version(head, sizeof(head), version), "empty version");
    std::memset(&head[48], 'v', 32);
    check(!fw_image_version(head, sizeof(head), version), "a version that fills the field: too long for the API");
    std::memset(&head[48], 0, 32);
    std::memcpy(&head[48], "a\"b", 3);
    check(!fw_image_version(head, sizeof(head), version), "a version JSON could not quote");

    // Every status the main board can answer, and only OK, OFFSET and SHORT go on.
    check(fw_transfer_fault(PORTA_FW_OK) == nullptr && fw_transfer_fault(PORTA_FW_OFFSET) == nullptr &&
              fw_transfer_fault(PORTA_FW_SHORT) == nullptr, "go on");
    const struct { uint8_t status; const char *why; } faults[] = {
        {PORTA_FW_NOT_UPDATE_MODE, "host_mode"}, {PORTA_FW_NO_SLOT, "no_slot"}, {PORTA_FW_TOO_BIG, "size"},
        {PORTA_FW_NO_TRANSFER, "host_restarted"}, {PORTA_FW_FLASH, "host_flash"}, {PORTA_FW_HASH, "hash"},
        {PORTA_FW_IMAGE, "image"}, {PORTA_FW_VERSION, "version"}, {11, "host"}, {255, "host"},
    };
    for (const auto &f : faults) {
        const char *why = fw_transfer_fault(f.status);
        check(why && std::strcmp(why, f.why) == 0, f.why);
    }
}

static void test_main_board_confirm()
{
    const char *T = "d18f9b2";
    // Just accepted: its old image may still be sending HELLO.
    check(fw_host_result(1000, true, 200, "7e61488", T) == FW_HOST_WAIT, "old image's last HELLO is not a roll back");
    check(fw_host_result(1000, true, 200, T, T) == FW_HOST_WAIT, "nor is anything else that soon");
    // Restarting: the last HELLO is from before.
    check(fw_host_result(20000, true, 19500, "7e61488", T) == FW_HOST_WAIT, "a HELLO from before the restart");
    check(fw_host_result(20000, true, 16001, "7e61488", T) == FW_HOST_WAIT, "one from inside the settle time");
    check(fw_host_result(20000, false, 0, "", T) == FW_HOST_WAIT, "none ever");
    // Back up.
    check(fw_host_result(20000, true, 1000, T, T) == FW_HOST_UPDATED, "the new version reports in");
    check(fw_host_result(20000, true, 15999, T, T) == FW_HOST_UPDATED, "just after the settle time");
    check(fw_host_result(140000, true, 1000, "7e61488", T) == FW_HOST_ROLLED_BACK, "the old version is back");
    check(fw_host_result(20000, true, 1000, "d18f9b2-dirty", T) == FW_HOST_ROLLED_BACK, "anything but the target");
    // Never came back.
    check(fw_host_result(FW_HOST_CONFIRM_MS - 1, true, FW_HOST_CONFIRM_MS, "7e61488", T) == FW_HOST_WAIT, "a second before the deadline");
    check(fw_host_result(FW_HOST_CONFIRM_MS, true, FW_HOST_CONFIRM_MS + 5000, "7e61488", T) == FW_HOST_SILENT, "silent past it");
    check(fw_host_result(FW_HOST_CONFIRM_MS + 1, true, 100, T, T) == FW_HOST_UPDATED, "a late HELLO still counts");
}

// ---- The transfer, end to end: fw_transfer_run() here, the main board's own receiver
// (components/porta/porta_fw_rx.c) there, and between them a link that loses frames.

struct Wire {
    // The main board: its slot and receiver.
    std::vector<uint8_t> slot;
    porta_fw_rx_t rx;
    porta_fw_ops_t ops;
    bool update_mode = true;
    bool image_ok = true;
    const char *image_version = "d18f9b2";
    int commits = 0, aborts = 0;
    // The sidekick: the staged image.
    std::vector<uint8_t> staged;
    bool read_fails = false;
    // The link: every `lose_every`-th exchange loses its request (odd count) or its answer (even).
    int lose_every = 0, exchanges = 0, lost = 0;
    bool dead = false;         // nothing gets through
    int restart_at = -1;       // the main board restarts when this exchange arrives
    int cancel_at = -1;        // the operator cancels once this many bytes are acknowledged
    uint32_t progress = 0;
    int progress_calls = 0;
};

static void wire_hash(const uint8_t *p, size_t n, uint8_t out[32])
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    for (int i = 0; i < 32; ++i) out[i] = (uint8_t)(h >> ((i % 4) * 8)) ^ (uint8_t)i;
}

static bool w_begin(void *c, uint32_t size) { ((Wire *)c)->slot.assign(size, 0xFF); return true; }
static bool w_write(void *c, uint32_t off, const uint8_t *d, size_t n) { std::memcpy(&((Wire *)c)->slot[off], d, n); return true; }
static bool w_hash(void *c, uint32_t size, uint8_t out[32]) { wire_hash(((Wire *)c)->slot.data(), size, out); return true; }
static bool w_finish(void *c, char v[PORTA_FW_VERSION_LEN + 1])
{
    std::snprintf(v, PORTA_FW_VERSION_LEN + 1, "%s", ((Wire *)c)->image_version);
    return ((Wire *)c)->image_ok;
}
static bool w_commit(void *c) { ((Wire *)c)->commits++; return true; }
static void w_abort(void *c) { ((Wire *)c)->aborts++; }

static void wire_init(Wire *w, size_t image_bytes, uint32_t slot_bytes = 1 << 20)
{
    w->staged.resize(image_bytes);
    for (size_t i = 0; i < image_bytes; ++i) w->staged[i] = (uint8_t)(i * 13 + 5);
    w->ops = {slot_bytes, w_begin, w_write, w_hash, w_finish, w_commit, w_abort};
    porta_fw_rx_init(&w->rx, &w->ops, w);
}

// One request and its answer across the link, through the real codec both ways. `request` is the
// encoded frame. False when either was lost.
static bool wire_exchange(Wire *w, const uint8_t *request, size_t n, porta_fw_reply_t *out)
{
    const int k = ++w->exchanges;
    if (w->dead) return false;
    if (k == w->restart_at) porta_fw_rx_init(&w->rx, &w->ops, w);  // RAM gone; the slot's bytes stay
    const bool lossy = w->lose_every > 0 && k % w->lose_every == 0;
    if (lossy) w->lost++;
    if (lossy && (w->lost & 1)) return false;  // the request never arrived

    porta_decoder_t dec;
    porta_decoder_init(&dec);
    porta_frame_t f;
    bool got = false;
    for (size_t i = 0; i < n; ++i) got = porta_decoder_push(&dec, request[i], &f) || got;
    if (!got) return false;
    porta_fw_begin_t b;
    uint32_t offset = 0;
    const uint8_t *data = nullptr;
    size_t len = 0;
    bool abort = false;
    porta_fw_reply_t r = {0, 0};
    if (porta_proto_parse_fw_begin(&f, &b)) r = porta_fw_rx_begin(&w->rx, &b, w->update_mode);
    else if (porta_proto_parse_fw_chunk(&f, &offset, &data, &len)) r = porta_fw_rx_chunk(&w->rx, offset, data, len);
    else if (porta_proto_parse_fw_end(&f, &abort)) r = porta_fw_rx_end(&w->rx, abort);
    else return false;
    if (lossy) return false;  // it arrived and was acted on; the answer was lost

    uint8_t reply[PORTA_PROTO_MAX_FRAME];
    const size_t rn = porta_proto_encode_fw_reply(f.type, r.status, r.offset, reply, sizeof(reply));
    porta_decoder_init(&dec);
    got = false;
    for (size_t i = 0; i < rn; ++i) got = porta_decoder_push(&dec, reply[i], &f) || got;
    return got && porta_proto_parse_fw_reply(&f, nullptr, out);
}

static bool io_read(void *c, uint32_t off, uint8_t *out, size_t n)
{
    Wire *w = (Wire *)c;
    if (w->read_fails) return false;
    std::memcpy(out, &w->staged[off], n);
    return true;
}
static bool io_begin(void *c, const porta_fw_begin_t *b, porta_fw_reply_t *out)
{
    uint8_t frame[PORTA_PROTO_MAX_FRAME];
    return wire_exchange((Wire *)c, frame, porta_proto_encode_fw_begin(b, frame, sizeof(frame)), out);
}
static bool io_chunk(void *c, uint32_t off, const uint8_t *d, size_t n, porta_fw_reply_t *out)
{
    uint8_t frame[PORTA_PROTO_MAX_FRAME];
    return wire_exchange((Wire *)c, frame, porta_proto_encode_fw_chunk(off, d, n, frame, sizeof(frame)), out);
}
static bool io_end(void *c, bool abort, porta_fw_reply_t *out)
{
    uint8_t frame[PORTA_PROTO_MAX_FRAME];
    return wire_exchange((Wire *)c, frame, porta_proto_encode_fw_end(abort, frame, sizeof(frame)), out);
}
static bool io_cancelled(void *c) { return ((Wire *)c)->cancel_at >= 0 && (int)((Wire *)c)->progress >= ((Wire *)c)->cancel_at; }
static void io_progress(void *c, uint32_t off) { ((Wire *)c)->progress = off; ((Wire *)c)->progress_calls++; }

static const fw_transfer_io_t WIRE_IO = {io_read, io_begin, io_chunk, io_end, io_cancelled, io_progress};

// Runs a transfer of the staged image; returns the failure code, "" for success.
static std::string run_transfer(Wire *w, int *detail = nullptr, const char *version = "d18f9b2")
{
    porta_fw_begin_t b = {};
    b.size = (uint32_t)w->staged.size();
    wire_hash(w->staged.data(), w->staged.size(), b.sha256);
    std::snprintf(b.version, sizeof(b.version), "%s", version);
    uint8_t buf[PORTA_FW_CHUNK_MAX];
    int d = 0;
    const char *why = fw_transfer_run(&WIRE_IO, w, &b, buf, &d);
    if (detail) *detail = d;
    return why ? why : "";
}

static void test_transfer()
{
    {   // A clean link: every byte lands, and the main board installs once.
        Wire w;
        wire_init(&w, 100000);
        check(run_transfer(&w) == "" && w.slot == w.staged && w.commits == 1, "clean transfer installs");
        check(w.exchanges == 1 + 417 + 1 && w.progress == 100000 && w.progress_calls == 417, "one exchange a chunk: 417 of 240 bytes");
    }
    // A link that loses a request or an answer every few exchanges: still exact, still one install.
    for (int every : {2, 3, 5, 7, 50}) {
        Wire w;
        wire_init(&w, 100001);  // an odd size, so the last chunk is short
        w.lose_every = every;
        const std::string why = run_transfer(&w);
        char what[64];
        std::snprintf(what, sizeof(what), "losing every %d: installs the exact image, once", every);
        check(why == "" && w.slot == w.staged && w.commits == 1 && porta_fw_rx_committed(&w.rx), what);
    }
    {   // A one-chunk image, and one a byte over a chunk.
        Wire a, b;
        wire_init(&a, 1);
        wire_init(&b, PORTA_FW_CHUNK_MAX + 1);
        check(run_transfer(&a) == "" && a.slot == a.staged, "a one-byte image");
        check(run_transfer(&b) == "" && b.slot == b.staged && b.exchanges == 4, "241 bytes: two chunks");
    }
    {   // Nothing answers.
        Wire w;
        wire_init(&w, 5000);
        w.dead = true;
        check(run_transfer(&w) == "link" && w.exchanges == FW_BEGIN_TRIES, "no answer to BEGIN: link, after its tries");
    }
    {   // The link dies partway: the chunk is tried FW_CHUNK_TRIES times, then the update fails.
        Wire w;
        wire_init(&w, 5000);
        w.cancel_at = -1;
        Wire *wp = &w;
        // Let BEGIN and four chunks through, then go dead.
        struct Kill { static bool chunk(void *c, uint32_t off, const uint8_t *d, size_t n, porta_fw_reply_t *out) {
            Wire *w = (Wire *)c;
            if (off >= 960) w->dead = true;
            return io_chunk(c, off, d, n, out);
        } };
        const fw_transfer_io_t io = {io_read, io_begin, Kill::chunk, io_end, io_cancelled, io_progress};
        porta_fw_begin_t b = {};
        b.size = 5000;
        wire_hash(w.staged.data(), 5000, b.sha256);
        std::snprintf(b.version, sizeof(b.version), "d18f9b2");
        uint8_t buf[PORTA_FW_CHUNK_MAX];
        int detail = 0;
        const char *why = fw_transfer_run(&io, wp, &b, buf, &detail);
        check(why && std::string(why) == "link" && detail == 960 && w.exchanges == 1 + 4 + FW_CHUNK_TRIES, "link lost at 960");
        // The sidekick starts again with the same image: the main board still has the first 960 bytes.
        w.dead = false;
        w.exchanges = 0;
        check(run_transfer(&w) == "" && w.slot == w.staged && w.commits == 1, "the retry installs");
        check(w.exchanges == 1 + (5000 - 960 + 239) / 240 + 1, "and went on from 960 instead of 0");
    }
    {   // The main board restarts mid-transfer: it has forgotten the transfer and says so.
        Wire w;
        wire_init(&w, 5000);
        w.restart_at = 6;
        int detail = 0;
        check(run_transfer(&w, &detail) == "host_restarted" && detail == PORTA_FW_NO_TRANSFER && w.commits == 0, "main board restarted");
    }
    {   // Refusals at BEGIN.
        Wire w;
        wire_init(&w, 5000);
        w.update_mode = false;
        check(run_transfer(&w) == "host_mode" && w.exchanges == 1, "not in update mode");
        Wire single;
        wire_init(&single, 5000, 0);
        check(run_transfer(&single) == "no_slot", "a main board with one app partition");
        Wire small;
        wire_init(&small, 5000, 4999);
        check(run_transfer(&small) == "size", "too big for its slot");
    }
    {   // What END refuses.
        Wire w;
        wire_init(&w, 5000);
        w.image_ok = false;
        check(run_transfer(&w) == "image" && w.commits == 0, "not an image for that board");
        Wire v;
        wire_init(&v, 5000);
        v.image_version = "7e61488";
        check(run_transfer(&v) == "version" && v.commits == 0, "the image is another version");
        Wire h;
        wire_init(&h, 5000);
        struct Flip { static bool chunk(void *c, uint32_t off, const uint8_t *d, size_t n, porta_fw_reply_t *out) {
            uint8_t copy[PORTA_FW_CHUNK_MAX];
            std::memcpy(copy, d, n);
            if (off == 480) copy[3] ^= 0x10;  // a bit flipped before the frame's CRC was made
            return io_chunk(c, off, copy, n, out);
        } };
        const fw_transfer_io_t io = {io_read, io_begin, Flip::chunk, io_end, io_cancelled, io_progress};
        porta_fw_begin_t b = {};
        b.size = 5000;
        wire_hash(h.staged.data(), 5000, b.sha256);
        std::snprintf(b.version, sizeof(b.version), "d18f9b2");
        uint8_t buf[PORTA_FW_CHUNK_MAX];
        int detail = 0;
        const char *why = fw_transfer_run(&io, &h, &b, buf, &detail);
        check(why && std::string(why) == "hash" && h.commits == 0, "bytes that are not the hashed ones");
    }
    {   // Cancelled partway: the main board is told to forget it.
        Wire w;
        wire_init(&w, 5000);
        w.cancel_at = 1200;
        check(run_transfer(&w) == "cancelled" && w.commits == 0 && w.aborts == 1, "cancel aborts the main board's transfer");
        w.cancel_at = -1;
        w.exchanges = 0;
        check(run_transfer(&w) == "" && w.exchanges == 1 + 21 + 1, "and the next try starts from 0");
    }
    {   // The staged image cannot be read.
        Wire w;
        wire_init(&w, 5000);
        w.read_fails = true;
        check(run_transfer(&w) == "flash" && w.commits == 0, "staged image unreadable");
    }
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
    test_main_board_image();
    test_main_board_confirm();
    test_transfer();
    test_record_and_result();
    test_confirm();
    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
