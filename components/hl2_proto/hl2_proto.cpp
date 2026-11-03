// hl2_proto.cpp — the HL2's Protocol 1 wire format (hl2_proto.h).

#include <cstring>

#include "hl2_proto.h"

namespace {

constexpr uint8_t kMagic0 = 0xEF;
constexpr uint8_t kMagic1 = 0xFE;
constexpr uint8_t kTypeData = 0x01;
constexpr uint8_t kTypeDiscovery = 0x02;
constexpr uint8_t kTypeStartStop = 0x04;
constexpr uint8_t kEndpointDownstream = 0x02;
constexpr uint8_t kEndpointUpstream = 0x06;
constexpr size_t kFrameHeaderLen = 8;  // 7F 7F 7F C0 C1 C2 C3 C4
constexpr size_t kFrameSampleBytes = HL2_FRAME_LEN - kFrameHeaderLen;
constexpr uint32_t kMaxNcoHz = 38400000;
constexpr uint32_t kPaEnableBit = 1u << 16;

constexpr uint8_t kRegConfig = 0x00;
constexpr uint8_t kRegNcoFirst = 0x02;
constexpr uint8_t kRegNcoLast = kRegNcoFirst + HL2_MAX_RECEIVERS - 1;
constexpr uint8_t kRegLna = 0x0a;

uint16_t be16(const uint8_t* p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t be32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

void put_be32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

int32_t be24_signed(const uint8_t* p)
{
    uint32_t v = (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
    if (v & 0x800000u) {
        v |= 0xFF000000u;
    }
    return static_cast<int32_t>(v);
}

// The receive registers are the only writes S1 can make (hl2_proto.h, RECEIVE ONLY).
bool is_receive_register(const Hl2Register& r)
{
    if (r.address == kRegConfig) {
        return (r.value & kPaEnableBit) == 0;
    }
    if (r.address >= kRegNcoFirst && r.address <= kRegNcoLast) {
        return r.value <= kMaxNcoHz;
    }
    return r.address == kRegLna;
}

void put_frame(uint8_t* frame, const Hl2Register& r)
{
    frame[0] = 0x7F;
    frame[1] = 0x7F;
    frame[2] = 0x7F;
    frame[3] = static_cast<uint8_t>((r.address & 0x3F) << 1);  // C0[0], MOX, stays clear
    put_be32(frame + 4, r.value);
    memset(frame + kFrameHeaderLen, 0, kFrameSampleBytes);   // L/R and transmit I/Q all zero
}

size_t encode_start_stop(uint8_t command, uint8_t* out, size_t cap)
{
    if (cap < HL2_START_STOP_LEN) {
        return 0;
    }
    memset(out, 0, HL2_START_STOP_LEN);
    out[0] = kMagic0;
    out[1] = kMagic1;
    out[2] = kTypeStartStop;
    out[3] = command;
    return HL2_START_STOP_LEN;
}

}  // namespace

size_t hl2_encode_discovery_request(uint8_t* out, size_t cap)
{
    if (cap < HL2_DISCOVERY_REQUEST_LEN) {
        return 0;
    }
    memset(out, 0, HL2_DISCOVERY_REQUEST_LEN);
    out[0] = kMagic0;
    out[1] = kMagic1;
    out[2] = kTypeDiscovery;
    return HL2_DISCOVERY_REQUEST_LEN;
}

bool hl2_parse_discovery_reply(const uint8_t* d, size_t len, Hl2DiscoveryReply* out)
{
    if (len != HL2_DISCOVERY_REPLY_LEN || d[0] != kMagic0 || d[1] != kMagic1 || d[10] != HL2_BOARD_ID) {
        return false;
    }
    switch (d[2]) {
        case 0x02:
        case 0x03:
            break;
        default:
            return false;
    }
    out->running = d[2] == 0x03;
    memcpy(out->mac, d + 3, sizeof out->mac);
    out->gateware_major = d[9];
    out->board_id = d[10];
    memcpy(out->static_ip, d + 13, sizeof out->static_ip);
    out->receivers = d[19];
    out->board = d[20] & 0x3F;
    out->gateware_minor = d[21];
    out->response_control = d[27];
    out->temperature_code = be16(d + 28) & 0x0FFF;
    out->forward_code = be16(d + 30) & 0x0FFF;
    out->reverse_code = be16(d + 32) & 0x0FFF;
    out->bias_code = be16(d + 34) & 0x0FFF;
    out->tx_buffer_latency_ms = d[38] & 0x7F;
    out->ptt_hang_ms = d[40] & 0x1F;
    return true;
}

float hl2_temperature_c(uint16_t code)
{
    return (3.26f * code / 4096.0f - 0.5f) / 0.01f;
}

size_t hl2_encode_start(uint8_t* out, size_t cap)
{
    return encode_start_stop(0x01, out, cap);
}

size_t hl2_encode_stop(uint8_t* out, size_t cap)
{
    return encode_start_stop(0x00, out, cap);
}

bool hl2_config_register(Hl2RxRate rate, int receivers, Hl2Register* out)
{
    if (receivers < 1 || receivers > HL2_MAX_RECEIVERS) {
        return false;
    }
    out->address = kRegConfig;
    out->value = (static_cast<uint32_t>(rate) << 24) | (static_cast<uint32_t>(receivers - 1) << 3) | (1u << 2);
    return true;
}

bool hl2_nco_register(int receiver, uint32_t hz, Hl2Register* out)
{
    if (receiver < 0 || receiver >= HL2_MAX_RECEIVERS || hz > kMaxNcoHz) {
        return false;
    }
    out->address = static_cast<uint8_t>(kRegNcoFirst + receiver);
    out->value = hz;
    return true;
}

bool hl2_lna_register(int gain_db, Hl2Register* out)
{
    if (gain_db < -12 || gain_db > 48) {
        return false;
    }
    out->address = kRegLna;
    out->value = (1u << 6) | static_cast<uint32_t>(gain_db + 12);
    return true;
}

size_t hl2_encode_control_packet(uint32_t sequence, const Hl2Register& first, const Hl2Register& second,
                                 uint8_t* out, size_t cap)
{
    if (cap < HL2_PACKET_LEN || !is_receive_register(first) || !is_receive_register(second)) {
        return 0;
    }
    out[0] = kMagic0;
    out[1] = kMagic1;
    out[2] = kTypeData;
    out[3] = kEndpointDownstream;
    put_be32(out + 4, sequence);
    put_frame(out + 8, first);
    put_frame(out + 8 + HL2_FRAME_LEN, second);
    return HL2_PACKET_LEN;
}

int hl2_samples_per_frame(int receivers)
{
    if (receivers < 1 || receivers > HL2_MAX_RECEIVERS) {
        return 0;
    }
    return static_cast<int>(kFrameSampleBytes) / (6 * receivers + 2);
}

bool hl2_parse_rx_packet(const uint8_t* d, size_t len, int receivers, Hl2RxPacket* out, int32_t* iq,
                         size_t iq_cap)
{
    const int per_frame = hl2_samples_per_frame(receivers);
    if (per_frame == 0 || len != HL2_PACKET_LEN || d[0] != kMagic0 || d[1] != kMagic1 || d[2] != kTypeData ||
        d[3] != kEndpointUpstream) {
        return false;
    }
    const size_t needed = static_cast<size_t>(2 * receivers * 2 * per_frame);
    if (iq_cap < needed) {
        return false;
    }
    out->sequence = be32(d + 4);
    out->samples = 2 * per_frame;
    const size_t stride = static_cast<size_t>(6 * receivers + 2);
    int32_t* w = iq;
    for (int f = 0; f < 2; ++f) {
        const uint8_t* frame = d + 8 + f * HL2_FRAME_LEN;
        if (frame[0] != 0x7F || frame[1] != 0x7F || frame[2] != 0x7F) {
            return false;
        }
        const uint8_t c0 = frame[3];
        Hl2Status& s = out->status[f];
        s.ptt = c0 & 0x01;
        s.command_response = c0 & 0x80;
        s.address = s.command_response ? static_cast<uint8_t>((c0 >> 1) & 0x3F) : static_cast<uint8_t>((c0 >> 3) & 0x03);
        memcpy(s.data, frame + 4, sizeof s.data);
        for (int k = 0; k < per_frame; ++k) {
            const uint8_t* sample = frame + kFrameHeaderLen + k * stride;
            for (int r = 0; r < receivers; ++r) {
                *w++ = be24_signed(sample + 6 * r);
                *w++ = be24_signed(sample + 6 * r + 3);
            }
        }
    }
    return true;
}

void hl2_apply_status(const Hl2Status& s, Hl2Telemetry* t)
{
    t->ptt = s.ptt;
    if (s.command_response) {
        return;
    }
    t->seen |= static_cast<uint8_t>(1u << s.address);
    switch (s.address) {
        case 0:
            t->adc_clipped = s.data[0] & 0x01;
            t->tx_inhibited = !(s.data[0] & 0x02);
            t->tx_fifo_recovery = s.data[2] & 0x80;
            t->tx_fifo_count = s.data[2] & 0x7F;
            t->gateware_major = s.data[3];
            break;
        case 1:
            t->temperature_code = be16(s.data) & 0x0FFF;
            t->forward_code = be16(s.data + 2) & 0x0FFF;
            break;
        case 2:
            t->reverse_code = be16(s.data) & 0x0FFF;
            t->bias_code = be16(s.data + 2) & 0x0FFF;
            break;
        default:  // address 3 carries a debug word, unused on an HL2 (control.v:475)
            break;
    }
}

uint32_t hl2_sequence_note(Hl2SequenceTracker* t, uint32_t sequence)
{
    if (!t->started) {
        t->started = true;
        t->expected = sequence + 1;
        return 0;
    }
    const uint32_t ahead = sequence - t->expected;  // wraps
    if (ahead >= 0x80000000u) {                     // behind the expected number
        ++t->out_of_order;
        return 0;
    }
    t->expected = sequence + 1;
    if (ahead == 0) {
        return 0;
    }
    ++t->gaps;
    t->lost += ahead;
    return ahead;
}

void hl2_link_local_ipv4(const uint8_t mac[6], bool avoid_reserved, uint8_t out[4])
{
    out[0] = 169;
    out[1] = 254;
    out[2] = mac[4];
    out[3] = mac[5];
    if (avoid_reserved) {
        switch (out[2]) {
            case 0:
                out[2] = 1;
                break;
            case 255:
                out[2] = 254;
                break;
            default:
                break;
        }
    }
}
