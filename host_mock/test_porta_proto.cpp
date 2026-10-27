// Host test for the sidekick/main frame codec (I28a, docs/architecture/sidekick.md (Port A)).
//
// The codec is where a link protocol quietly goes wrong: a length off by one,
// a CRC that misses a class of errors, a decoder that treats a payload byte as
// a frame start, or one that never recovers after noise. None of that is
// visible on a bench -- it shows up as "the link is flaky" months later. So
// every one of those is pinned here, including the deliberate limitation
// (a corrupt frame can swallow the frame behind it) so that behaviour is a
// recorded decision rather than a surprise.

#include "porta_proto.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;

static void check(bool ok, const char* what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        g_fail++;
    }
}

// Feeds every byte and collects whatever frames fall out.
static std::vector<porta_frame_t> run(porta_decoder_t* d, const std::vector<uint8_t>& bytes)
{
    std::vector<porta_frame_t> got;
    porta_frame_t f;
    for (uint8_t b : bytes) {
        if (porta_decoder_push(d, b, &f)) {
            got.push_back(f);
        }
    }
    return got;
}

static std::vector<uint8_t> encode(uint8_t type, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> buf(PORTA_PROTO_MAX_FRAME);
    size_t n = porta_proto_encode(type, payload.empty() ? nullptr : payload.data(),
                                  (uint8_t)payload.size(), buf.data(), buf.size());
    buf.resize(n);
    return buf;
}

static void test_round_trip()
{
    porta_decoder_t d;
    porta_decoder_init(&d);

    std::vector<uint8_t> payload = {0x01, 0x02, 0x03, 0xFF, 0x00};
    auto wire = encode(PORTA_MSG_CONFIG_VALUE, payload);
    check(wire.size() == payload.size() + 4, "frame is payload + 4 overhead bytes");
    check(wire[0] == PORTA_PROTO_SYNC, "frame starts with the sync byte");
    check(wire[1] == PORTA_MSG_CONFIG_VALUE, "type is second");
    check(wire[2] == payload.size(), "length is third");

    auto got = run(&d, wire);
    check(got.size() == 1, "one frame in, one frame out");
    if (got.size() == 1) {
        check(got[0].type == PORTA_MSG_CONFIG_VALUE, "type round-trips");
        check(got[0].len == payload.size(), "length round-trips");
        check(std::memcmp(got[0].payload, payload.data(), payload.size()) == 0,
              "payload round-trips");
    }
    check(d.frames_ok == 1 && d.crc_errors == 0, "counters after a clean frame");
}

static void test_empty_and_max_payload()
{
    porta_decoder_t d;
    porta_decoder_init(&d);

    // len == 0 is legal: POLL and NOTHING carry nothing.
    auto empty = encode(PORTA_MSG_POLL, {});
    check(empty.size() == 4, "empty frame is 4 bytes");
    auto got = run(&d, empty);
    check(got.size() == 1 && got[0].len == 0, "zero-length payload decodes");

    // 255 is the largest length the single length byte can express.
    std::vector<uint8_t> big(PORTA_PROTO_MAX_PAYLOAD);
    for (size_t i = 0; i < big.size(); ++i) {
        big[i] = (uint8_t)(i * 7 + 3);
    }
    auto wire = encode(PORTA_MSG_FILE_DATA, big);
    check(wire.size() == PORTA_PROTO_MAX_FRAME, "max frame is 259 bytes");
    got = run(&d, wire);
    check(got.size() == 1, "max-length frame decodes");
    if (got.size() == 1) {
        check(got[0].len == PORTA_PROTO_MAX_PAYLOAD, "max length round-trips");
        check(std::memcmp(got[0].payload, big.data(), big.size()) == 0,
              "max payload round-trips");
    }
}

// The reason the decoder counts bytes instead of hunting for sync: payloads are
// binary and 0xC6 will occur in them.
static void test_sync_byte_inside_payload()
{
    porta_decoder_t d;
    porta_decoder_init(&d);

    std::vector<uint8_t> payload = {PORTA_PROTO_SYNC, PORTA_PROTO_SYNC, 0x00, PORTA_PROTO_SYNC};
    auto got = run(&d, encode(PORTA_MSG_FILE_DATA, payload));
    check(got.size() == 1, "a payload full of sync bytes is still one frame");
    if (got.size() == 1) {
        check(got[0].len == 4 && got[0].payload[0] == PORTA_PROTO_SYNC,
              "sync bytes inside a payload are data");
    }
}

// Every single-bit flip anywhere in the frame must be caught. This is the whole
// job of the CRC, and a sum-of-bytes checksum (what the old beacon used) fails
// it -- two compensating flips, or a flip in the high bit, can slip through.
static void test_crc_catches_every_single_bit_flip()
{
    std::vector<uint8_t> payload = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70};
    auto clean = encode(PORTA_MSG_CONFIG_VALUE, payload);

    int missed = 0;
    for (size_t byte = 0; byte < clean.size(); ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            auto corrupt = clean;
            corrupt[byte] ^= (uint8_t)(1u << bit);
            if (corrupt[0] != PORTA_PROTO_SYNC) {
                continue;  // flipping the sync byte means the frame is never started
            }
            porta_decoder_t d;
            porta_decoder_init(&d);
            if (!run(&d, corrupt).empty()) {
                missed++;
            }
        }
    }
    check(missed == 0, "CRC rejects every single-bit flip in the framed bytes");
}

static void test_truncated_frame_then_recovery()
{
    porta_decoder_t d;
    porta_decoder_init(&d);

    auto full = encode(PORTA_MSG_CONFIG_VALUE, {1, 2, 3, 4, 5, 6, 7, 8});
    std::vector<uint8_t> truncated(full.begin(), full.end() - 3);
    auto got = run(&d, truncated);
    check(got.empty(), "a truncated frame yields nothing");

    // The decoder is mid-payload, so the next frame's sync is consumed as
    // payload -- the truncated frame swallows it, exactly as documented. What
    // matters is that it recovers rather than wedging.
    got = run(&d, full);
    got = run(&d, full);
    check(!got.empty(), "decoder recovers after a truncated frame");
}

static void test_garbage_before_a_frame()
{
    porta_decoder_t d;
    porta_decoder_init(&d);

    std::vector<uint8_t> stream = {'$', 'G', 'P', 'G', 'G', 'A', ',', '1', '2', '\r', '\n'};
    auto frame = encode(PORTA_MSG_HELLO, std::vector<uint8_t>(PORTA_HELLO_PAYLOAD_LEN, 0x41));
    stream.insert(stream.end(), frame.begin(), frame.end());

    auto got = run(&d, stream);
    check(got.size() == 1, "NMEA-looking garbage before a frame is skipped");
    check(d.crc_errors == 0, "clean garbage costs no CRC error");
}

// The documented limitation, pinned so it stays a decision. Noise containing a
// stray sync byte starts a false frame that eats the real one behind it; the
// frame after that is found.
static void test_false_sync_eats_one_frame_then_recovers()
{
    porta_decoder_t d;
    porta_decoder_init(&d);

    std::vector<uint8_t> stream = {PORTA_PROTO_SYNC, 0x99, 0x04, 0xAA};  // bogus header
    auto frame = encode(PORTA_MSG_POLL, {});
    stream.insert(stream.end(), frame.begin(), frame.end());
    stream.insert(stream.end(), frame.begin(), frame.end());

    auto got = run(&d, stream);
    check(got.size() == 1, "a false sync costs exactly one real frame");
    check(d.crc_errors == 1, "the false frame is counted as a CRC error");
}

static void test_encode_refuses_a_short_buffer()
{
    uint8_t small[4];
    std::vector<uint8_t> payload = {1, 2, 3, 4, 5};
    check(porta_proto_encode(PORTA_MSG_CONFIG_VALUE, payload.data(), (uint8_t)payload.size(),
                             small, sizeof(small)) == 0,
          "encode refuses rather than writing a partial frame");

    uint8_t exact[4];
    check(porta_proto_encode(PORTA_MSG_POLL, nullptr, 0, exact, sizeof(exact)) == 4,
          "a buffer of exactly the needed size is accepted");
}

// Counters must survive the decoder's own internal resync after a bad frame --
// otherwise a wire quietly failing half its frames looks healthy. They must NOT
// survive an explicit init, which means "this is a new link".
static void test_counters()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    check(d.frames_ok == 0 && d.crc_errors == 0, "init zeroes the counters");

    run(&d, encode(PORTA_MSG_POLL, {}));
    check(d.frames_ok == 1, "one good frame counted");

    auto bad = encode(PORTA_MSG_POLL, {});
    bad.back() ^= 0xFF;
    run(&d, bad);
    check(d.crc_errors == 1, "a bad frame is counted");
    check(d.frames_ok == 1, "an internal resync keeps the good-frame count");

    run(&d, encode(PORTA_MSG_POLL, {}));
    check(d.frames_ok == 2, "decoder still works after a bad frame");

    porta_decoder_init(&d);
    check(d.frames_ok == 0 && d.crc_errors == 0, "an explicit init starts a new link");
}

