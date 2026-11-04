#pragma once

// ============================================================================
// hl2_proto.h — the Hermes-Lite 2's openHPSDR Protocol 1 wire format ([RFC 0014](../../../docs/rfcs/0014-hl2-two-p4.md)
// §3.3, row mg9c).
//
// Pure codec: no sockets, no globals, no ESP-IDF, nothing allocated. Host-tested (host_test_hl2_proto). The wire
// contract is this header plus the golden frames in host_mock/hl2_golden/, recorded from the operator's HL2 on
// gateware 74.2. Written from the gateware's behaviour (softerhardware/Hermes-Lite2, cited per field) and the
// openHPSDR documents; no client's code was copied.
//
// RECEIVE ONLY (S1). This header can build discovery, start, stop, and writes to the receive registers, and
// nothing that keys the radio: every downstream frame has MOX clear and zero transmit samples, and
// hl2_encode_control_packet() refuses every other register, and the PA-enable bit of register 0x00. Transmit is
// S2's, under RFC 0014 §3.8.
//
// Every multi-byte field on the wire is big-endian.
// ============================================================================

#include <cstddef>
#include <cstdint>

constexpr uint16_t HL2_PORT = 1024;
constexpr size_t HL2_DISCOVERY_REQUEST_LEN = 63;
constexpr size_t HL2_DISCOVERY_REPLY_LEN = 60;
constexpr size_t HL2_START_STOP_LEN = 64;
constexpr size_t HL2_PACKET_LEN = 1032;  // EF FE 01, endpoint, 32-bit sequence, two 512-byte frames
constexpr size_t HL2_FRAME_LEN = 512;    // 7F 7F 7F, C0..C4, 504 bytes of samples
constexpr int HL2_MAX_RECEIVERS = 4;     // the standard hl2b5up_main gateware
constexpr uint8_t HL2_BOARD_ID = 0x06;

// ---- Discovery -------------------------------------------------------------------------------------------------

// The 60-byte reply (gateware rtl/usopenhpsdr1.v:265-307; reply byte n is case label 0x3b - n).
struct Hl2DiscoveryReply {
    bool running;                  // status byte: 02 stopped, 03 running
    uint8_t mac[6];
    uint8_t gateware_major;        // 74 for 74.2
    uint8_t gateware_minor;
    uint8_t board_id;              // HL2_BOARD_ID on an HL2
    uint8_t receivers;
    uint8_t board;                 // BOARD[5:0]
    uint8_t static_ip[4];          // 0.0.0.0 when the EEPROM holds none
    uint8_t response_control;      // {ext CW key, PTT, EXTTR, internal T/R, tx_on, cw_on, clip count[1:0]}
    uint16_t temperature_code;     // 12-bit ADC codes; see hl2_temperature_c()
    uint16_t forward_code;
    uint16_t reverse_code;
    uint16_t bias_code;
    uint8_t tx_buffer_latency_ms;  // 10 at power-on on 74.2 (usopenhpsdr1.v:108)
    uint8_t ptt_hang_ms;           // 4 at power-on on 74.2 (usopenhpsdr1.v:109)
};

// Writes the 63-byte request (EF FE 02 and 60 zeros). Returns its length, or 0 if cap is too small.
size_t hl2_encode_discovery_request(uint8_t* out, size_t cap);

// False unless data is a 60-byte HL2 discovery reply (EF FE, status 02 or 03, board ID 0x06).
bool hl2_parse_discovery_reply(const uint8_t* data, size_t len, Hl2DiscoveryReply* out);

// Degrees Celsius from the temperature code (RFC 0014 §2.3).
float hl2_temperature_c(uint16_t code);

// ---- Start and stop --------------------------------------------------------------------------------------------

// EF FE 04 01 (IQ on, no bandscope) or EF FE 04 00, padded to 64 bytes. Returns the length, or 0.
size_t hl2_encode_start(uint8_t* out, size_t cap);
size_t hl2_encode_stop(uint8_t* out, size_t cap);

// ---- Downstream: receive registers -----------------------------------------------------------------------------

enum class Hl2RxRate : uint8_t { Rate48k = 0, Rate96k = 1, Rate192k = 2, Rate384k = 3 };

// One register write: a 6-bit address and its 32-bit value (C1..C4). A frame carries one.
struct Hl2Register {
    uint8_t address;
    uint32_t value;
};

// Register 0x00: rate [25:24], the N2ADR filter byte [23:17], last receiver [6:3], duplex [2]
// (radio_openhpsdr1/radio.v:269; RFC 0014 §2.3). Duplex is always set, so the receive NCOs stay independent of the TX
// NCO. False unless 1 <= receivers <= HL2_MAX_RECEIVERS.
bool hl2_config_register(Hl2RxRate rate, int receivers, Hl2Register* out, uint8_t n2adr = 0);

