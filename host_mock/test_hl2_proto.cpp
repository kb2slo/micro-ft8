// hl2_proto: the HL2's Protocol 1 wire format (components/hl2_proto/include/hl2_proto.h), held to golden frames
// from the operator's HL2 on gateware 74.2 (hl2_golden/; RFC 0014 §2.5, S1).

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "hl2_proto.h"

static int g_fails = 0;

static void expect(bool ok, const char* msg)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    }
}

// A golden file: hex bytes separated by white space; a line starting with '#' is a comment.
static std::vector<uint8_t> load_hex(const char* path)
{
    std::vector<uint8_t> bytes;
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "FAIL: cannot open %s\n", path);
        ++g_fails;
        return bytes;
    }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') {
            continue;
        }
        const char* p = line;
        unsigned v;
        int n;
        while (sscanf(p, "%2x%n", &v, &n) == 1) {
            bytes.push_back(static_cast<uint8_t>(v));
            p += n;
            while (*p == ' ') {
                ++p;
            }
        }
    }
    fclose(f);
    return bytes;
}

static void test_discovery()
{
    uint8_t req[HL2_DISCOVERY_REQUEST_LEN + 1];
    expect(hl2_encode_discovery_request(req, sizeof req) == 63, "the discovery request is 63 bytes");
    expect(req[0] == 0xEF && req[1] == 0xFE && req[2] == 0x02, "and starts EF FE 02");
    bool zeros = true;
    for (size_t i = 3; i < 63; ++i) {
        zeros = zeros && req[i] == 0;
    }
    expect(zeros, "then 60 zeros");
    expect(hl2_encode_discovery_request(req, 62) == 0, "a short buffer is refused");

    std::vector<uint8_t> a = load_hex("hl2_golden/discovery_reply_2026-10-30.hex");
    Hl2DiscoveryReply r{};
    expect(a.size() == 60 && hl2_parse_discovery_reply(a.data(), a.size(), &r), "the 2026-10-30 reply parses");
    expect(!r.running, "stopped");
    expect(r.gateware_major == 74 && r.gateware_minor == 2, "gateware 74.2");
    expect(r.board_id == HL2_BOARD_ID && r.board == 5 && r.receivers == 4, "an HL2, BOARD 5, 4 receivers");
    expect(r.static_ip[0] == 0 && r.static_ip[3] == 0, "no static IP stored");
    expect(r.temperature_code == 0x3D3 && std::fabs(hl2_temperature_c(r.temperature_code) - 27.92f) < 0.01f,
           "temperature 27.9 C (SparkSDR showed 27.8 the same hour)");
    expect(r.forward_code == 10 && r.reverse_code == 11, "forward 10, reverse 11 at rest");
    expect(r.response_control == 0x03, "the clip count had saturated after streaming with an antenna");
    expect(r.tx_buffer_latency_ms == 10 && r.ptt_hang_ms == 4, "the power-on transmit settings");

    std::vector<uint8_t> b = load_hex("hl2_golden/discovery_reply_2026-11-03.hex");
    expect(b.size() == 60 && hl2_parse_discovery_reply(b.data(), b.size(), &r), "the 2026-11-03 reply parses");
    expect(r.response_control == 0 && r.temperature_code == 0x3B4 && r.reverse_code == 12,
           "no clip just after power-on; 25.5 C");

    expect(!hl2_parse_discovery_reply(b.data(), 59, &r), "a short reply is refused");
    std::vector<uint8_t> c = b;
    c[10] = 0x01;
    expect(!hl2_parse_discovery_reply(c.data(), c.size(), &r), "a board that is not an HL2 is refused");
    c = b;
    c[2] = 0x04;
    expect(!hl2_parse_discovery_reply(c.data(), c.size(), &r), "an unknown status is refused");
}