static void test_hello_round_trip()
{
    porta_decoder_t d;
    porta_decoder_init(&d);

    uint8_t buf[PORTA_PROTO_MAX_FRAME];
    size_t n = porta_proto_encode_hello(PORTA_PROTO_VERSION, "f0c79d3-dirty", buf, sizeof(buf));
    check(n == PORTA_HELLO_PAYLOAD_LEN + 4, "hello frame size");

    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "hello decodes");
    if (got.size() == 1) {
        uint8_t ver = 0;
        char build[PORTA_HELLO_VERSION_LEN + 1] = {};
        check(porta_proto_parse_hello(&got[0], &ver, build), "hello payload parses");
        check(ver == PORTA_PROTO_VERSION, "protocol version round-trips");
        check(std::string(build) == "f0c79d3-dirty", "build version round-trips");
    }

    // A version that fills all 32 bytes has no room for a terminator on the
    // wire; the decoder must still hand back a usable C string.
    std::string full(PORTA_HELLO_VERSION_LEN, 'x');
    n = porta_proto_encode_hello(7, full.c_str(), buf, sizeof(buf));
    porta_decoder_init(&d);
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "hello with a full-width version decodes");
    if (got.size() == 1) {
        uint8_t ver = 0;
        char build[PORTA_HELLO_VERSION_LEN + 1] = {};
        check(porta_proto_parse_hello(&got[0], &ver, build), "full-width hello parses");
        check(std::string(build) == full, "a 32-character version is not truncated");
    }
}

static void test_hello_rejects_wrong_shape()
{
    porta_frame_t f = {};
    f.type = PORTA_MSG_CONFIG_VALUE;
    f.len = PORTA_HELLO_PAYLOAD_LEN;
    check(!porta_proto_parse_hello(&f, nullptr, nullptr), "wrong type is not a hello");

    f.type = PORTA_MSG_HELLO;
    f.len = 4;
    check(!porta_proto_parse_hello(&f, nullptr, nullptr), "wrong length is not a hello");
}

// EVENT is a namespace: subtype first, body after. These pin the field packing
// and, more importantly, the boundaries -- an empty text, a full-width text, and
// a frame one byte too short to hold the fixed part.
static void test_log_event()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    size_t n = porta_proto_encode_log(1789000000u, "Queued: CQ KB2SLO FN30", buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "log event decodes");
    if (got.size() == 1) {
        char text[PORTA_EVENT_TEXT_MAX + 1] = {};
        uint32_t ts = 0;
        check(porta_proto_parse_log(&got[0], &ts, text), "log event parses");
        check(ts == 1789000000u, "timestamp round-trips through the envelope");
        check(std::string(text) == "Queued: CQ KB2SLO FN30", "log text round-trips");
        // A decode parser must reject a log event and vice versa: same frame
        // type, different subtype, and confusing them would silently misread
        // the first seven bytes of a message as numeric fields.
        porta_decode_event_t dec;
        check(!porta_proto_parse_decode(&got[0], &dec), "a log event is not a decode event");
    }

    // Empty text is legal -- a zero-length log line is odd but not malformed.
    porta_decoder_init(&d);
    n = porta_proto_encode_log(0, "", buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1 && got[0].len == 5, "empty log event is envelope only");

    // Longer than the field: truncated, not overflowed.
    porta_decoder_init(&d);
    std::string longtext(PORTA_EVENT_TEXT_MAX + 40, 'z');
    n = porta_proto_encode_log(42u, longtext.c_str(), buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "over-long log event still encodes");
    if (got.size() == 1) {
        char text[PORTA_EVENT_TEXT_MAX + 1] = {};
        porta_proto_parse_log(&got[0], nullptr, text);
        check(std::strlen(text) == PORTA_EVENT_TEXT_MAX, "over-long text is truncated to the field");
    }
}

// Zero is "the host does not know the time", which happens on a cold unit with
// no GPS and no DS3231. It must survive as zero rather than being rejected or
// substituted, because the viewer needs to tell "no clock" from "midnight".
static void test_unset_clock_round_trips()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];
    size_t n = porta_proto_encode_log(0, "no clock yet", buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "unset-clock log event decodes");
    if (got.size() == 1) {
        uint32_t ts = 12345;
        char text[PORTA_EVENT_TEXT_MAX + 1] = {};
        check(porta_proto_parse_log(&got[0], &ts, text), "unset-clock event parses");
        check(ts == 0, "a zero timestamp stays zero");
    }
}

static void test_decode_event()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    porta_decode_event_t in = {};
    std::snprintf(in.text, sizeof(in.text), "CQ DX W1AW FN31");
    in.epoch_secs = 1789012345u;
    in.decode_id = 424242u;    // non-zero, to catch a field that silently reads as zero
    in.snr = -21;              // negative, to catch an unsigned round-trip
    in.offset_hz = 2750;       // above 2047, to catch a truncated width
    in.dt_centis = -145;       // negative, likewise
    in.is_cq = true;
    in.is_recent_qso = true;
    in.slot_idx = 119267489u;  // a real 2026 FT8 slot, above 2^24 to catch a narrow width
    in.slot_len_ds = 150;
    in.dial_hz = 14074000u;    // above 2^16, likewise

    size_t n = porta_proto_encode_decode(&in, buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "decode event decodes");
    if (got.size() == 1) {
        porta_decode_event_t out;
        check(porta_proto_parse_decode(&got[0], &out), "decode event parses");
        check(std::string(out.text) == "CQ DX W1AW FN31", "text round-trips");
        check(out.decode_id == 424242u, "decode_id round-trips");
        check(out.snr == -21, "negative SNR round-trips");
        check(out.offset_hz == 2750, "offset above 2047 round-trips");
        check(out.dt_centis == -145, "negative dt round-trips");
        check(out.epoch_secs == 1789012345u, "decode timestamp round-trips");
        check(out.is_cq && out.is_recent_qso && !out.is_to_me, "flags round-trip independently");
        check(out.slot_idx == 119267489u, "slot index round-trips");
        check(out.slot_len_ds == 150, "slot length round-trips");
        check(out.dial_hz == 14074000u, "dial frequency round-trips");

        char text[PORTA_EVENT_TEXT_MAX + 1] = {};
        check(!porta_proto_parse_log(&got[0], nullptr, text), "a decode event is not a log event");
    }

    // A frame carrying the subtype but not the fixed fields must be rejected
    // rather than read past its own length.
    porta_frame_t stub = {};
    stub.type = PORTA_MSG_EVENT;
    stub.len = 8;   // envelope plus part of the fixed body
    stub.payload[0] = PORTA_EVT_DECODE;
    porta_decode_event_t out;
    check(!porta_proto_parse_decode(&stub, &out), "a short decode event is rejected");
}

// QUEUE_ENTRY reports one autoseq context. entry_id is the load-bearing field:
// it must survive independently of state/tries/dxcall, since QUEUE_CANCEL
// addresses by this id rather than by list position (the queue re-sorts by
// priority, so a position is not safe to cancel by).
static void test_queue_entry_event()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    porta_queue_entry_event_t in = {};
    in.epoch_secs = 1789012345u;
    in.entry_id = 4242;
    in.state = 1;            // REPLYING
    in.tries_sent = 2;
    in.try_limit = 12;
    in.flags = PORTA_QUEUE_ODD | PORTA_QUEUE_CUSTOM;
    std::snprintf(in.dxcall, sizeof(in.dxcall), "KB2SLO/P");  // exercise a portable suffix

    size_t n = porta_proto_encode_queue_entry(&in, buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "queue_entry event decodes");
    if (got.size() == 1) {
        porta_queue_entry_event_t out;
        check(porta_proto_parse_queue_entry(&got[0], &out), "queue_entry event parses");
        check(out.entry_id == 4242, "entry_id round-trips");
        check(out.state == 1, "state round-trips");
        check(out.tries_sent == 2 && out.try_limit == 12, "tries round-trip");
        check(out.flags == (PORTA_QUEUE_ODD | PORTA_QUEUE_CUSTOM), "parity and custom round-trip");
        check(std::string(out.dxcall) == "KB2SLO/P", "dxcall with a portable suffix round-trips");

        porta_decode_event_t dec;
        check(!porta_proto_parse_decode(&got[0], &dec), "a queue_entry event is not a decode event");
    }

    // IDLE (state 6) is the removal signal, not a separate flag -- must round
    // trip like any other state rather than being special-cased away.
    porta_decoder_init(&d);
    in.state = 6;
    n = porta_proto_encode_queue_entry(&in, buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    if (got.size() == 1) {
        porta_queue_entry_event_t out;
        check(porta_proto_parse_queue_entry(&got[0], &out) && out.state == 6,
              "IDLE state round-trips like any other -- it is the removal signal");
    }

    // A dxcall_len claiming more bytes than the frame actually carries must be
    // rejected rather than read past the payload.
    porta_frame_t stub = {};
    stub.type = PORTA_MSG_EVENT;
    stub.len = 12;  // fixed part only
    stub.payload[0] = PORTA_EVT_QUEUE_ENTRY;
    stub.payload[11] = 5;  // claims a 5-byte dxcall that is not there
    porta_queue_entry_event_t out;
    check(!porta_proto_parse_queue_entry(&stub, &out), "a truncated dxcall is rejected");
}

static void test_slot_state_event()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    porta_slot_state_event_t in = {};
    in.epoch_secs = 1789012345u;
    in.slot_parity = 1;
    in.resolved_offset_hz = 1834;

    size_t n = porta_proto_encode_slot_state(&in, buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "slot_state event decodes");
    if (got.size() == 1) {
        porta_slot_state_event_t out;
        check(porta_proto_parse_slot_state(&got[0], &out), "slot_state event parses");
        check(out.slot_parity == 1, "slot_parity round-trips");
        check(out.resolved_offset_hz == 1834, "resolved_offset_hz round-trips");

        porta_queue_entry_event_t qe;
        check(!porta_proto_parse_queue_entry(&got[0], &qe),
              "a slot_state event is not a queue_entry event");
    }
}

