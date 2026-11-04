// The HL2 over the P4's Ethernet port, receive only (hl2_transport.h; docs/roadmap/hl2-support.md, Decided and Backend; row mg9c).
//
// Board-only: the EMAC, the IP101 PHY, lwIP and one UDP socket. Everything sent and parsed goes through
// components/hl2_proto, which cannot encode MOX or any transmit register, so nothing here can key the radio.
//
// Addresses (docs/roadmap/hl2-support.md, Decided): ours is static link-local, derived from our MAC as the HL2 derives its own,
// so the segment needs no DHCP server and no conflict probe. Finding the HL2 never assumes 169.254/16 beyond the
// broadcast it is sent to: whatever address answers discovery is the one streamed from.
//
// Pacing: one downstream control packet for every upstream packet, so the HL2's watchdog is fed exactly while we
// are receiving, and the stream stops on its own if this task does.

#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_attr.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "esp_netif_net_stack.h"
#include "lwip/etharp.h"
#include "lwip/priv/tcpip_priv.h"
#include "lwip/sockets.h"

#include "debug_log.h"
#include "dial_history.h"
#include "hl2_proto.h"
#include "hl2_rx_frontend.h"
#include "iq_frontend.h"
#include "rtc_core.h"

#include "hl2_transport.h"
#include "porta_host.h"

static const char* TAG = "HL2";

// The U213's IP101 (docs/architecture/main_board.md): ESP-IDF's ESP32-P4 RMII defaults, plus these.
constexpr int kPhyAddress = 1;
constexpr int kPhyResetGpio = 51;

constexpr int kRecvTimeoutMs = 100;
constexpr int64_t kStreamLostUs = 1000 * 1000;
constexpr int64_t kStatusEveryUs = 15 * 1000 * 1000;
constexpr int kDiscoveryEveryMs = 1000;
// The LNA starts at the HL2's own power-on gain and steps down while the ADC clips, never back up within a stream:
// with an antenna on and no filter chosen the ADC clipped on every status word (bench, 2026-11-03).
constexpr int kLnaStartDb = 20;
constexpr int kLnaFloorDb = -12;
constexpr int kLnaStepDb = 3;
constexpr int64_t kLnaCheckUs = 1000 * 1000;
constexpr EventBits_t kLinkUp = BIT0;
constexpr EventBits_t kActive = BIT1;  // the HL2 is the radio feeding channel 0

static EventGroupHandle_t s_events;
static uint8_t s_our_ip[4];
static esp_netif_t* s_netif;
static uint32_t s_known_hl2 = 0;  // the HL2's address once found this boot, network order; 0 until then

// What the transport knows of the HL2, for the app's radio chip and slot bar (hl2_transport_state_name()).
enum class Hl2State : int { Off, NoLink, Silent, Busy, Found, Streaming, Lost };
static std::atomic<int> s_state{static_cast<int>(Hl2State::Off)};
static std::atomic<int64_t> s_overload_until_us{0};  // the ADC clipped with the LNA at its floor until then

static void set_state(Hl2State st)
{
    s_state.store(static_cast<int>(st));
}

// ARP, run in lwIP's thread: clear this interface's cache, or ask whether an address has a resolved entry. A
// silent HL2 still answers ARP, so a fresh entry after a probe means it is there and only not answering discovery.
struct ArpCall {
    struct tcpip_api_call_data base;
    ip4_addr_t ip;
    bool clear;
    bool found;
};

static err_t arp_in_lwip(struct tcpip_api_call_data* call)
{
    ArpCall* a = reinterpret_cast<ArpCall*>(call);
    struct netif* n = static_cast<struct netif*>(esp_netif_get_netif_impl(s_netif));
    if (!n) {
        return ERR_IF;
    }
    if (a->clear) {
        etharp_cleanup_netif(n);
    } else {
        struct eth_addr* eth = nullptr;
        const ip4_addr_t* ip = nullptr;
        a->found = etharp_find_addr(n, &a->ip, &eth, &ip) >= 0;
    }
    return ERR_OK;
}