// The N2ADR filter byte for receiving at hz (RFC 0014 §2.3): b0 160 m, b1 80 m, b2 60/40 m, b3 30/20 m, b4 17/15 m,
// b5 12/10 m low-pass, in the receive path as well as transmit. The receive high-pass (about 2.5 MHz at -3 dB,
// 45 dB down at 1 MHz) is in line unless b6 is set: its relay is a NAND of b6 and the HL2's INTTR (n2adr.net, U5,
// K7). So b6 is set only below 2.5 MHz, where the high-pass would cost 160 m 17 dB.
uint8_t hl2_n2adr_receive_filters(uint32_t hz);

// Registers 0x02..0x05: receiver 0..3's NCO, in hertz (radio.v:255). False for another receiver or above 38.4 MHz.
bool hl2_nco_register(int receiver, uint32_t hz, Hl2Register* out);

// Register 0x0a in direct mode, bit 6: gain -12..+48 dB (ad9866.v:133). False outside that range.
bool hl2_lna_register(int gain_db, Hl2Register* out);

// A 1032-byte EP2 packet carrying two register writes, MOX clear and zero transmit samples. Returns its length, or
// 0 if cap is too small or either register is not one of the receive registers above (or sets register 0x00's
// PA-enable bit [16]).
size_t hl2_encode_control_packet(uint32_t sequence, const Hl2Register& first, const Hl2Register& second,
                                 uint8_t* out, size_t cap);

// ---- Upstream: receive packets ---------------------------------------------------------------------------------

// C0..C4 of one upstream frame (control.v:466-475).
struct Hl2Status {
    bool ptt;                  // C0[0]
    bool command_response;     // C0[7]: a reply to a command on port 1025, not a status word
    uint8_t address;           // status: C0[4:3], 0..3; command response: C0[6:1]
    uint8_t data[4];           // C1..C4
};

// What the status words say, accumulated over frames; each address fills its own fields.
struct Hl2Telemetry {
    uint8_t seen;              // bit n set once address n has been applied
    bool ptt;                  // from every status word
    bool adc_clipped;          // address 0, C1[0]: the ADC clipped in the last interval
    bool tx_inhibited;         // address 0, C1[1] clear
    bool tx_fifo_recovery;     // address 0, C3[7] (fifos.v:116)
    uint8_t tx_fifo_count;     // address 0, C3[6:0]
    uint8_t gateware_major;    // address 0, C4
    uint16_t temperature_code; // address 1
    uint16_t forward_code;
    uint16_t reverse_code;     // address 2
    uint16_t bias_code;
};

struct Hl2RxPacket {
    uint32_t sequence;
    Hl2Status status[2];       // one per frame
    int samples;               // per receiver, both frames
};

// Samples per receiver in one frame: floor(504 / (6 N + 2)). 0 for N outside 1..HL2_MAX_RECEIVERS.
int hl2_samples_per_frame(int receivers);

// Parses an EP6 packet for the given receiver count. I/Q goes to iq as signed 24-bit values, sample by sample, and
// within a sample receiver by receiver, I then Q: iq needs 2 * receivers * 2 * hl2_samples_per_frame() entries. The
// 16-bit microphone word (zeros on an HL2) is skipped. False on a wrong length, header or frame sync, a bad
// receiver count, or too small an iq; then out and iq are not to be used.
bool hl2_parse_rx_packet(const uint8_t* data, size_t len, int receivers, Hl2RxPacket* out, int32_t* iq,
                         size_t iq_cap);

// Folds one status word into telemetry. A command response changes nothing but ptt.
void hl2_apply_status(const Hl2Status& status, Hl2Telemetry* telemetry);

// ---- Sequence gaps (RFC 0014 §3.11) ----------------------------------------------------------------------------

// Upstream sequence numbers, packet by packet. The first packet seen sets the start; a later one that is not the
// next counts the packets between as lost; one at or behind the expected number (a duplicate or a reordered
// packet) is counted and otherwise ignored. Arithmetic wraps at 2^32.
struct Hl2SequenceTracker {
    bool started;
    uint32_t expected;
    uint32_t gaps;          // places where packets went missing
    uint32_t lost;          // packets missing in all
    uint32_t out_of_order;  // duplicates and late arrivals
};

// Notes one sequence number; returns how many packets were lost just before it (0 for the expected one, a
// duplicate or a late one), so the caller can zero-fill that many packets' samples.
uint32_t hl2_sequence_note(Hl2SequenceTracker* tracker, uint32_t sequence);

// ---- Addresses -------------------------------------------------------------------------------------------------

// The link-local address 169.254.MAC[15:8].MAC[7:0], as the HL2 computes its own with no DHCP lease
// (rtl/ethernet/network.v:182). For our own boards pass avoid_reserved: RFC 3927 reserves 169.254.0.x and
// 169.254.255.x, which then become 169.254.1.x and 169.254.254.x. The HL2 itself does not avoid them.
void hl2_link_local_ipv4(const uint8_t mac[6], bool avoid_reserved, uint8_t out[4]);