static void test_usb_status_event()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    porta_usb_status_event_t in = {};
    in.epoch_secs = 1789012345u;
    in.present = true;
    in.kind = 0;  // UsbCKind::QmxQdx
    in.vid = 0x0483;
    in.pid = 0xA34C;

    size_t n = porta_proto_encode_usb_status(&in, buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "usb_status event decodes");
    if (got.size() == 1) {
        porta_usb_status_event_t out;
        check(porta_proto_parse_usb_status(&got[0], &out), "usb_status event parses");
        check(out.present, "present round-trips true (attach)");
        check(out.kind == 0, "kind round-trips");
        check(out.vid == 0x0483, "vid round-trips");
        check(out.pid == 0xA34C, "pid round-trips");

        porta_slot_state_event_t ss;
        check(!porta_proto_parse_slot_state(&got[0], &ss),
              "a usb_status event is not a slot_state event");
    }

    // Detach of an unrecognized device -- the case the UI falls back to
    // showing raw vid/pid for, since there's no name to show.
    porta_usb_status_event_t unknown = {};
    unknown.epoch_secs = 1789012400u;
    unknown.present = false;
    unknown.kind = 2;  // UsbCKind::Other
    unknown.vid = 0x1234;
    unknown.pid = 0x5678;

    n = porta_proto_encode_usb_status(&unknown, buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "usb_status detach event decodes");
    if (got.size() == 1) {
        porta_usb_status_event_t out;
        check(porta_proto_parse_usb_status(&got[0], &out), "usb_status detach event parses");
        check(!out.present, "present round-trips false (detach)");
        check(out.kind == 2, "unrecognized kind round-trips");
        check(out.vid == 0x1234 && out.pid == 0x5678, "unrecognized vid/pid round-trip");
    }
}

static void test_radio_state_event()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    porta_radio_state_event_t in = {};
    in.epoch_secs = 1789012345u;
    in.freq_hz = 14074300u;
    in.tx = 1;
    in.mode = 6;
    in.split = 1;
    size_t n = porta_proto_encode_radio_state(&in, buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "radio_state event decodes");
    if (got.size() == 1) {
        porta_radio_state_event_t out;
        check(porta_proto_parse_radio_state(&got[0], &out), "radio_state event parses");
        check(out.epoch_secs == 1789012345u, "radio_state epoch round-trips");
        check(out.freq_hz == 14074300u, "radio_state frequency round-trips");
        check(out.tx == 1 && out.mode == 6 && out.split == 1, "radio_state flags round-trip");
        porta_usb_status_event_t usb;
        check(!porta_proto_parse_usb_status(&got[0], &usb),
              "a radio_state event is not a usb_status event");
    }
    in.tx = 0;
    in.split = 0;
    n = porta_proto_encode_radio_state(&in, buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "radio_state RX event decodes");
    if (got.size() == 1) {
        porta_radio_state_event_t out;
        check(porta_proto_parse_radio_state(&got[0], &out) && !out.tx && !out.split,
              "radio_state RX with split off round-trips");
    }
}

static void test_activity_event()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    porta_activity_event_t in = {};
    in.epoch_secs = 1789012345u;
    in.slot_idx = 119267489u;
    in.block_ms = 160;
    in.first = 77;
    in.n = 3;
    in.lo_hz = 200;
    in.hi_hz = 2900;
    for (uint8_t i = 0; i < in.n; ++i) {
        in.level[i] = (uint8_t)(i * 40);
        for (unsigned j = 0; j < PORTA_ACTIVITY_MASK_BYTES; ++j) in.mask[i][j] = (uint8_t)(i * 16 + j);
    }
    size_t n = porta_proto_encode_activity(&in, buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "activity event decodes");
    if (got.size() == 1) {
        porta_activity_event_t out;
        check(porta_proto_parse_activity(&got[0], &out), "activity event parses");
        check(out.epoch_secs == 1789012345u && out.slot_idx == 119267489u &&
                  out.block_ms == 160 && out.first == 77 && out.n == 3 &&
                  out.lo_hz == 200 && out.hi_hz == 2900,
              "activity header round-trips");
        check(std::memcmp(out.level, in.level, 3) == 0, "activity levels round-trip");
        check(std::memcmp(out.mask, in.mask, 3 * PORTA_ACTIVITY_MASK_BYTES) == 0,
              "activity masks round-trip");
        porta_radio_state_event_t radio;
        check(!porta_proto_parse_radio_state(&got[0], &radio),
              "an activity event is not a radio_state event");
        char json[256];
        check(porta_event_json_activity(json, sizeof(json), 9, &out) > 0 &&
                  std::string(json) == "{\"s\":9,\"t\":1789012345,\"d\":8,\"si\":119267489,"
                                       "\"bm\":160,\"b\":77,\"lo\":200,\"hi\":2900,\"v\":[0,40,80],"
                                       "\"m\":[\"000102030405060708090a0b\",\"101112131415161718191a1b\","
                                       "\"202122232425262728292a2b\"]}",
              "activity row is the app's shape: levels, then each mask as hex in wire order");
    }

    // The largest batch, all 255s and every cell on: one frame, and a row the sidekick's row
    // buffer (send_event_row(), PORTA_EVENT_TEXT_MAX * 6 + 224) holds.
    in.n = PORTA_ACTIVITY_LEVELS_MAX;
    in.lo_hz = 65535;
    in.hi_hz = 65535;
    std::memset(in.level, 255, sizeof(in.level));
    std::memset(in.mask, 0xFF, sizeof(in.mask));
    in.epoch_secs = 4294967295u;
    in.slot_idx = 4294967295u;
    n = porta_proto_encode_activity(&in, buf, sizeof(buf));
    check(n > 0, "a full batch fits one frame");
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    porta_activity_event_t full;
    check(got.size() == 1 && porta_proto_parse_activity(&got[0], &full) &&
              full.n == PORTA_ACTIVITY_LEVELS_MAX &&
              full.mask[PORTA_ACTIVITY_LEVELS_MAX - 1][PORTA_ACTIVITY_MASK_BYTES - 1] == 0xFF,
          "a full batch round-trips");
    char row[PORTA_EVENT_TEXT_MAX * 6 + 224];
    check(got.size() == 1 && porta_event_json_activity(row, sizeof(row), 4294967295u, &full) > 0,
          "a full batch's row fits the sidekick's row buffer");

    in.n = 0;
    check(porta_proto_encode_activity(&in, buf, sizeof(buf)) == 0, "no blocks is not encoded");
    in.n = PORTA_ACTIVITY_LEVELS_MAX + 1;
    check(porta_proto_encode_activity(&in, buf, sizeof(buf)) == 0, "too many blocks is not encoded");

    // A count that disagrees with the frame's length is malformed.
    in.n = 3;
    n = porta_proto_encode_activity(&in, buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    if (got.size() == 1) {
        got[0].payload[11] = 4;
        porta_activity_event_t out;
        check(!porta_proto_parse_activity(&got[0], &out), "a count past the frame is refused");
    }
}

static void test_tx_hud_event()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    porta_tx_hud_event_t in = {};
    in.epoch_secs = 1789012345u;
    in.active = true;
    in.aborted = false;
    in.writes_blocked = false;
    in.power_dw = 42;   // 4.2 W
    in.swr_c = 135;     // 1.35
    in.battery_pct = 87;
    in.offset_hz = 1500;
    std::snprintf(in.text, sizeof(in.text), "CQ KB2SLO FN30");

    size_t n = porta_proto_encode_tx_hud(&in, buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "tx_hud event decodes");
    if (got.size() == 1) {
        porta_tx_hud_event_t out;
        check(porta_proto_parse_tx_hud(&got[0], &out), "tx_hud event parses");
        check(out.active && !out.aborted && !out.writes_blocked, "flags round-trip");
        check(out.power_dw == 42, "power_dw round-trips");
        check(out.swr_c == 135, "swr_c round-trips");
        check(out.battery_pct == 87, "battery_pct round-trips");
        check(out.offset_hz == 1500, "offset_hz round-trips");
        check(std::string(out.text) == "CQ KB2SLO FN30", "text round-trips");
        check(std::string(out.reason).empty(), "reason empty when not aborted");

        porta_slot_state_event_t ss;
        check(!porta_proto_parse_slot_state(&got[0], &ss),
              "a tx_hud event is not a slot_state event");
    }

    // -1 sentinels (unread power/SWR/battery) must survive the cast through
    // uint16_t/uint8_t on the wire and back, same as rst_sent/rst_rcvd's -99.
    // The abort reason must survive alongside the message text -- both are
    // real fields now, not one hardcoded client-side.
    porta_tx_hud_event_t unknown = {};
    unknown.active = false;
    unknown.aborted = true;
    unknown.writes_blocked = true;
    unknown.power_dw = -1;
    unknown.swr_c = -1;
    unknown.battery_pct = -1;
    std::snprintf(unknown.reason, sizeof(unknown.reason), "low battery");
    std::snprintf(unknown.text, sizeof(unknown.text), "CQ KB2SLO FN30");
    porta_decoder_init(&d);
    n = porta_proto_encode_tx_hud(&unknown, buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    if (got.size() == 1) {
        porta_tx_hud_event_t out;
        check(porta_proto_parse_tx_hud(&got[0], &out) &&
              !out.active && out.aborted && out.writes_blocked &&
              out.power_dw == -1 && out.swr_c == -1 && out.battery_pct == -1 &&
              out.offset_hz == 0 &&
              std::string(out.reason) == "low battery" &&
              std::string(out.text) == "CQ KB2SLO FN30",
              "unknown sentinels (offset_hz's is 0, not -1 -- it's unsigned), abort reason, and text round-trip together");
    }
}