static bool arp_call(uint32_t addr, bool clear)
{
    ArpCall a = {};
    a.ip.addr = addr;
    a.clear = clear;
    return tcpip_api_call(arp_in_lwip, &a.base) == ERR_OK && a.found;
}

// One packet each way, and one packet's I/Q: static, so the task's stack stays small, and in PSRAM, since lwIP
// copies to and from them and no DMA touches them (docs/architecture/memory.md).
static EXT_RAM_BSS_ATTR uint8_t s_rx[HL2_PACKET_LEN + 16];
static EXT_RAM_BSS_ATTR uint8_t s_tx[HL2_PACKET_LEN];
static EXT_RAM_BSS_ATTR int32_t s_iq[2 * 2 * 63];

// ---- Samples to decode -----------------------------------------------------------------------------------------
//
// The HL2 task writes each packet's I/Q, as float frames (I, Q, full scale ±1), into a stream buffer; the decode
// pipeline's ingest task reads them through the front end (hl2_audio_read_ft8_samples). One writer, one reader.
// Only whole packets are written, so the buffer always holds whole frames, and a packet with no room is dropped and
// counted. A sequence gap is written as that many packets of zeros (docs/roadmap/hl2-support.md, Receive), so the slot's timeline holds.
// Half a second of stream, in PSRAM; so is the front end, since internal RAM is the budget (memory.md).
constexpr int kFramesPerPacket = 2 * 63;
constexpr size_t kFrameBytes = 2 * sizeof(float);
constexpr size_t kRingBytes = 24000 * kFrameBytes;
static EXT_RAM_BSS_ATTR uint8_t s_ring_storage[kRingBytes + 1];
static StaticStreamBuffer_t s_ring_struct;
static StreamBufferHandle_t s_ring;
static EXT_RAM_BSS_ATTR float s_frames[2 * kFramesPerPacket];        // the HL2 task's packet, as floats
static EXT_RAM_BSS_ATTR float s_zero_frames[2 * kFramesPerPacket];   // a lost packet
static EXT_RAM_BSS_ATTR iq_frontend_t s_frontend;
static EXT_RAM_BSS_ATTR float s_read_frames[2 * 8 * kFramesPerPacket];  // the reader's chunk
static std::atomic<bool> s_streaming{false};   // samples are arriving; the reader may run
static std::atomic<uint32_t> s_stream_gen{0};  // bumped at each stream start, so the reader resets the front end
static uint32_t s_reader_gen = 0;              // ingest task only
static std::atomic<uint32_t> s_frontend_us{0};
static std::atomic<uint32_t> s_ring_drops{0};  // packets with no room in the ring
static std::atomic<uint32_t> s_dial_hz{0};     // the station's dial: the receiver's NCO


static void ring_write_packet(const float* frames)
{
    const size_t bytes = kFramesPerPacket * kFrameBytes;
    if (xStreamBufferSpacesAvailable(s_ring) < bytes ||
        xStreamBufferSend(s_ring, frames, bytes, 0) != bytes) {
        s_ring_drops.fetch_add(1, std::memory_order_relaxed);
    }
}