static void test_start_stop()
{
    uint8_t p[HL2_START_STOP_LEN];
    expect(hl2_encode_start(p, sizeof p) == 64 && p[2] == 0x04 && p[3] == 0x01 && p[63] == 0, "start: EF FE 04 01");
    expect(hl2_encode_stop(p, sizeof p) == 64 && p[2] == 0x04 && p[3] == 0x00, "stop: EF FE 04 00");
    expect(hl2_encode_start(p, 63) == 0, "a short buffer is refused");
}

static bool frame_is_receive_only(const uint8_t* frame)
{
    if ((frame[3] & 0x01) != 0) {
        return false;  // MOX
    }
    for (size_t i = 8; i < HL2_FRAME_LEN; ++i) {
        if (frame[i] != 0) {
            return false;  // a transmit sample
        }
    }
    return true;
}

static void test_control_packet()
{
    Hl2Register cfg{}, nco{}, lna{};
    expect(hl2_config_register(Hl2RxRate::Rate48k, 1, &cfg), "one receiver at 48 kHz");
    expect(hl2_nco_register(0, 14074000, &nco), "receiver 1 at 14.074 MHz");

    std::vector<uint8_t> golden = load_hex("hl2_golden/ep2_rx_config_seq0.hex");
    uint8_t pkt[HL2_PACKET_LEN];
    expect(hl2_encode_control_packet(0, cfg, nco, pkt, sizeof pkt) == HL2_PACKET_LEN, "the packet encodes");
    expect(golden.size() == HL2_PACKET_LEN && memcmp(pkt, golden.data(), HL2_PACKET_LEN) == 0,
           "byte for byte the packet the HL2 streamed on");
    expect(frame_is_receive_only(pkt + 8) && frame_is_receive_only(pkt + 8 + HL2_FRAME_LEN),
           "MOX clear and transmit samples zero in both frames");

    expect(hl2_config_register(Hl2RxRate::Rate48k, 4, &cfg) && cfg.value == ((3u << 3) | (1u << 2)),
           "four receivers: last receiver 3, duplex");
    expect(hl2_n2adr_receive_filters(7074000) == 0x44, "40 m: the 60/40 m low-pass and the receive high-pass");
    expect(hl2_n2adr_receive_filters(14074000) == 0x48 && hl2_n2adr_receive_filters(28074000) == 0x60,
           "20 m and 10 m: their low-passes and the high-pass");
    expect(hl2_n2adr_receive_filters(3573000) == 0x02 && hl2_n2adr_receive_filters(1840000) == 0x01,
           "80 m and 160 m: low-pass only, no high-pass");
    expect(hl2_config_register(Hl2RxRate::Rate48k, 1, &cfg, 0x44) && ((cfg.value >> 17) & 0x7F) == 0x44 &&
               !(cfg.value & (1u << 16)),
           "the filter byte lands in [23:17], clear of PA enable [16]");
    {
        uint8_t pkt2[HL2_PACKET_LEN];
        Hl2Register n{};
        hl2_nco_register(0, 7074000, &n);
        expect(hl2_encode_control_packet(2, cfg, n, pkt2, sizeof pkt2) == HL2_PACKET_LEN,
               "a config with filters is a receive register");
    }
    expect(hl2_config_register(Hl2RxRate::Rate384k, 1, &cfg) && (cfg.value >> 24) == 3, "384 kHz is rate 3");
    expect(!hl2_config_register(Hl2RxRate::Rate48k, 0, &cfg) && !hl2_config_register(Hl2RxRate::Rate48k, 5, &cfg),
           "0 or 5 receivers are refused");
    expect(hl2_nco_register(3, 7074000, &nco) && nco.address == 0x05, "receiver 4's NCO is register 0x05");
    expect(!hl2_nco_register(4, 7074000, &nco), "a fifth receiver is refused");
    expect(!hl2_nco_register(0, 38400001, &nco), "above 38.4 MHz is refused");
    expect(hl2_lna_register(-12, &lna) && lna.value == 0x40, "LNA -12 dB is direct-mode code 0");
    expect(hl2_lna_register(48, &lna) && lna.value == 0x40 + 60, "LNA +48 dB is code 60");
    expect(!hl2_lna_register(49, &lna) && !hl2_lna_register(-13, &lna), "outside -12..+48 dB is refused");

    // Receive only: nothing that keys or drives the radio can be encoded.
    hl2_config_register(Hl2RxRate::Rate48k, 1, &cfg);
    const Hl2Register refused[] = {
        {0x00, cfg.value | (1u << 16)},  // register 0x00 with PA enable
        {0x01, 14074000},                // TX NCO
        {0x09, 0xF0000000},              // drive level
        {0x17, 31u << 8},                // PTT hang 31, no auto-unkey
        {0x39, 1u << 27},                // watchdog disable
        {0x06, 7074000},                 // receiver 5's NCO
    };
    for (const Hl2Register& r : refused) {
        expect(hl2_encode_control_packet(1, r, cfg, pkt, sizeof pkt) == 0, "a non-receive register is refused");
        expect(hl2_encode_control_packet(1, cfg, r, pkt, sizeof pkt) == 0, "in either frame");
    }
    expect(hl2_encode_control_packet(1, cfg, cfg, pkt, HL2_PACKET_LEN - 1) == 0, "a short buffer is refused");
}