// Not keyed: an active HUD with nothing on the air, its own flag so the page does not match a
// reason string. A normal HUD must not carry it.
static void test_tx_hud_not_keyed()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    porta_tx_hud_event_t in = {};
    in.active = true;
    in.not_keyed = true;
    in.power_dw = -1;
    in.swr_c = -1;
    in.battery_pct = -1;
    in.offset_hz = 1234;
    std::snprintf(in.reason, sizeof(in.reason), "CAT not ready");
    std::snprintf(in.text, sizeof(in.text), "CQ KB2SLO FN30");
    size_t n = porta_proto_encode_tx_hud(&in, buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "not-keyed tx_hud decodes");
    if (got.size() == 1) {
        porta_tx_hud_event_t out;
        check(porta_proto_parse_tx_hud(&got[0], &out) && out.active && out.not_keyed && !out.aborted &&
              out.offset_hz == 1234 && std::string(out.reason) == "CAT not ready" &&
              std::string(out.text) == "CQ KB2SLO FN30",
              "not_keyed round-trips with its reason and message");
        char json[256];
        check(porta_event_json_tx_hud(json, sizeof(json), 7, &out) > 0 &&
              std::string(json).find("\"nk\":1") != std::string::npos,
              "the browser event carries nk");
    }

    in.not_keyed = false;
    porta_decoder_init(&d);
    n = porta_proto_encode_tx_hud(&in, buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    if (got.size() == 1) {
        porta_tx_hud_event_t out;
        check(porta_proto_parse_tx_hud(&got[0], &out) && !out.not_keyed, "the flag is off unless set");
    }
}

static void test_action_radio_read()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];
    size_t n = porta_proto_encode_radio_read(buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1 && got[0].len == 1, "radio_read is the verb alone");
    if (got.size() == 1) {
        check(porta_proto_parse_radio_read(&got[0]), "radio_read parses");
        check(!porta_proto_parse_cq_call(&got[0]), "radio_read is not cq_call");
    }
    porta_decoder_init(&d);
    n = porta_proto_encode_cq_call(buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    if (got.size() == 1) {
        check(!porta_proto_parse_radio_read(&got[0]), "cq_call is not radio_read");
    }
}

// ACTION is the control direction, and the first thing to cross it is the
// clock. These pin the round trip and, more importantly, that a reply can be
// matched to its request by verb -- there is no sequence number, so a NAK that
// forgot which verb it was answering would be unattributable.
static void test_action_set_clock()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    size_t n = porta_proto_encode_set_clock(1789012345u, 750, buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "set_clock decodes");
    if (got.size() == 1) {
        uint32_t secs = 0; uint16_t ms = 0;
        check(porta_proto_parse_set_clock(&got[0], &secs, &ms), "set_clock parses");
        check(secs == 1789012345u, "epoch seconds round-trip");
        check(ms == 750, "milliseconds round-trip");
    }

    // Wrong length must be refused rather than read past the payload.
    porta_frame_t stub = {};
    stub.type = PORTA_MSG_ACTION;
    stub.len = 3;
    stub.payload[0] = PORTA_ACT_SET_CLOCK;
    uint32_t secs = 0;
    check(!porta_proto_parse_set_clock(&stub, &secs, nullptr), "a short set_clock is refused");
}

static void test_action_tx_cancel()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    // 0x02 was TX_FREE (I33); cancel remains verb-only at 0x03.
    size_t n = porta_proto_encode_tx_cancel(buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1 && got[0].len == 1, "tx_cancel is verb-only");
    if (got.size() == 1) {
        check(porta_proto_parse_tx_cancel(&got[0]), "tx_cancel parses");
        check(!porta_proto_parse_set_clock(&got[0], nullptr, nullptr), "tx_cancel is not set_clock");
    }
}

static void test_action_connect_and_tune()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    size_t n = porta_proto_encode_connect(buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1 && got[0].len == 1, "connect is verb-only");
    if (got.size() == 1) {
        check(porta_proto_parse_connect(&got[0]), "connect parses");
        check(!porta_proto_parse_tx_cancel(&got[0]), "connect is not cancel");
        bool on = true;
        check(!porta_proto_parse_tune(&got[0], &on), "connect is not tune");
    }

    porta_decoder_init(&d);
    n = porta_proto_encode_tune(true, buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1 && got[0].len == 2, "tune is verb + on");
    if (got.size() == 1) {
        bool on = false;
        check(porta_proto_parse_tune(&got[0], &on), "tune on parses");
        check(on, "tune on is true");
        check(!porta_proto_parse_connect(&got[0]), "tune is not connect");
    }

    porta_decoder_init(&d);
    n = porta_proto_encode_tune(false, buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "tune off frames");
    if (got.size() == 1) {
        bool on = true;
        check(porta_proto_parse_tune(&got[0], &on) && !on, "tune off is false");
    }
}

static void test_action_factory_reset()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    size_t n = porta_proto_encode_factory_reset(PORTA_FACTORY_RESET_LOGS, buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1 && got[0].len == 2, "factory reset is verb + flags");
    if (got.size() == 1) {
        uint8_t flags = 0;
        check(porta_proto_parse_factory_reset(&got[0], &flags), "factory reset parses");
        check(flags == PORTA_FACTORY_RESET_LOGS, "factory reset carries the logs flag");
        check(!porta_proto_parse_tune(&got[0], nullptr), "factory reset is not tune");
    }

    porta_decoder_init(&d);
    n = porta_proto_encode_factory_reset(0, buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    uint8_t flags = 0xFF;
    check(got.size() == 1 && porta_proto_parse_factory_reset(&got[0], &flags) && flags == 0,
          "factory reset without logs has no flags");

    // Verb-only (a malformed sender) and another verb do not parse.
    porta_decoder_init(&d);
    n = porta_proto_encode_connect(buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1 && !porta_proto_parse_factory_reset(&got[0], &flags),
          "connect is not factory reset");
}

static void test_action_cq_tries_parity()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    size_t n = porta_proto_encode_cq_call(buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1 && got[0].len == 1, "cq_call is the verb alone");
    if (got.size() == 1) {
        check(porta_proto_parse_cq_call(&got[0]), "cq_call parses");
        check(!porta_proto_parse_tx_cancel(&got[0]), "cq_call is not tx_cancel");
    }

    // The two share a shape, so each must refuse the other's verb.
    porta_decoder_init(&d);
    n = porta_proto_encode_queue_tries(4242, 99, buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1 && got[0].len == 4, "queue_tries is verb + u16 entry_id + u8 tries");
    if (got.size() == 1) {
        uint16_t id = 0;
        uint8_t tries = 0;
        check(porta_proto_parse_queue_tries(&got[0], &id, &tries) && id == 4242 && tries == 99,
              "queue_tries round-trips");
        check(!porta_proto_parse_queue_parity(&got[0], nullptr, nullptr),
              "queue_tries is not queue_parity");
        check(!porta_proto_parse_cq_call(&got[0]), "queue_tries is not cq_call");
    }

    porta_decoder_init(&d);
    n = porta_proto_encode_queue_parity(0xBEEF, 1, buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    if (got.size() == 1) {
        uint16_t id = 0;
        uint8_t parity = 0;
        check(porta_proto_parse_queue_parity(&got[0], &id, &parity) && id == 0xBEEF && parity == 1,
              "queue_parity round-trips");
        check(!porta_proto_parse_queue_tries(&got[0], nullptr, nullptr),
              "queue_parity is not queue_tries");
    }
}

static void test_action_queue_cancel_and_reply()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    size_t n = porta_proto_encode_queue_cancel(4242, buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1 && got[0].len == 3, "queue_cancel is verb + u16 entry_id");
    if (got.size() == 1) {
        uint16_t id = 0;
        check(porta_proto_parse_queue_cancel(&got[0], &id) && id == 4242,
              "queue_cancel entry_id round-trips");
        check(!porta_proto_parse_queue_tries(&got[0], nullptr, nullptr),
              "queue_cancel is not queue_tries");
    }

    porta_decoder_init(&d);
    n = porta_proto_encode_queue_reply(0xDEADBEEFu, buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1 && got[0].len == 5, "queue_reply is verb + u32 decode_id");
    if (got.size() == 1) {
        uint32_t id = 0;
        check(porta_proto_parse_queue_reply(&got[0], &id) && id == 0xDEADBEEFu,
              "queue_reply decode_id round-trips, including the high bit");
        check(!porta_proto_parse_queue_cancel(&got[0], nullptr), "queue_reply is not queue_cancel");
    }
}

static void test_action_file_delete()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    size_t n = porta_proto_encode_file_delete("", buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1 && got[0].len == 1, "file_delete clear-all is verb only");
    if (got.size() == 1) {
        char name[PORTA_FILENAME_MAX + 1] = {'x'};
        check(porta_proto_parse_file_delete(&got[0], name) && name[0] == '\0',
              "file_delete clear-all round-trips empty name");
    }

    porta_decoder_init(&d);
    n = porta_proto_encode_file_delete("20260918.adi", buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1 && got[0].len == 1 + 12, "file_delete carries basename");
    if (got.size() == 1) {
        char name[PORTA_FILENAME_MAX + 1] = {};
        check(porta_proto_parse_file_delete(&got[0], name) &&
              std::string(name) == "20260918.adi",
              "file_delete basename round-trips");
        check(!porta_proto_parse_queue_reply(&got[0], nullptr), "file_delete is not queue_reply");
    }

    check(porta_proto_encode_file_delete("no/slash.adi", buf, sizeof(buf)) == 0,
          "encode refuses a path slash");
    {
        porta_frame_t fake = {};
        fake.type = PORTA_MSG_ACTION;
        fake.len = 3;
        fake.payload[0] = PORTA_ACT_FILE_DELETE;
        fake.payload[1] = 'a';
        fake.payload[2] = '/';
        char name[PORTA_FILENAME_MAX + 1];
        check(!porta_proto_parse_file_delete(&fake, name), "parse rejects path slash");
    }
}