static void on_eth_event(void* /*arg*/, esp_event_base_t /*base*/, int32_t id, void* data)
{
    switch (id) {
        case ETHERNET_EVENT_CONNECTED: {
            xEventGroupSetBits(s_events, kLinkUp);
            // The IP101 is 10/100: a half-duplex link would show only as gaps under load, so say what it got.
            eth_speed_t speed = ETH_SPEED_10M;
            eth_duplex_t duplex = ETH_DUPLEX_HALF;
            esp_eth_handle_t eth = data ? *static_cast<esp_eth_handle_t*>(data) : nullptr;
            if (eth) {
                esp_eth_ioctl(eth, ETH_CMD_G_SPEED, &speed);
                esp_eth_ioctl(eth, ETH_CMD_G_DUPLEX_MODE, &duplex);
            }
            ESP_LOGI(TAG, "link up, %s Mb/s %s duplex", speed == ETH_SPEED_100M ? "100" : "10",
                     duplex == ETH_DUPLEX_FULL ? "full" : "half");
            if (duplex != ETH_DUPLEX_FULL) {
                debug_log_line("HL2 Ethernet link is half duplex: expect gaps");
            }
            if (s_state.load() != static_cast<int>(Hl2State::Off) && s_state.load() != static_cast<int>(Hl2State::Lost)) {
                set_state(Hl2State::Silent);
            }
            break;
        }
        case ETHERNET_EVENT_DISCONNECTED:
            xEventGroupClearBits(s_events, kLinkUp);
            ESP_LOGW(TAG, "link down");
            debug_log_line("HL2 Ethernet link down");
            if (s_state.load() != static_cast<int>(Hl2State::Off)) {
                set_state(Hl2State::NoLink);
            }
            break;
        default:
            break;
    }
}

static bool start_ethernet(void)
{
    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) {
        err = esp_event_loop_create_default();
        if (err == ESP_ERR_INVALID_STATE) {
            err = ESP_OK;  // already made by someone else
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "netif or event loop: %s", esp_err_to_name(err));
        return false;
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = kPhyAddress;
    phy_config.reset_gpio_num = kPhyResetGpio;
    esp_eth_mac_t* mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    esp_eth_phy_t* phy = esp_eth_phy_new_ip101(&phy_config);
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = nullptr;
    if (!mac || !phy || (err = esp_eth_driver_install(&config, &eth)) != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver: %s", mac && phy ? esp_err_to_name(err) : "MAC or PHY not created");
        return false;
    }

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t* netif = esp_netif_new(&netif_config);
    s_netif = netif;
    if (!netif || esp_netif_attach(netif, esp_eth_new_netif_glue(eth)) != ESP_OK) {
        ESP_LOGE(TAG, "netif attach failed");
        return false;
    }

    uint8_t our_mac[6];
    esp_read_mac(our_mac, ESP_MAC_ETH);
    hl2_link_local_ipv4(our_mac, true, s_our_ip);
    esp_netif_dhcpc_stop(netif);
    esp_netif_ip_info_t info = {};
    IP4_ADDR(&info.ip, s_our_ip[0], s_our_ip[1], s_our_ip[2], s_our_ip[3]);
    IP4_ADDR(&info.netmask, 255, 255, 0, 0);
    if ((err = esp_netif_set_ip_info(netif, &info)) != ESP_OK) {
        ESP_LOGE(TAG, "static link-local address: %s", esp_err_to_name(err));
        return false;
    }

    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, nullptr);
    if ((err = esp_eth_start(eth)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Ethernet started, our address %u.%u.%u.%u", s_our_ip[0], s_our_ip[1], s_our_ip[2],
             s_our_ip[3]);
    return true;
}

