// The HL2 over the P4's Ethernet port, receive only (hl2_transport.h; RFC 0014 §3.1, §3.3, S1).
//
// Board-only: the EMAC, the IP101 PHY, lwIP and one UDP socket. Everything sent and parsed goes through
// components/hl2_proto, which cannot encode MOX or any transmit register, so nothing here can key the radio.
//
// Addresses (RFC 0014 §8, 2026-11-03): ours is static link-local, derived from our MAC as the HL2 derives its own,
// so the segment needs no DHCP server and no conflict probe. Finding the HL2 never assumes 169.254/16 beyond the
// broadcast it is sent to: whatever address answers discovery is the one streamed from.
//
// Pacing: one downstream control packet for every upstream packet, so the HL2's watchdog is fed exactly while we
// are receiving, and the stream stops on its own if this task does.

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
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "debug_log.h"
#include "hl2_proto.h"

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
constexpr EventBits_t kLinkUp = BIT0;
constexpr EventBits_t kActive = BIT1;  // the HL2 is the radio feeding channel 0

static EventGroupHandle_t s_events;
static uint8_t s_our_ip[4];

// One packet each way, and one packet's I/Q: static, so the task's stack stays small, and in PSRAM, since lwIP
// copies to and from them and no DMA touches them (docs/architecture/memory.md).
static EXT_RAM_BSS_ATTR uint8_t s_rx[HL2_PACKET_LEN + 16];
static EXT_RAM_BSS_ATTR uint8_t s_tx[HL2_PACKET_LEN];
static EXT_RAM_BSS_ATTR int32_t s_iq[2 * 2 * 63];

static void on_eth_event(void* /*arg*/, esp_event_base_t /*base*/, int32_t id, void* /*data*/)
{
    switch (id) {
        case ETHERNET_EVENT_CONNECTED:
            xEventGroupSetBits(s_events, kLinkUp);
            ESP_LOGI(TAG, "link up");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            xEventGroupClearBits(s_events, kLinkUp);
            ESP_LOGW(TAG, "link down");
            debug_log_line("HL2 Ethernet link down");
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
                // Another client holds the stream; taking it would be a stream loss for them (RFC 0014 §3.9).
                if (!said_running) {
                    ESP_LOGW(TAG, "an HL2 answered but is already streaming to another client; waiting");
                    said_running = true;
                }
                continue;
            }
            *hl2 = from;
            hl2->sin_port = htons(HL2_PORT);
            return true;
        }
    }
    return false;
}

static void log_status(const Hl2SequenceTracker& seq, uint32_t packets, uint32_t clipped,
                       const Hl2Telemetry& tel)
{
    char line[160];
    snprintf(line, sizeof line, "HL2 rx packets %lu gaps %lu lost %lu late %lu clipped %lu temp %.1f C",
             (unsigned long)packets, (unsigned long)seq.gaps, (unsigned long)seq.lost,
             (unsigned long)seq.out_of_order, (unsigned long)clipped,
             (tel.seen & 0x02) ? hl2_temperature_c(tel.temperature_code) : 0.0f);
    ESP_LOGI(TAG, "%s", line);
    if (seq.gaps || clipped) {
        debug_log_line(line);
    }
}

// Starts one receiver and runs until the stream stops arriving. Returns after sending stop.
static void stream(int sock, const sockaddr_in& hl2)
{
    Hl2Register config{}, nco{};
    hl2_config_register(Hl2RxRate::Rate48k, 1, &config);
    hl2_nco_register(0, static_cast<uint32_t>(porta_p4_station_freq_hz()), &nco);
    uint32_t tx_seq = 0;
    for (int i = 0; i < 4; ++i) {
        if (hl2_encode_control_packet(tx_seq++, config, nco, s_tx, sizeof s_tx)) {
            send_to(sock, hl2, s_tx, sizeof s_tx);
        }
    }
    uint8_t start[HL2_START_STOP_LEN];
    hl2_encode_start(start, sizeof start);
    send_to(sock, hl2, start, sizeof start);
    ESP_LOGI(TAG, "streaming one receiver at 48 kHz, NCO %lu Hz", (unsigned long)nco.value);
    debug_log_line("HL2 streaming");

    Hl2SequenceTracker seq{};
    Hl2Telemetry tel{};
    uint32_t packets = 0, clipped = 0;
    int64_t last_rx = esp_timer_get_time();
    int64_t next_status = last_rx + kStatusEveryUs;
    bool lost = false;
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
        hl2_sequence_note(&seq, pkt.sequence);
        for (const Hl2Status& s : pkt.status) {
            hl2_apply_status(s, &tel);
            if (!s.command_response && s.address == 0 && tel.adc_clipped) {
                ++clipped;
            }
        }
        if (hl2_encode_control_packet(tx_seq++, config, nco, s_tx, sizeof s_tx)) {
            send_to(sock, hl2, s_tx, sizeof s_tx);
        }
    }

    uint8_t stop[HL2_START_STOP_LEN];
    hl2_encode_stop(stop, sizeof stop);
    send_to(sock, hl2, stop, sizeof stop);
    log_status(seq, packets, clipped, tel);
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
            // Same last two MAC bytes: one address for two boards (RFC 0014 §8, 2026-11-03). Never stream then.
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
        if (!s_events || !start_ethernet()) {
            debug_log_line("HL2 Ethernet could not start; no HL2 receive");
            return false;
        }
        // Core 0, with the main loop; core 1 is decode's (docs/architecture/main_board.md, Tasks).
        if (xTaskCreatePinnedToCore(hl2_task, "hl2_rx", 4096, nullptr, 5, nullptr, 0) != pdPASS) {
            ESP_LOGE(TAG, "could not create the HL2 task");
            return false;
        }
        s_started = true;
        s_failed = false;
    }
    xEventGroupSetBits(s_events, kActive);
    return true;
}