static void test_ack_and_nak()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    size_t n = porta_proto_encode_ack(PORTA_ACT_SET_CLOCK, buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "ack decodes");
    if (got.size() == 1) {
        uint8_t verb = 0;
        check(porta_proto_parse_ack(&got[0], &verb), "ack parses");
        check(verb == PORTA_ACT_SET_CLOCK, "ack carries the verb it answers");
        char reason[PORTA_EVENT_TEXT_MAX + 1] = {};
        check(!porta_proto_parse_nak(&got[0], nullptr, reason), "an ack is not a nak");
    }

    porta_decoder_init(&d);
    n = porta_proto_encode_nak(PORTA_ACT_SET_CLOCK, "clock is read-only", buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "nak decodes");
    if (got.size() == 1) {
        uint8_t verb = 0;
        char reason[PORTA_EVENT_TEXT_MAX + 1] = {};
        check(porta_proto_parse_nak(&got[0], &verb, reason), "nak parses");
        check(verb == PORTA_ACT_SET_CLOCK, "nak carries the verb it answers");
        check(std::string(reason) == "clock is read-only", "nak reason round-trips");
        check(!porta_proto_parse_ack(&got[0], nullptr), "a nak is not an ack");
    }

    // A reason is optional; the verb alone is a valid refusal.
    porta_decoder_init(&d);
    n = porta_proto_encode_nak(PORTA_ACT_SET_CLOCK, nullptr, buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1 && got[0].len == 1, "a reasonless nak is one byte");
}

static void test_config_kv()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    size_t n = porta_proto_encode_config_get("", buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "config get-all encodes");
    if (got.size() == 1) {
        char key[PORTA_CONFIG_KEY_MAX + 1] = {'x'};
        check(porta_proto_parse_config_get(&got[0], key), "get-all parses");
        check(key[0] == '\0', "get-all key is empty");
    }

    porta_decoder_init(&d);
    n = porta_proto_encode_config_set("call", "KB2SLO", buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "config set encodes");
    if (got.size() == 1) {
        char key[PORTA_CONFIG_KEY_MAX + 1] = {};
        char value[PORTA_CONFIG_VALUE_MAX + 1] = {};
        check(porta_proto_parse_config_set(&got[0], key, value), "set parses");
        check(std::string(key) == "call", "set key");
        check(std::string(value) == "KB2SLO", "set value");
        check(!porta_proto_parse_config_get(&got[0], key), "set is not get");
    }

    porta_decoder_init(&d);
    n = porta_proto_encode_config_value("grid", "FN20", buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "config value encodes");
    if (got.size() == 1) {
        char key[PORTA_CONFIG_KEY_MAX + 1] = {};
        char value[PORTA_CONFIG_VALUE_MAX + 1] = {};
        check(porta_proto_parse_config_value(&got[0], key, value), "value parses");
        check(std::string(key) == "grid" && std::string(value) == "FN20", "value kv");
    }

    check(porta_proto_encode_config_set("", "x", buf, sizeof(buf)) == 0, "set refuses empty key");
    check(porta_proto_encode_config_set("call", nullptr, buf, sizeof(buf)) == 0,
          "set refuses null value");
}

// FILE_LIST / FILE_READ page like components/qso_log/include/qso_browse.h's QsoBrowsePager
// (skip/take), and FILE_DATA carries one row per frame -- a NAME row for
// FILE_LIST, an ENTRY row for FILE_READ, discriminated by the same
// first-payload-byte pattern EVENT and ACTION already use.
static void test_file_list_and_read_requests()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    size_t n = porta_proto_encode_file_list_req(PORTA_FILE_LIST_QSO_DAILY, 3, 6, buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "file_list request encodes");
    if (got.size() == 1) {
        uint8_t kind = 0; uint16_t skip = 0; uint8_t take = 0;
        check(porta_proto_parse_file_list_req(&got[0], &kind, &skip, &take),
              "file_list request parses");
        check(kind == PORTA_FILE_LIST_QSO_DAILY, "kind round-trips");
        check(skip == 3 && take == 6, "skip/take round-trip");
    }

    porta_decoder_init(&d);
    n = porta_proto_encode_file_read_req("20260914.adi", 12, 6, buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "file_read request encodes");
    if (got.size() == 1) {
        char name[PORTA_FILENAME_MAX + 1] = {};
        uint16_t skip = 0; uint8_t take = 0;
        check(porta_proto_parse_file_read_req(&got[0], name, &skip, &take),
              "file_read request parses");
        check(std::string(name) == "20260914.adi", "filename round-trips");
        check(skip == 12 && take == 6, "file_read skip/take round-trip");
    }

    check(porta_proto_encode_file_read_req("", 0, 6, buf, sizeof(buf)) == 0,
          "file_read refuses an empty filename");
    check(porta_proto_encode_file_read_req(nullptr, 0, 6, buf, sizeof(buf)) == 0,
          "file_read refuses a null filename");
}

static void test_file_data_rows()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    size_t n = porta_proto_encode_file_name_row("20260914.adi", buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "file_name row encodes");
    if (got.size() == 1) {
        char name[PORTA_FILENAME_MAX + 1] = {};
        check(porta_proto_parse_file_name_row(&got[0], name), "file_name row parses");
        check(std::string(name) == "20260914.adi", "name round-trips");

        porta_qso_entry_row_t entry;
        check(!porta_proto_parse_file_entry_row(&got[0], &entry),
              "a name row is not an entry row");
    }

    porta_qso_entry_row_t in = {};
    std::snprintf(in.time_on, sizeof(in.time_on), "19:04");
    std::snprintf(in.band, sizeof(in.band), "20m");
    std::snprintf(in.call, sizeof(in.call), "W1ABC");
    in.rst_sent = 3;
    in.rst_rcvd = -11;
    std::snprintf(in.grid, sizeof(in.grid), "FN42");
    std::snprintf(in.freq, sizeof(in.freq), "14.074");
    std::snprintf(in.my_grid, sizeof(in.my_grid), "FN30");
    std::snprintf(in.comment, sizeof(in.comment), "nice sigs, tnx QSO");
    std::snprintf(in.mode, sizeof(in.mode), "FT8");
    std::snprintf(in.station_callsign, sizeof(in.station_callsign), "KB2SLO");
    std::snprintf(in.my_pota_ref, sizeof(in.my_pota_ref), "US-1234,US-5678");
    std::snprintf(in.my_sota_ref, sizeof(in.my_sota_ref), "W7O/CC-001");

    porta_decoder_init(&d);
    n = porta_proto_encode_file_entry_row(&in, buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "file_entry row encodes");
    if (got.size() == 1) {
        porta_qso_entry_row_t out;
        check(porta_proto_parse_file_entry_row(&got[0], &out), "file_entry row parses");
        check(std::string(out.time_on) == "19:04", "time_on round-trips");
        check(std::string(out.band) == "20m", "band round-trips");
        check(std::string(out.call) == "W1ABC", "call round-trips");
        check(out.rst_sent == 3 && out.rst_rcvd == -11, "rst values round-trip, including negative");
        check(std::string(out.grid) == "FN42", "grid round-trips");
        check(std::string(out.freq) == "14.074", "freq round-trips");
        check(std::string(out.my_grid) == "FN30", "my_grid round-trips");
        check(std::string(out.comment) == "nice sigs, tnx QSO", "comment round-trips");
        check(std::string(out.mode) == "FT8", "mode round-trips");
        check(std::string(out.station_callsign) == "KB2SLO", "station_callsign round-trips");
        check(std::string(out.my_pota_ref) == "US-1234,US-5678", "my_pota_ref round-trips");
        check(std::string(out.my_sota_ref) == "W7O/CC-001", "my_sota_ref round-trips");
    }

    // Empty grid/freq/my_grid/comment (not every QSO logs all of these, and
    // legacy .txt logs have none) must round-trip as empty, not crash or
    // desync the frame. mode/station_callsign are required ADIF fields in
    // practice but the wire format itself does not refuse an empty one.
    // my_pota_ref/my_sota_ref are empty on every non-activation QSO -- the
    // common case, not an edge case.
    porta_qso_entry_row_t no_grid = in;
    no_grid.grid[0] = '\0';
    no_grid.freq[0] = '\0';
    no_grid.my_grid[0] = '\0';
    no_grid.comment[0] = '\0';
    no_grid.mode[0] = '\0';
    no_grid.station_callsign[0] = '\0';
    no_grid.my_pota_ref[0] = '\0';
    no_grid.my_sota_ref[0] = '\0';
    porta_decoder_init(&d);
    n = porta_proto_encode_file_entry_row(&no_grid, buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    if (got.size() == 1) {
        porta_qso_entry_row_t out;
        check(porta_proto_parse_file_entry_row(&got[0], &out) &&
              std::string(out.grid).empty() && std::string(out.freq).empty() &&
              std::string(out.my_grid).empty() && std::string(out.comment).empty() &&
              std::string(out.mode).empty() && std::string(out.station_callsign).empty() &&
              std::string(out.my_pota_ref).empty() && std::string(out.my_sota_ref).empty(),
              "empty grid/freq/my_grid/comment/mode/station_callsign/my_pota_ref/my_sota_ref round-trip");
    }

    // No report uses -99, the same sentinel QsoContext::snr_tx/snr_rx already
    // use -- must survive round-trip like any other value, not be special-cased.
    porta_decoder_init(&d);
    in.rst_sent = -99;
    in.rst_rcvd = -99;
    n = porta_proto_encode_file_entry_row(&in, buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    if (got.size() == 1) {
        porta_qso_entry_row_t out;
        check(porta_proto_parse_file_entry_row(&got[0], &out) &&
              out.rst_sent == -99 && out.rst_rcvd == -99,
              "the no-report sentinel round-trips");
    }

    // qso_browse.cpp always yields "HH:MM" or "??:??" -- a caller passing
    // anything else is a programming error, not a wire condition, and must
    // be refused at encode rather than silently truncated or padded.
    porta_qso_entry_row_t bad_time = in;
    std::snprintf(bad_time.time_on, sizeof(bad_time.time_on), "1");
    check(porta_proto_encode_file_entry_row(&bad_time, buf, sizeof(buf)) == 0,
          "encode refuses a malformed time_on");
}

static void test_sweep_event()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    porta_sweep_event_t in = {};
    in.epoch_secs = 1789012345u;
    in.kind = PORTA_SWEEP_POINT;
    in.range_index = 3;
    in.freq_hz = 14225000u;
    in.swr_c = 138;
    size_t n = porta_proto_encode_sweep_event(&in, buf, sizeof(buf));
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "sweep point event decodes");
    if (got.size() == 1) {
        porta_sweep_event_t out;
        check(porta_proto_parse_sweep_event(&got[0], &out), "sweep point event parses");
        check(out.kind == PORTA_SWEEP_POINT && out.range_index == 3, "kind and range round-trip");
        check(out.freq_hz == 14225000u, "a frequency above 16 bits round-trips");
        check(out.swr_c == 138, "SWR round-trips");
        check(out.epoch_secs == 1789012345u, "time round-trips");
        porta_usb_status_event_t usb;
        check(!porta_proto_parse_usb_status(&got[0], &usb), "a sweep event is not a usb_status event");
    }

    // Events that carry no reading send -1, which has to survive the unsigned wire field.
    porta_sweep_event_t aborted = {};
    aborted.kind = PORTA_SWEEP_ABORTED;
    aborted.swr_c = -1;
    aborted.reason = PORTA_SWEEP_REASON_STALLED;
    n = porta_proto_encode_sweep_event(&aborted, buf, sizeof(buf));
    got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "sweep aborted event decodes");
    if (got.size() == 1) {
        porta_sweep_event_t out;
        check(porta_proto_parse_sweep_event(&got[0], &out), "sweep aborted event parses");
        check(out.kind == PORTA_SWEEP_ABORTED && out.reason == PORTA_SWEEP_REASON_STALLED, "reason round-trips");
        check(out.swr_c == -1, "-1 round-trips as -1");
    }

    porta_frame_t fake = {};
    fake.type = PORTA_MSG_EVENT;
    fake.payload[0] = PORTA_EVT_SWEEP;
    fake.len = 5;
    porta_sweep_event_t out;
    check(!porta_proto_parse_sweep_event(&fake, &out), "a short sweep event is refused");
    check(porta_proto_encode_sweep_event(nullptr, buf, sizeof(buf)) == 0, "encode refuses null");
}