static void send_to(int sock, const sockaddr_in& to, const uint8_t* data, size_t len)
{
    sendto(sock, data, len, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
}

// Broadcasts discovery until a stopped HL2 answers; fills its address. False if the link dropped meanwhile.
static bool discover(int sock, sockaddr_in* hl2, Hl2DiscoveryReply* reply)
{
    sockaddr_in broadcast = {};
    broadcast.sin_family = AF_INET;
    broadcast.sin_port = htons(HL2_PORT);
    broadcast.sin_addr.s_addr = htonl(0xA9FEFFFFu);  // 169.254.255.255
    uint8_t request[HL2_DISCOVERY_REQUEST_LEN];
    hl2_encode_discovery_request(request, sizeof request);
    bool said_running = false;
    while ((xEventGroupGetBits(s_events) & (kLinkUp | kActive)) == (kLinkUp | kActive)) {
        bool running_reply = false;
        if (s_known_hl2) {
            // Probe where it was: a fresh ARP entry after this send means it is there, if silent.
            arp_call(s_known_hl2, true);
            sockaddr_in known = broadcast;
            known.sin_addr.s_addr = s_known_hl2;
            send_to(sock, known, request, sizeof request);
        }
        send_to(sock, broadcast, request, sizeof request);
        const int64_t until = esp_timer_get_time() + kDiscoveryEveryMs * 1000;
        while (esp_timer_get_time() < until) {
            sockaddr_in from = {};
            socklen_t from_len = sizeof from;
            const int n = recvfrom(sock, s_rx, sizeof s_rx, 0, reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n <= 0 || !hl2_parse_discovery_reply(s_rx, static_cast<size_t>(n), reply)) {
                continue;
            }
            if (reply->running) {
                // Another client holds the stream; taking it would be a stream loss for them (docs/roadmap/hl2-support.md, Safety 11).
                running_reply = true;
                set_state(Hl2State::Busy);
                if (!said_running) {
                    ESP_LOGW(TAG, "an HL2 answered but is already streaming to another client; waiting");
                    said_running = true;
                }
                continue;
            }
            *hl2 = from;
            hl2->sin_port = htons(HL2_PORT);
            s_known_hl2 = from.sin_addr.s_addr;
            set_state(Hl2State::Found);
            return true;
        }
        if (!running_reply) {
            if (s_known_hl2 && arp_call(s_known_hl2, false)) {
                set_state(Hl2State::Busy);  // answers ARP, not discovery: streaming to someone else
            } else if (s_state.load() != static_cast<int>(Hl2State::Lost)) {
                set_state(Hl2State::Silent);
            }
        }
    }
    return false;
}

static void log_status(const Hl2SequenceTracker& seq, uint32_t packets, uint32_t clipped,
                       const Hl2Telemetry& tel)
{
    char line[192];
    const uint32_t drops = s_ring_drops.load(std::memory_order_relaxed);
    snprintf(line, sizeof line,
             "HL2 rx packets %lu gaps %lu lost %lu late %lu ring_drops %lu clipped %lu temp %.1f C",
             (unsigned long)packets, (unsigned long)seq.gaps, (unsigned long)seq.lost,
             (unsigned long)seq.out_of_order, (unsigned long)drops, (unsigned long)clipped,
             (tel.seen & 0x02) ? hl2_temperature_c(tel.temperature_code) : 0.0f);
    ESP_LOGI(TAG, "%s", line);
    if (seq.gaps || clipped || drops) {
        debug_log_line(line);
    }
}

// Starts one receiver and runs until the stream stops arriving. Returns after sending stop.
static void stream(int sock, const sockaddr_in& hl2)
{
    Hl2Register config{}, nco{}, lna{};
    uint32_t dial = s_dial_hz.load();
    hl2_config_register(Hl2RxRate::Rate48k, 1, &config, hl2_n2adr_receive_filters(dial));
    int lna_db = kLnaStartDb;
    hl2_lna_register(lna_db, &lna);
    if (!hl2_nco_register(0, dial, &nco)) {
        ESP_LOGE(TAG, "dial %lu Hz is outside the HL2's range; not streaming", (unsigned long)dial);
        vTaskDelay(pdMS_TO_TICKS(1000));
        return;
    }
    uint32_t tx_seq = 0;
    for (int i = 0; i < 4; ++i) {
        if (hl2_encode_control_packet(tx_seq++, (i & 1) ? lna : config, nco, s_tx, sizeof s_tx)) {
            send_to(sock, hl2, s_tx, sizeof s_tx);
        }
    }
    uint8_t start[HL2_START_STOP_LEN];
    hl2_encode_start(start, sizeof start);
    send_to(sock, hl2, start, sizeof start);
    ESP_LOGI(TAG, "streaming one receiver at 48 kHz, NCO %lu Hz, N2ADR filters 0x%02x, LNA %+d dB",
             (unsigned long)nco.value, (unsigned)((config.value >> 17) & 0x7F), lna_db);
    debug_log_line("HL2 streaming");

    Hl2SequenceTracker seq{};
    Hl2Telemetry tel{};
    uint32_t packets = 0, clipped = 0;
    xStreamBufferReset(s_ring);  // nothing of an earlier stream; the reader is not running yet
    s_ring_drops.store(0);
    s_stream_gen.fetch_add(1);
    s_streaming = true;
    set_state(Hl2State::Streaming);
    int64_t last_rx = esp_timer_get_time();
    int64_t next_status = last_rx + kStatusEveryUs;
    bool lost = false;
    uint32_t window_status0 = 0, window_clipped = 0;
    int64_t next_lna_check = last_rx + kLnaCheckUs;
    while (true) {
        const int64_t now = esp_timer_get_time();
        const EventBits_t bits = xEventGroupGetBits(s_events);
        if (!(bits & kActive)) {
            break;  // another radio was chosen
        }
        if (now - last_rx > kStreamLostUs || !(bits & kLinkUp)) {
            lost = true;
            break;
        }
        if (now >= next_status) {
            log_status(seq, packets, clipped, tel);
            next_status = now + kStatusEveryUs;
        }
        const int n = recv(sock, s_rx, sizeof s_rx, 0);
        Hl2RxPacket pkt;
        if (n <= 0 || !hl2_parse_rx_packet(s_rx, static_cast<size_t>(n), 1, &pkt, s_iq, sizeof s_iq / sizeof s_iq[0])) {
            continue;
        }
        last_rx = esp_timer_get_time();
        ++packets;
        const uint32_t late_before = seq.out_of_order;
        const uint32_t missing = hl2_sequence_note(&seq, pkt.sequence);
        if (seq.out_of_order != late_before) {
            // A duplicate or a late packet: its place in the timeline was already written, zero-filled when it
            // went missing, so its samples would push every later block late. Counted, not written.
            continue;
        }
        for (uint32_t i = 0; i < missing && i < 400; ++i) {  // past ~1 s of loss the stream is lost anyway
            ring_write_packet(s_zero_frames);
        }
        for (int i = 0; i < 2 * kFramesPerPacket; ++i) {
            s_frames[i] = static_cast<float>(s_iq[i]) / 8388608.0f;
        }
        ring_write_packet(s_frames);
        const uint32_t want = s_dial_hz.load(std::memory_order_relaxed);
        if (want != dial) {  // the band changed: retune and refilter from the next control packet on
            dial = want;
            if (!hl2_nco_register(0, want, &nco)) {
                ESP_LOGE(TAG, "dial %lu Hz is outside the HL2's range; NCO kept", (unsigned long)want);
            }
            hl2_config_register(Hl2RxRate::Rate48k, 1, &config, hl2_n2adr_receive_filters(want));
            ESP_LOGI(TAG, "N2ADR filters 0x%02x for %lu Hz", (unsigned)((config.value >> 17) & 0x7F),
                     (unsigned long)want);
        }
        for (const Hl2Status& s : pkt.status) {
            hl2_apply_status(s, &tel);
            if (!s.command_response && s.address == 0) {
                ++window_status0;
                if (tel.adc_clipped) {
                    ++clipped;
                    ++window_clipped;
                }
            }
        }
        if (last_rx >= next_lna_check) {
            // More than 1% of a second's address-0 words clipped: 3 dB less gain.
            if (window_clipped * 100 > window_status0 && lna_db == kLnaFloorDb) {
                s_overload_until_us = last_rx + kStatusEveryUs;  // the slot bar says so for 15 s
            }
            if (window_clipped * 100 > window_status0 && lna_db > kLnaFloorDb) {
                lna_db = lna_db - kLnaStepDb < kLnaFloorDb ? kLnaFloorDb : lna_db - kLnaStepDb;
                hl2_lna_register(lna_db, &lna);
                ESP_LOGI(TAG, "ADC clipped in %lu of %lu status words: LNA %+d dB", (unsigned long)window_clipped,
                         (unsigned long)window_status0, lna_db);
            }
            window_status0 = window_clipped = 0;
            next_lna_check = last_rx + kLnaCheckUs;
        }
        // Each packet carries the NCO and, in turn, the config (filters) and the LNA.
        if (hl2_encode_control_packet(tx_seq, (tx_seq & 1) ? lna : config, nco, s_tx, sizeof s_tx)) {
            send_to(sock, hl2, s_tx, sizeof s_tx);
        }
        ++tx_seq;
    }

    s_streaming = false;  // the pipeline's run ends on its next should_stop
    uint8_t stop[HL2_START_STOP_LEN];
    hl2_encode_stop(stop, sizeof stop);
    send_to(sock, hl2, stop, sizeof stop);
    log_status(seq, packets, clipped, tel);
    s_overload_until_us = 0;
    set_state(lost ? Hl2State::Lost : Hl2State::Off);
    if (lost) {
        ESP_LOGW(TAG, "stream lost after %lu packets; back to discovery", (unsigned long)packets);
        debug_log_line("HL2 stream lost; looking for the radio again");
    } else {
        ESP_LOGI(TAG, "stream stopped after %lu packets: another radio was chosen", (unsigned long)packets);
        debug_log_line("HL2 stream stopped: another radio was chosen");
    }
}

static void hl2_task(void* /*arg*/)
{
    // The front end's design calls libm (soft double), so it runs here, before any stream, on this task's stack.
    if (iq_frontend_init(&s_frontend, &kHl2RxFrontendConfig) != IQ_FRONTEND_OK) {
        ESP_LOGE(TAG, "I/Q front end init failed: the HL2 cannot be decoded");
        debug_log_line("HL2 I/Q front end init failed; no HL2 decode this boot");
    }
    ESP_LOGI(TAG, "I/Q front end ready, NCO %.3f Hz above the LO; stack %u bytes free", iq_frontend_nco_hz(&s_frontend),
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const int on = 1;
    timeval timeout = {0, kRecvTimeoutMs * 1000};
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (sock < 0 || setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof on) != 0 ||
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) != 0 ||
        bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof local) != 0) {
        ESP_LOGE(TAG, "UDP socket failed; the HL2 path is stopped");
        debug_log_line("HL2 UDP socket failed; no HL2 receive this boot");
        vTaskDelete(nullptr);
        return;
    }
    while (true) {
        xEventGroupWaitBits(s_events, kLinkUp | kActive, pdFALSE, pdTRUE, portMAX_DELAY);
        sockaddr_in hl2 = {};
        Hl2DiscoveryReply reply{};
        if (!discover(sock, &hl2, &reply)) {
            continue;
        }
        const uint8_t* a = reinterpret_cast<const uint8_t*>(&hl2.sin_addr.s_addr);
        uint8_t derived[4];
        hl2_link_local_ipv4(reply.mac, false, derived);
        if (memcmp(derived, s_our_ip, sizeof derived) == 0) {
            // Same last two MAC bytes: one address for two boards (docs/roadmap/hl2-support.md, Decided). Never stream then.
            ESP_LOGE(TAG, "the HL2's link-local address is ours (%u.%u.%u.%u); not streaming", a[0], a[1], a[2], a[3]);
            debug_log_line("HL2 address collides with this P4's; HL2 receive stopped this boot");
            break;
        }
        char line[128];
        snprintf(line, sizeof line, "HL2 found at %u.%u.%u.%u: gateware %u.%u, %u receivers, %.1f C", a[0], a[1],
                 a[2], a[3], reply.gateware_major, reply.gateware_minor, reply.receivers,
                 hl2_temperature_c(reply.temperature_code));
        ESP_LOGI(TAG, "%s", line);
        debug_log_line(line);
        stream(sock, hl2);
    }
    close(sock);
    vTaskDelete(nullptr);
}