static void test_rx_packet()
{
    std::vector<uint8_t> p = load_hex("hl2_golden/ep6_1rx_48k_2026-11-03.hex");
    expect(p.size() == HL2_PACKET_LEN, "the golden receive packet is 1032 bytes");
    if (p.size() != HL2_PACKET_LEN) {
        return;
    }
    expect(hl2_samples_per_frame(1) == 63 && hl2_samples_per_frame(4) == 19, "63 samples a frame for 1, 19 for 4");
    expect(hl2_samples_per_frame(0) == 0 && hl2_samples_per_frame(5) == 0, "no receiver count outside 1..4");

    Hl2RxPacket pkt{};
    int32_t iq[2 * 4 * 2 * 63];
    expect(hl2_parse_rx_packet(p.data(), p.size(), 1, &pkt, iq, 252), "it parses for one receiver");
    expect(pkt.sequence == 100 && pkt.samples == 126, "sequence 100, 126 samples");
    expect(iq[0] == 72 && iq[1] == -106 && iq[2] == 29 && iq[3] == -28, "the first samples, sign-extended");
    expect(iq[124] == -11 && iq[125] == -82, "frame 1's last sample");
    expect(iq[126] == 1 && iq[127] == -7 && iq[250] == 22 && iq[251] == -34, "frame 2's first and last");

    expect(!pkt.status[0].command_response && pkt.status[0].address == 3 && !pkt.status[0].ptt,
           "frame 1 carries status address 3");
    expect(pkt.status[1].address == 0 && !pkt.status[1].ptt, "frame 2 carries address 0, PTT off");
    Hl2Telemetry t{};
    hl2_apply_status(pkt.status[0], &t);
    hl2_apply_status(pkt.status[1], &t);
    expect(t.seen == 0x09, "addresses 0 and 3 seen");
    expect(!t.adc_clipped && !t.tx_inhibited && t.gateware_major == 74, "no clip, not inhibited, gateware 74");
    expect(t.tx_fifo_recovery && t.tx_fifo_count == 0, "C3 0x80: the TX FIFO recovery flag set, count 0");

    Hl2Status s1{false, false, 1, {0x03, 0xCF, 0x00, 0x0A}};  // address 1 from the same capture
    Hl2Status s2{false, false, 2, {0x00, 0x0C, 0x00, 0x00}};
    hl2_apply_status(s1, &t);
    hl2_apply_status(s2, &t);
    expect(t.temperature_code == 0x3CF && t.forward_code == 10 && t.reverse_code == 12 && t.bias_code == 0,
           "addresses 1 and 2: temperature, forward, reverse, bias");
    Hl2Status cmd{true, true, 0x3A, {0xFF, 0xFF, 0xFF, 0xFF}};
    hl2_apply_status(cmd, &t);
    expect(t.ptt && t.reverse_code == 12, "a command response changes only PTT");

    expect(hl2_parse_rx_packet(p.data(), p.size(), 4, &pkt, iq, sizeof iq / sizeof iq[0]) && pkt.samples == 38,
           "four receivers: 38 samples each");
    expect(!hl2_parse_rx_packet(p.data(), p.size(), 1, &pkt, iq, 251), "too small an I/Q buffer is refused");
    expect(!hl2_parse_rx_packet(p.data(), p.size() - 1, 1, &pkt, iq, 252), "a short packet is refused");
    std::vector<uint8_t> bad = p;
    bad[3] = 0x02;
    expect(!hl2_parse_rx_packet(bad.data(), bad.size(), 1, &pkt, iq, 252), "a downstream endpoint is refused");
    bad = p;
    bad[8 + HL2_FRAME_LEN + 1] = 0x00;
    expect(!hl2_parse_rx_packet(bad.data(), bad.size(), 1, &pkt, iq, 252), "a lost frame sync is refused");
}