static void test_action_sweep_start()
{
    porta_decoder_t d;
    porta_decoder_init(&d);
    uint8_t buf[PORTA_PROTO_MAX_FRAME];

    const porta_sweep_range_t in[] = {{7000, 7300, 20}, {14000, 14350, 25}, {28000, 29700, 100}};
    size_t n = porta_proto_encode_sweep_start(in, 3, buf, sizeof(buf));
    check(n > 0, "sweep start encodes");
    auto got = run(&d, std::vector<uint8_t>(buf, buf + n));
    check(got.size() == 1, "sweep start decodes");
    if (got.size() == 1) {
        porta_sweep_range_t out[PORTA_SWEEP_MAX_RANGES];
        size_t count = 0;
        check(porta_proto_parse_sweep_start(&got[0], out, &count), "sweep start parses");
        check(count == 3, "three ranges round-trip");
        check(count == 3 && out[0].lo_khz == 7000 && out[0].hi_khz == 7300 && out[0].step_khz == 20,
              "first range round-trips");
        check(count == 3 && out[2].lo_khz == 28000 && out[2].hi_khz == 29700 && out[2].step_khz == 100,
              "last range round-trips");
        check(!porta_proto_parse_tune(&got[0], nullptr), "a sweep start is not a tune");
    }

    porta_sweep_range_t twelve[PORTA_SWEEP_MAX_RANGES];
    for (auto& r : twelve) {
        r = {7000, 7010, 10};
    }
    check(porta_proto_encode_sweep_start(twelve, PORTA_SWEEP_MAX_RANGES, buf, sizeof(buf)) > 0,
          "the most ranges a frame holds encodes");
    check(porta_proto_encode_sweep_start(twelve, PORTA_SWEEP_MAX_RANGES + 1, buf, sizeof(buf)) == 0,
          "one more range is refused");
    check(porta_proto_encode_sweep_start(twelve, 0, buf, sizeof(buf)) == 0, "no ranges is refused");

    porta_frame_t fake = {};
    fake.type = PORTA_MSG_ACTION;
    fake.payload[0] = PORTA_ACT_SWEEP_START;
    porta_sweep_range_t out[PORTA_SWEEP_MAX_RANGES];
    size_t count = 0;
    fake.payload[1] = 2;
    fake.len = 2 + 6;  // claims two ranges, carries one
    check(!porta_proto_parse_sweep_start(&fake, out, &count), "a count that does not match the length is refused");
    fake.payload[1] = 0;
    fake.len = 2;
    check(!porta_proto_parse_sweep_start(&fake, out, &count), "a zero count is refused");
    fake.payload[1] = PORTA_SWEEP_MAX_RANGES + 1;
    fake.len = 2 + 6 * (PORTA_SWEEP_MAX_RANGES + 1);
    check(!porta_proto_parse_sweep_start(&fake, out, &count), "too many ranges are refused");
}

static void check_row(const std::string& got, const char* want, const char* what)
{
    if (got != want) {
        std::printf("FAIL: %s\n  got  %s\n  want %s\n", what, got.c_str(), want);
        g_fail++;
    }
}

// The rows GET /api/events sends. host_link.c and the scenario simulator
// both call this encoder, so a changed field fails here rather than only
// on a browser.
static void test_event_json()
{
    char row[1024];

    check(porta_event_json_log(row, sizeof(row), 8, 1700000005, "a\"b") > 0,
          "log row encodes");
    check_row(row, "{\"s\":8,\"t\":1700000005,\"d\":0,\"x\":\"a\\\"b\"}", "log row");

    porta_decode_event_t dec;
    std::memset(&dec, 0, sizeof(dec));
    dec.epoch_secs = 1700000000;
    dec.decode_id = 7;
    std::snprintf(dec.text, sizeof(dec.text), "CQ N6HAN CM87");
    dec.snr = -5;
    dec.offset_hz = 1200;
    dec.dt_centis = -15;
    dec.is_cq = true;
    dec.is_recent_qso = true;
    dec.slot_idx = 113333333;
    dec.slot_len_ds = 150;
    dec.dial_hz = 14074000;
    check(porta_event_json_decode(row, sizeof(row), 3, &dec) > 0, "decode row encodes");
    check_row(row,
              "{\"s\":3,\"t\":1700000000,\"d\":1,\"x\":\"CQ N6HAN CM87\","
              "\"id\":7,\"snr\":-5,\"hz\":1200,\"dt\":-0.15,\"cq\":1,\"me\":0,\"rq\":1,"
              "\"si\":113333333,\"sl\":150,\"fq\":14074000}",
              "decode row");

    porta_queue_entry_event_t q;
    std::memset(&q, 0, sizeof(q));
    q.epoch_secs = 1700000001;
    q.entry_id = 2;
    q.state = 1;
    q.tries_sent = 0;
    q.try_limit = 6;
    q.flags = PORTA_QUEUE_ODD;
    std::snprintf(q.dxcall, sizeof(q.dxcall), "N6HAN");
    check(porta_event_json_queue(row, sizeof(row), 4, &q) > 0, "queue row encodes");
    check_row(row,
              "{\"s\":4,\"t\":1700000001,\"d\":2,\"eid\":2,\"st\":1,\"ts\":0,\"tl\":6,"
              "\"p\":1,\"cu\":0,\"x\":\"N6HAN\"}",
              "queue row");

    porta_slot_state_event_t slot;
    std::memset(&slot, 0, sizeof(slot));
    slot.epoch_secs = 1700000002;
    slot.slot_parity = 1;
    slot.resolved_offset_hz = 1500;
    check(porta_event_json_slot(row, sizeof(row), 5, &slot) > 0, "slot row encodes");
    check_row(row, "{\"s\":5,\"t\":1700000002,\"d\":3,\"sp\":1,\"hz\":1500}", "slot row");

    porta_tx_hud_event_t hud;
    std::memset(&hud, 0, sizeof(hud));
    hud.epoch_secs = 1700000003;
    hud.active = true;
    hud.power_dw = -1;
    hud.swr_c = -1;
    hud.battery_pct = -1;
    hud.offset_hz = 1200;
    std::snprintf(hud.text, sizeof(hud.text), "N6HAN AG6AQ CM97");
    check(porta_event_json_tx_hud(row, sizeof(row), 6, &hud) > 0, "hud row encodes");
    check_row(row,
              "{\"s\":6,\"t\":1700000003,\"d\":4,\"active\":1,\"aborted\":0,\"wrblk\":0,\"nk\":0,"
              "\"pw\":-1,\"swr\":-1,\"batt\":-1,\"hz\":1200,\"reason\":\"\",\"x\":\"N6HAN AG6AQ CM97\"}",
              "hud row");

    porta_usb_status_event_t usb;
    std::memset(&usb, 0, sizeof(usb));
    usb.epoch_secs = 1700000004;
    usb.present = true;
    usb.kind = 0;
    usb.vid = 0x0483;
    usb.pid = 0xA34C;
    check(porta_event_json_usb(row, sizeof(row), 7, &usb) > 0, "usb row encodes");
    check_row(row,
              "{\"s\":7,\"t\":1700000004,\"d\":5,\"present\":1,\"kind\":0,\"vid\":1155,\"pid\":41804}",
              "usb row");

    porta_radio_state_event_t radio;
    std::memset(&radio, 0, sizeof(radio));
    radio.epoch_secs = 1700000004;
    radio.freq_hz = 14074300;
    radio.tx = 1;
    radio.mode = 6;
    check(porta_event_json_radio(row, sizeof(row), 10, &radio) > 0, "radio row encodes");
    check_row(row, "{\"s\":10,\"t\":1700000004,\"d\":7,\"hz\":14074300,\"tx\":1,\"md\":6,\"sp\":0}",
              "radio row");

    porta_sweep_event_t sweep;
    std::memset(&sweep, 0, sizeof(sweep));
    sweep.epoch_secs = 1700000005;
    sweep.kind = PORTA_SWEEP_POINT;
    sweep.range_index = 2;
    sweep.freq_hz = 14225000;
    sweep.swr_c = 138;
    check(porta_event_json_sweep(row, sizeof(row), 8, &sweep) > 0, "sweep row encodes");
    check_row(row,
              "{\"s\":8,\"t\":1700000005,\"d\":6,\"k\":0,\"r\":2,\"hz\":14225000,\"swr\":138,\"why\":0}",
              "sweep row");
    sweep.kind = PORTA_SWEEP_ABORTED;
    sweep.swr_c = -1;
    sweep.freq_hz = 0;
    sweep.reason = PORTA_SWEEP_REASON_CAT_LOST;
    check(porta_event_json_sweep(row, sizeof(row), 9, &sweep) > 0, "sweep abort row encodes");
    check_row(row,
              "{\"s\":9,\"t\":1700000005,\"d\":6,\"k\":3,\"r\":2,\"hz\":0,\"swr\":-1,\"why\":1}",
              "sweep abort row");

    check(porta_event_json_log(row, 8, 1, 1, "too long for this buffer") == 0,
          "a short buffer encodes nothing");
}