static bool s_started = false;
static bool s_failed = false;  // a half-started EMAC is not installed twice

bool hl2_transport_set_active(bool active)
{
    if (!active) {
        if (s_events) {
            xEventGroupClearBits(s_events, kActive);  // the task sends stop on its next packet or timeout
        }
        set_state(Hl2State::Off);
        return true;
    }
    if (s_failed) {
        return false;
    }
    if (!s_started) {
        s_failed = true;  // until every step below has succeeded
        if (!s_events) {
            s_events = xEventGroupCreate();
        }
        if (!s_ring) {
            s_ring = xStreamBufferCreateStatic(kRingBytes, kFrameBytes, s_ring_storage, &s_ring_struct);
        }
        if (!s_events || !s_ring || !start_ethernet()) {
            debug_log_line("HL2 Ethernet could not start; no HL2 receive");
            return false;
        }
        // Core 0, with the main loop; core 1 is decode's (docs/architecture/main_board.md, Tasks).
        if (xTaskCreatePinnedToCore(hl2_task, "hl2_rx", 6144, nullptr, 5, nullptr, 0) != pdPASS) {
            ESP_LOGE(TAG, "could not create the HL2 task");
            return false;
        }
        s_started = true;
        s_failed = false;
    }
    if (s_state.load() == static_cast<int>(Hl2State::Off)) {
        set_state((xEventGroupGetBits(s_events) & kLinkUp) ? Hl2State::Silent : Hl2State::NoLink);
    }
    xEventGroupSetBits(s_events, kActive);
    return true;
}