static void test_sequence()
{
    Hl2SequenceTracker t{};
    expect(hl2_sequence_note(&t, 500) == 0, "the first packet sets the start");
    expect(hl2_sequence_note(&t, 501) == 0 && t.gaps == 0, "the next one is no gap");
    expect(hl2_sequence_note(&t, 505) == 3 && t.gaps == 1 && t.lost == 3, "502-504 lost: one gap of 3");
    expect(hl2_sequence_note(&t, 503) == 0 && t.out_of_order == 1 && t.lost == 3, "a late 503 is out of order");
    expect(hl2_sequence_note(&t, 505) == 0 && t.out_of_order == 2, "a duplicate is out of order");
    expect(hl2_sequence_note(&t, 506) == 0 && t.gaps == 1, "and the stream goes on");

    Hl2SequenceTracker w{};
    hl2_sequence_note(&w, 0xFFFFFFFEu);
    expect(hl2_sequence_note(&w, 0xFFFFFFFFu) == 0, "up to the top");
    expect(hl2_sequence_note(&w, 0) == 0 && w.gaps == 0, "wraps to 0 with no gap");
    expect(hl2_sequence_note(&w, 2) == 1 && w.lost == 1, "a gap just past the wrap");
}

static void test_link_local()
{
    const uint8_t hl2_mac[6] = {0x00, 0x1c, 0xc0, 0xa2, 0x13, 0xdd};
    uint8_t ip[4];
    hl2_link_local_ipv4(hl2_mac, false, ip);
    expect(ip[0] == 169 && ip[1] == 254 && ip[2] == 19 && ip[3] == 221,
           "the HL2's own: 169.254.19.221, as it answered on 2026-11-03");
    const uint8_t low[6] = {1, 2, 3, 4, 0x00, 0x07};
    hl2_link_local_ipv4(low, false, ip);
    expect(ip[2] == 0, "the HL2 does not avoid 169.254.0.x");
    hl2_link_local_ipv4(low, true, ip);
    expect(ip[2] == 1 && ip[3] == 7, "our boards do: 169.254.1.7");
    const uint8_t high[6] = {1, 2, 3, 4, 0xFF, 0x07};
    hl2_link_local_ipv4(high, true, ip);
    expect(ip[2] == 254, "and 169.254.255.x becomes 169.254.254.x");
}

int main()
{
    test_discovery();
    test_start_stop();
    test_control_packet();
    test_rx_packet();
    test_sequence();
    test_link_local();
    if (g_fails) {
        fprintf(stderr, "test_hl2_proto: %d failure(s)\n", g_fails);
        return 1;
    }
    printf("test_hl2_proto: ok\n");
    return 0;
}