static porta_frame_t decode_one(const uint8_t* wire, size_t n, porta_decoder_t* d)
{
    porta_frame_t got;
    std::memset(&got, 0, sizeof(got));
    porta_decoder_init(d);
    for (size_t i = 0; i < n; ++i) {
        if (porta_decoder_push(d, wire[i], &got)) {
            return got;
        }
    }
    got.len = 0xFF;
    return got;
}

static void test_link_patterns()
{
    uint8_t payload[PORTA_PROTO_MAX_PAYLOAD];
    uint8_t again[PORTA_PROTO_MAX_PAYLOAD];

    porta_link_data_fill(payload, 0x12345678u, 0);
    check(payload[0] == 0 && payload[3] == 0, "seq 0 is little-endian in the first four bytes");
    bool all_sync = true;
    for (unsigned i = 4; i < PORTA_PROTO_MAX_PAYLOAD; ++i) {
        if (payload[i] != PORTA_PROTO_SYNC) {
            all_sync = false;
        }
    }
    check(all_sync, "seq 0 repeats the sync byte");

    porta_link_data_fill(payload, 1u, 1);
    check(payload[0] == 1 && payload[4] == 0x00, "seq 1 is the zero pattern after the sequence");
    porta_link_data_fill(payload, 1u, 2);
    check(payload[4] == 0xFF, "seq 2 is all ones");
    porta_link_data_fill(payload, 1u, 3);
    check(payload[4] == 0xAA && payload[5] == 0x55, "seq 3 alternates bits");
    porta_link_data_fill(payload, 0u, 4);
    bool flat = true;
    bool all_zero = true;
    for (unsigned i = 5; i < PORTA_PROTO_MAX_PAYLOAD; ++i) {
        if (payload[i] != payload[4]) {
            flat = false;
        }
        if (payload[i] != 0) {
            all_zero = false;
        }
    }
    check(!flat && !all_zero, "seq 4 is a pseudo-random pattern, including seed 0");
    porta_link_data_fill(again, 0u, 4);
    check(std::memcmp(payload, again, sizeof(payload)) == 0, "the same seed and seq fill the same bytes");
    porta_link_data_fill(again, 1u, 4);
    check(std::memcmp(payload, again, sizeof(payload)) != 0, "a different seed changes the pseudo-random frame");
    porta_link_data_fill(payload, 1u, 5);
    check(payload[4] == PORTA_PROTO_SYNC, "the five patterns cycle");
}

static void test_link_checker()
{
    const uint32_t seed = 0xA5A5u;
    porta_link_checker_t c;
    porta_link_checker_init(&c, seed);

    uint8_t payload[PORTA_PROTO_MAX_PAYLOAD];
    for (uint32_t seq = 0; seq < 10; ++seq) {
        porta_link_data_fill(payload, seed, seq);
        check(porta_link_checker_push(&c, payload, PORTA_PROTO_MAX_PAYLOAD), "a full payload is accepted");
    }
    check(c.frames_ok == 10 && c.drops == 0 && c.duplicates == 0 && c.mismatches == 0,
          "ten frames in order count as ten good frames");

    porta_link_checker_init(&c, seed);
    porta_link_data_fill(payload, seed, 0);
    porta_link_checker_push(&c, payload, PORTA_PROTO_MAX_PAYLOAD);
    porta_link_data_fill(payload, seed, 3);
    porta_link_checker_push(&c, payload, PORTA_PROTO_MAX_PAYLOAD);
    check(c.drops == 2 && c.frames_ok == 2 && c.next_seq == 4, "a gap counts the missing sequences as drops");

    porta_link_data_fill(payload, seed, 0);
    porta_link_checker_push(&c, payload, PORTA_PROTO_MAX_PAYLOAD);
    check(c.duplicates == 1 && c.next_seq == 4, "a sequence already passed is a duplicate");
    porta_link_data_fill(payload, seed, 4);
    porta_link_checker_push(&c, payload, PORTA_PROTO_MAX_PAYLOAD);
    check(c.frames_ok == 3 && c.next_seq == 5, "a duplicate does not stall the next sequence");

    porta_link_checker_init(&c, seed);
    porta_link_data_fill(payload, seed, 0);
    payload[10] ^= 0x01;
    porta_link_checker_push(&c, payload, PORTA_PROTO_MAX_PAYLOAD);
    porta_link_data_fill(payload, seed, 1);
    porta_link_checker_push(&c, payload, PORTA_PROTO_MAX_PAYLOAD);
    check(c.mismatches == 1 && c.frames_ok == 1 && c.next_seq == 2,
          "a wrong byte is a mismatch and the next sequence still counts");

    check(!porta_link_checker_push(&c, payload, 4), "a short payload is refused");
    check(c.frames_ok == 1, "a refused payload does not move the counters");
}

// The checker and the CRC count different failures. A payload the CRC accepts
// but that was not generated from the seed is a mismatch. A bit flipped on
// the wire never reaches the checker.
static void test_link_mismatch_is_not_a_crc_error()
{
    const uint32_t seed = 7;
    uint8_t wire[PORTA_PROTO_MAX_FRAME];
    size_t n = porta_proto_encode_link_data(seed, 0, wire, sizeof(wire));
    check(n == PORTA_PROTO_MAX_FRAME, "a stress frame is a max-length frame");
    wire[3 + 10] ^= 0x01;
    // Re-seal the CRC over the damaged payload so the decoder accepts it.
    wire[n - 1] = porta_proto_crc8(&wire[1], n - 2);

    porta_decoder_t d;
    porta_frame_t got = decode_one(wire, n, &d);
    check(d.crc_errors == 0 && d.frames_ok == 1, "a re-sealed payload passes the CRC");
    porta_link_checker_t c;
    porta_link_checker_init(&c, seed);
    check(porta_link_checker_push(&c, got.payload, got.len), "the checker sees the frame");
    check(c.mismatches == 1 && c.frames_ok == 0, "the checker counts the byte the CRC let through");

    n = porta_proto_encode_link_data(seed, 0, wire, sizeof(wire));
    wire[10] ^= 0x01;
    got = decode_one(wire, n, &d);
    check(d.crc_errors == 1 && d.frames_ok == 0, "a bit flipped on the wire is a CRC error");
    check(got.len == 0xFF, "a CRC error yields no frame for the checker");
}