const char* hl2_transport_state_name(void)
{
    switch (static_cast<Hl2State>(s_state.load())) {
        case Hl2State::Off:
            return "off";
        case Hl2State::NoLink:
            return "nolink";
        case Hl2State::Silent:
            return "silent";
        case Hl2State::Busy:
            return "busy";
        case Hl2State::Found:
            return "found";
        case Hl2State::Streaming:
            return "streaming";
        case Hl2State::Lost:
            return "lost";
    }
    return "off";
}

bool hl2_transport_overloaded(void)
{
    return esp_timer_get_time() < s_overload_until_us.load();
}

void hl2_transport_set_dial(uint32_t hz)
{
    if (hz == 0 || hz == s_dial_hz.load()) {
        return;
    }
    s_dial_hz.store(hz);
    // Each decode carries the dial its slot was heard on (dial_history.h); the main loop owns the history.
    dial_history_note(rtc_now_ms(), hz);
    ESP_LOGI(TAG, "dial %lu Hz", (unsigned long)hz);
}

bool hl2_audio_ready(void)
{
    return s_streaming;
}

bool hl2_audio_should_stop(void* /*ctx*/)
{
    return !s_streaming;
}

uint32_t hl2_audio_take_frontend_us(void* /*ctx*/)
{
    return s_frontend_us.exchange(0, std::memory_order_relaxed);
}

int hl2_audio_read_ft8_samples(void* /*ctx*/, float* out, int max_samples)
{
    if (!s_ring || !out || max_samples <= 0) {
        return 0;
    }
    const uint32_t gen = s_stream_gen.load();
    if (gen != s_reader_gen) {
        iq_frontend_reset(&s_frontend);  // a new stream: no filter history from the last one
        s_reader_gen = gen;
    }
    size_t frames = sizeof s_read_frames / kFrameBytes;
    while (frames > 1 && iq_frontend_output_capacity(&s_frontend, frames) > static_cast<size_t>(max_samples)) {
        frames /= 2;
    }
    const size_t got = xStreamBufferReceive(s_ring, s_read_frames, frames * kFrameBytes, pdMS_TO_TICKS(200));
    if (got < kFrameBytes) {
        return 0;
    }
    const int64_t t0 = esp_timer_get_time();
    const int n = iq_frontend_process(&s_frontend, s_read_frames, got / kFrameBytes, out,
                                      static_cast<size_t>(max_samples));
    s_frontend_us.fetch_add(static_cast<uint32_t>(esp_timer_get_time() - t0), std::memory_order_relaxed);
    return n > 0 ? n : 0;
}