static void test_link_control_frames()
{
    uint8_t wire[PORTA_PROTO_MAX_FRAME];
    porta_decoder_t d;

    size_t n = porta_proto_encode_link_start(0x01020304u, true, 10200u, wire, sizeof(wire));
    porta_frame_t got = decode_one(wire, n, &d);
    uint32_t seed = 0;
    bool tune = false;
    uint32_t bytes = 0;
    check(porta_proto_parse_link_start(&got, &seed, &tune, &bytes) && seed == 0x01020304u && tune &&
              bytes == 10200u,
          "start carries the seed, the tune flag, and the byte quota");
    check(porta_proto_encode_link_start(1u, false, 1000u, wire, sizeof(wire)) == 0,
          "encode refuses a byte quota that is not on the list");
    check(!porta_proto_parse_link_stop(&got), "start is not a stop");

    n = porta_proto_encode_link_set_rate(460800u, wire, sizeof(wire));
    got = decode_one(wire, n, &d);
    uint32_t baud = 0;
    check(porta_proto_parse_link_set_rate(&got, &baud) && baud == 460800u, "set-rate carries an allowed baud");
    check(porta_proto_encode_link_set_rate(12345u, wire, sizeof(wire)) == 0, "encode refuses a baud not on the list");

    uint8_t bad[5] = {PORTA_LINK_SET_RATE, 0x80, 0x25, 0x00, 0x00};  // 9600
    n = porta_proto_encode(PORTA_MSG_LINK_TEST, bad, 5, wire, sizeof(wire));
    got = decode_one(wire, n, &d);
    check(!porta_proto_parse_link_set_rate(&got, &baud), "parse refuses a baud not on the list");

    n = porta_proto_encode_link_stop(wire, sizeof(wire));
    got = decode_one(wire, n, &d);
    check(porta_proto_parse_link_stop(&got), "stop round-trips");
    check(porta_proto_encode_link_stop(wire, 2) == 0, "a short buffer encodes no control frame");

    check(porta_link_rate_at(0) == PORTA_BAUD, "the sweep starts at the resting rate");
    check(porta_link_rate_at(0) == 2500000u, "the resting rate is 2500000");
    check(porta_link_rate_count() == 8, "the sweep has eight candidate rates");
    check(porta_link_rate_at(1) == 115200u, "115200 stays on the list");
    check(porta_link_rate_at(6) == 2000000u, "2000000 is still on the list");
    check(porta_link_rate_at(7) == 4000000u, "the sweep ends at 4000000");
    bool all_allowed = true;
    for (unsigned i = 0; i < porta_link_rate_count(); ++i) {
        if (!porta_link_rate_allowed(porta_link_rate_at(i))) {
            all_allowed = false;
        }
    }
    check(all_allowed && !porta_link_rate_allowed(115201u), "only the sweep list is allowed");

    check(porta_link_baud_now(460800u, 5000, 5000 - 2999) == 460800u, "traffic inside the idle window holds the rate");
    check(porta_link_baud_now(460800u, 5000, 5000 - 3000) == PORTA_BAUD, "three seconds without traffic reverts");
    check(porta_link_baud_now(PORTA_BAUD, 99999, 0) == PORTA_BAUD, "the resting rate stays");
    check(porta_link_baud_now(9600u, 10, 10) == PORTA_BAUD, "a baud that is not allowed reverts");
    check(porta_link_baud_now(460800u, 0x10u, 0xFFFFFFF0u) == 460800u, "the idle window handles a timer wrap");
    check(porta_link_baud_now(460800u, 0x1000u, 0xFFFFF000u) == PORTA_BAUD, "a wrapped gap past the window reverts");

    porta_link_status_t st;
    std::memset(&st, 0, sizeof(st));
    st.baud = 460800;
    st.frames_ok = 3;
    st.tx_bytes = 765;
    st.running = true;
    n = porta_proto_encode_link_result(&st, wire, sizeof(wire));
    got = decode_one(wire, n, &d);
    porta_link_status_t back;
    check(porta_proto_parse_link_result(&got, &back) && back.baud == 460800 && back.frames_ok == 3 &&
              back.tx_bytes == 765 && back.running && !back.failed,
          "a result frame round-trips the counters");
}

// Two ends exchange whatever porta_link_run_next produces, including a baud
// switch. `now` advances by step_ms each round.
static void exchange(porta_link_run_t* coord, porta_link_run_t* peer, uint32_t* now,
                     uint32_t step_ms, int rounds)
{
    for (int i = 0; i < rounds; ++i) {
        uint8_t wire[PORTA_PROTO_MAX_FRAME];
        uint32_t apply = 0;
        size_t n = porta_link_run_next(coord, *now, 0, wire, sizeof(wire), &apply);
        if (n > 0) {
            porta_decoder_t d;
            porta_frame_t got = decode_one(wire, n, &d);
            uint32_t sw = 0;
            porta_link_run_on_frame(peer, &got, *now, 0, &sw);
            if (sw != 0) {
                porta_link_run_applied(peer, *now);
            }
        }
        if (apply != 0) {
            porta_link_run_applied(coord, *now);
        }
        apply = 0;
        n = porta_link_run_next(peer, *now, 0, wire, sizeof(wire), &apply);
        if (n > 0) {
            porta_decoder_t d;
            porta_frame_t got = decode_one(wire, n, &d);
            uint32_t sw = 0;
            porta_link_run_on_frame(coord, &got, *now, 0, &sw);
        }
        *now += step_ms;
    }
}

static void test_link_run()
{
    const uint32_t pass = 10200u;
    unsigned target = 0;
    for (unsigned i = 0; i < porta_link_rate_count(); ++i) {
        if (porta_link_rate_at(i) == 460800u) {
            target = i;
        }
    }
    porta_link_run_t coord;
    porta_link_run_t peer;
    porta_link_run_init(&peer);
    porta_link_run_begin(&coord, 0x42u, pass, true, false, target, 0);
    uint32_t now = 0;
    for (int i = 0; i < 400 && coord.baud == PORTA_BAUD; ++i) {
        exchange(&coord, &peer, &now, 10, 1);
    }
    check(porta_link_run_active(&peer), "start wakes the peer");
    check(coord.baud == 460800u && coord.tx_bytes == 0,
          "the coordinator switches to the selected rate before the quota");
    for (int i = 0; i < 800 && porta_link_run_active(&coord); ++i) {
        exchange(&coord, &peer, &now, 10, 1);
    }
    check(!porta_link_run_active(&coord) && coord.baud == 460800u,
          "a clean quota finishes at the selected rate and does not climb");

    char note[PORTA_EVENT_TEXT_MAX + 1];
    check(porta_link_run_take_note(&coord, note, sizeof(note)) > 0, "a finished rate leaves a log line");

    // Index 0 is the resting rate, so that choice never switches.
    porta_link_run_init(&peer);
    porta_link_run_begin(&coord, 0x42u, pass, true, false, 0, 0);
    now = 0;
    for (int i = 0; i < 400 && porta_link_run_active(&coord); ++i) {
        exchange(&coord, &peer, &now, 10, 1);
    }
    check(!porta_link_run_active(&coord) && coord.baud == PORTA_BAUD,
          "the resting rate runs there and finishes there");
    check(!porta_link_run_active(&peer), "the peer is finished after stop");

    // The peer must accept another start after it has finished. Otherwise the
    // next Run sends a full quota and receives nothing.
    porta_link_run_begin(&coord, 0x99u, pass, true, false, 0, now);
    uint8_t again[PORTA_PROTO_MAX_FRAME];
    uint32_t again_apply = 0;
    size_t again_n = porta_link_run_next(&coord, now, 0, again, sizeof(again), &again_apply);
    porta_decoder_t again_dec;
    porta_frame_t again_got = decode_one(again, again_n, &again_dec);
    uint32_t again_sw = 0;
    porta_link_run_on_frame(&peer, &again_got, now, 0, &again_sw);
    check(porta_link_run_active(&peer), "a later start wakes the peer again");

    // A corrupted payload fails the rate and stops the run.
    porta_link_run_init(&peer);
    porta_link_run_begin(&coord, 0x42u, pass, true, true, 0, 0);
    now = 0;
    uint8_t wire[PORTA_PROTO_MAX_FRAME];
    uint32_t apply = 0;
    size_t n = porta_link_run_next(&coord, now, 0, wire, sizeof(wire), &apply);
    porta_decoder_t d;
    porta_frame_t got = decode_one(wire, n, &d);
    uint32_t sw = 0;
    porta_link_run_on_frame(&peer, &got, now, 0, &sw);
    n = porta_link_run_next(&coord, now, 0, wire, sizeof(wire), &apply);
    wire[3 + 10] ^= 0x01u;
    wire[n - 1] = porta_proto_crc8(&wire[1], n - 2);
    got = decode_one(wire, n, &d);
    porta_link_run_on_frame(&peer, &got, now, 0, &sw);
    n = porta_link_run_next(&coord, now, 0, wire, sizeof(wire), &apply);
    got = decode_one(wire, n, &d);
    porta_link_run_on_frame(&peer, &got, now, 0, &sw);
    // Peer has sent nothing. Send until this end's quota is met, then the idle wait.
    for (int i = 0; i < 80 && coord.tx_bytes < pass; ++i) {
        n = porta_link_run_next(&coord, now, 0, wire, sizeof(wire), &apply);
        now += 10;
    }
    now += PORTA_LINK_IDLE_MS;
    n = porta_link_run_next(&coord, now, 0, wire, sizeof(wire), &apply);
    got = decode_one(wire, n, &d);
    check(porta_proto_parse_link_stop(&got), "a bad payload ends the run with stop");
    check(!porta_link_run_active(&coord), "the coordinator is done after a failure");

    porta_decoder_init(&d);
    porta_decoder_push(&d, PORTA_PROTO_SYNC, &got);
    porta_decoder_push(&d, 0x01, &got);
    porta_decoder_resync(&d);
    auto recovered = run(&d, encode(PORTA_MSG_POLL, {}));
    check(d.frames_ok == 1 && recovered.size() == 1, "resync drops a partial frame and keeps going");
}

int main()
{
    test_round_trip();
    test_empty_and_max_payload();
    test_sync_byte_inside_payload();
    test_crc_catches_every_single_bit_flip();
    test_truncated_frame_then_recovery();
    test_garbage_before_a_frame();
    test_false_sync_eats_one_frame_then_recovers();
    test_encode_refuses_a_short_buffer();
    test_counters();
    test_hello_round_trip();
    test_hello_rejects_wrong_shape();
    test_log_event();
    test_unset_clock_round_trips();
    test_decode_event();
    test_queue_entry_event();
    test_slot_state_event();
    test_usb_status_event();
    test_radio_state_event();
    test_activity_event();
    test_sweep_event();
    test_tx_hud_event();
    test_action_set_clock();
    test_action_tx_cancel();
    test_action_connect_and_tune();
    test_action_factory_reset();
    test_action_cq_tries_parity();
    test_tx_hud_not_keyed();
    test_action_radio_read();
    test_action_queue_cancel_and_reply();
    test_action_file_delete();
    test_action_sweep_start();
    test_ack_and_nak();
    test_config_kv();
    test_file_list_and_read_requests();
    test_file_data_rows();
    test_event_json();
    test_link_patterns();
    test_link_checker();
    test_link_mismatch_is_not_a_crc_error();
    test_link_control_frames();
    test_link_run();

    if (g_fail) {
        std::printf("FAILED: %d check(s)\n", g_fail);
        return 1;
    }
    std::printf("PASS: porta protocol codec\n");
    return 0;
}
