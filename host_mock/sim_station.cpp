/*
 * sim_station — live scenario server.
 *
 * Plays a host_mock JSON scenario on a slot clock and serves the Operate
 * and Log routes the web app already polls. USB, CAT, and audio are not
 * simulated: cat_ready and audio_rx are simply reported up so the page
 * renders, and one USB-present event names a QMX.
 *
 *   ./sim_station [--scale N] [--port N] scenario.json
 *
 * --scale divides the 15 s period. 1 is real time. The page's own clock
 * is not replaced: slot indexes come from the wall clock.
 */

#include "scenario.h"

#include "autoseq.h"
#include "porta_proto.h"
#include "swr_sweep.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <sstream>
#include <string>
#include <vector>

static const char* kToken = "sim-station-token";

static std::map<std::string, std::string> g_cfg;
static std::string g_log_file;
static Scenario g_scenario;

static std::string json_escape(const std::string& in)
{
    std::string out;
    for (unsigned char c : in) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back((char)c);
        } else if (c < 0x20 || c == 0x7F) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out.push_back((char)c);
        }
    }
    return out;
}

static int cq_type_number(const std::string& name)
{
    if (name == "SOTA") return 1;
    if (name == "POTA") return 2;
    if (name == "QRP") return 3;
    if (name == "FD") return 4;
    if (name == "FREETEXT") return 5;
    return 0;
}

static void seed_config(const TestData& td)
{
    g_cfg["call"] = td.config.my_callsign;
    g_cfg["grid"] = td.config.my_grid;
    g_cfg["cq_type"] = std::to_string(cq_type_number(td.config.cq_type));
    g_cfg["free_text"] = td.config.free_text;
    g_cfg["tries"] = std::to_string(td.config.tries);
    g_cfg["offset"] = "1500";
    g_cfg["offset_src"] = "0";
    g_cfg["tx_min_delay_ms"] = "50";
    g_cfg["amp_swr_guard"] = "0";
    g_cfg["amp_prime_on_band_change"] = "0";
    g_cfg["band_sel"] = "5";
    g_cfg["protocol_mode"] = "FT8";
    g_cfg["active_bands"] = "40 30 20 17 15 12 10";
    g_cfg["act_refs"] = "";
    g_cfg["cat_ready"] = "1";
    g_cfg["audio_rx"] = "1";
    g_cfg["tune"] = "0";
    g_cfg["sk_fw_version"] = "sim";
    g_cfg["sk_wifi_mode"] = "sim";
    g_cfg["sk_network_name"] = "simulator";
    g_cfg["sk_ip"] = "127.0.0.1";
    g_cfg["sk_uptime"] = "0";
    g_cfg["sk_crc_errors"] = "0";
    g_cfg["sk_frames_ok"] = "0";
    g_cfg["sk_ring_gaps"] = "0";
    g_cfg["sk_config_retries"] = "0";

    char day[16];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    snprintf(day, sizeof(day), "%04d%02d%02d.adi", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    g_log_file = day;
}

static std::string config_json()
{
    std::string body = "{\"ok\":true";
    for (const auto& kv : g_cfg) {
        body += ",\"";
        body += json_escape(kv.first);
        body += "\":\"";
        body += json_escape(kv.second);
        body += "\"";
    }
    body += "}";
    return body;
}

static std::string events_json(uint32_t since)
{
    std::string body = "{\"events\":[";
    bool first = true;
    for (size_t i = 0; i < g_scenario.events.size(); ++i) {
        const uint32_t seq = (uint32_t)i + 1u;
        if (seq < since) {
            continue;
        }
        if (!first) {
            body += ",";
        }
        first = false;
        body += g_scenario.events[i];
    }
    body += "],\"seq\":";
    body += std::to_string(g_scenario.next_seq);
    body += "}";
    return body;
}

static std::string log_files_json()
{
    if (g_scenario.log.empty()) {
        return "{\"ok\":true,\"files\":[]}";
    }
    return "{\"ok\":true,\"files\":[\"" + g_log_file + "\"]}";
}

static std::string log_entries_json(const std::string& file, unsigned skip, unsigned take)
{
    if (file != g_log_file) {
        return "{\"ok\":false,\"why\":\"no such file\"}";
    }
    std::string body = "{\"ok\":true,\"entries\":[";
    bool first = true;
    unsigned seen = 0;
    unsigned emitted = 0;
    for (auto it = g_scenario.log.begin(); it != g_scenario.log.end(); ++it) {
        if (seen++ < skip) {
            continue;
        }
        if (emitted >= take) {
            break;
        }
        if (!first) {
            body += ",";
        }
        first = false;
        emitted++;
        const SimLogEntry& e = *it;
        body += "{\"time\":\"" + json_escape(e.time_on) + "\",\"band\":\"20m\",\"call\":\"";
        body += json_escape(e.call);
        body += "\",\"rst_sent\":";
        body += std::to_string(e.rst_sent);
        body += ",\"rst_rcvd\":";
        body += std::to_string(e.rst_rcvd);
        body += ",\"grid\":\"" + json_escape(e.grid) + "\",\"freq\":\"14.074\",\"my_grid\":\"";
        body += json_escape(e.my_grid);
        body += "\",\"comment\":\"\",\"mode\":\"FT8\",\"station_callsign\":\"";
        body += json_escape(e.station_call);
        body += "\",\"my_pota_ref\":\"\",\"my_sota_ref\":\"\"}";
    }
    body += "]}";
    return body;
}

static std::map<std::string, std::string> parse_query(const std::string& q)
{
    std::map<std::string, std::string> out;
    size_t i = 0;
    while (i < q.size()) {
        const size_t amp = q.find('&', i);
        const std::string pair = q.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            out[pair.substr(0, eq)] = pair.substr(eq + 1);
        }
        if (amp == std::string::npos) {
            break;
        }
        i = amp + 1;
    }
    return out;
}

static void apply_config_body(const std::string& body)
{
    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        g_cfg[key] = value;
    }
    scenario_set_station(g_scenario, g_cfg["call"], g_cfg["grid"]);
    scenario_set_cq_type(g_scenario, std::atoi(g_cfg["cq_type"].c_str()), g_cfg["free_text"]);
    if (!g_cfg["tries"].empty()) {
        scenario_set_tries(g_scenario, std::atoi(g_cfg["tries"].c_str()));
    }
}

// A fake QMX for the SWR sweep, so the real state machine (components/swr_sweep) runs behind the
// page without a radio. It has the bands 40, 30, 20, 17, 15, 12 and 10 m (others are refused, as a
// real QMX refuses a band it is not configured for), and reads like a 40 m end-fed half-wave:
// low on 40, 20, 15 and 10 m, high on 30, 17 and 12 m.
namespace {
struct SimBand {
    int lo_khz, hi_khz;
    double base, bowl;  // SWR at the band's middle, and how much it rises toward the edges
};
const SimBand kSimBands[] = {
    {7000, 7300, 1.15, 0.30},   {10100, 10150, 2.60, 0.10}, {14000, 14350, 1.35, 0.10},
    {18068, 18168, 2.30, 0.15}, {21000, 21450, 1.55, 0.20}, {24890, 24990, 2.80, 0.10},
    {28000, 29700, 1.70, 0.25},
};
int g_sim_fa = 14074000;
bool g_sim_keyed = false;
int g_sim_fa_reply = -1;
int g_sim_sw_reply = -1;

const SimBand* sim_band_for(int hz)
{
    for (const auto& b : kSimBands) {
        if (hz >= b.lo_khz * 1000 && hz <= b.hi_khz * 1000) {
            return &b;
        }
    }
    return nullptr;
}

int sim_swr_hundredths(int hz)
{
    const SimBand* b = sim_band_for(hz);
    if (!b) {
        return 900;
    }
    const double pos = ((double)hz / 1000.0 - b->lo_khz) / (double)(b->hi_khz - b->lo_khz);
    const double off = pos - 0.5;
    const unsigned jitter = (unsigned)(hz / 1000) * 2654435761u;  // deterministic wobble, +-0.02
    return (int)((b->base + b->bowl * 4.0 * off * off + ((int)(jitter >> 28) - 8) * 0.0025) * 100.0);
}

esp_err_t sim_unkey() { g_sim_keyed = false; return ESP_OK; }
esp_err_t sim_mode() { g_sim_keyed = false; return ESP_OK; }
esp_err_t sim_set(int hz)
{
    if (!g_sim_keyed && sim_band_for(hz)) {
        g_sim_fa = hz;
    }
    return ESP_OK;
}
esp_err_t sim_qfreq() { g_sim_fa_reply = g_sim_fa; return ESP_OK; }
esp_err_t sim_key() { g_sim_keyed = true; return ESP_OK; }
esp_err_t sim_qswr()
{
    if (g_sim_keyed) {
        g_sim_sw_reply = sim_swr_hundredths(g_sim_fa);
    }
    return ESP_OK;
}
bool sim_freply(int* hz)
{
    if (g_sim_fa_reply < 0) return false;
    *hz = g_sim_fa_reply;
    return true;
}
bool sim_sreply(int* v)
{
    if (g_sim_sw_reply < 0) return false;
    *v = g_sim_sw_reply;
    return true;
}
void sim_clear() { g_sim_fa_reply = -1; g_sim_sw_reply = -1; }
esp_err_t sim_restore(int hz) { g_sim_fa = hz; g_sim_keyed = false; return ESP_OK; }

const radio_swr_io_t kSimIo = {sim_unkey, sim_mode, sim_set, sim_qfreq, sim_key, sim_qswr,
                               sim_freply, sim_sreply, sim_clear, sim_restore};

void sim_sweep_event(const swr_sweep_event_t* e, void*)
{
    uint8_t kind = PORTA_SWEEP_DONE;
    switch (e->kind) {
        case SWR_SWEEP_EVENT_POINT: kind = PORTA_SWEEP_POINT; break;
        case SWR_SWEEP_EVENT_BAND_SKIPPED: kind = PORTA_SWEEP_BAND_SKIPPED; break;
        case SWR_SWEEP_EVENT_DONE: kind = PORTA_SWEEP_DONE; break;
        case SWR_SWEEP_EVENT_ABORTED: kind = PORTA_SWEEP_ABORTED; break;
    }
    scenario_emit_sweep(g_scenario, kind, e->range_index, e->freq_hz, e->swr_hundredths, (uint8_t)e->abort_reason);
}

// POST /api/swr/start, body "lo-hi-step,lo-hi-step" in kHz: what the sidekick takes.
std::string sim_sweep_start(const std::string& body)
{
    // What the P4 refuses over (poe_p4/main/porta_host.cpp), so the page's ask-first flow
    // has something to ask about.
    if (autoseq_cq_entry_id() != 0) return "{\"ok\":false,\"why\":\"a CQ is queued\"}";
    swr_sweep_range_t ranges[SWR_SWEEP_MAX_RANGES + 1];
    size_t count = 0;
    const char* p = body.c_str();
    while (*p && count <= SWR_SWEEP_MAX_RANGES) {
        char* end = nullptr;
        const unsigned long lo = std::strtoul(p, &end, 10);
        if (end == p || *end != '-') return "{\"ok\":false,\"why\":\"malformed\"}";
        p = end + 1;
        const unsigned long hi = std::strtoul(p, &end, 10);
        if (end == p || *end != '-') return "{\"ok\":false,\"why\":\"malformed\"}";
        p = end + 1;
        const unsigned long step = std::strtoul(p, &end, 10);
        if (end == p) return "{\"ok\":false,\"why\":\"malformed\"}";
        ranges[count++] = {(uint32_t)lo, (uint32_t)hi, (uint32_t)step};
        p = end;
        if (*p == ',') ++p;
    }
    switch (swr_sweep_request_start(ranges, count, 14074000u, &kSimIo, sim_sweep_event, nullptr)) {
        case SWR_SWEEP_START_OK: return "{\"ok\":true}";
        case SWR_SWEEP_START_BAD_RANGE: return "{\"ok\":false,\"why\":\"bad range\"}";
        case SWR_SWEEP_START_TOO_MANY: return "{\"ok\":false,\"why\":\"too many points\"}";
        case SWR_SWEEP_START_BUSY: return "{\"ok\":false,\"why\":\"a sweep is already running\"}";
        case SWR_SWEEP_START_UNSUPPORTED: return "{\"ok\":false,\"why\":\"this radio cannot sweep\"}";
    }
    return "{\"ok\":false,\"why\":\"sweep refused\"}";
}
}  // namespace

static std::string handle(const std::string& method, const std::string& url,
                          const std::map<std::string, std::string>& headers,
                          const std::string& body, int* status)
{
    *status = 200;
    const size_t qpos = url.find('?');
    const std::string path = url.substr(0, qpos);
    const auto query = parse_query(qpos == std::string::npos ? "" : url.substr(qpos + 1));

    auto token_ok = [&]() {
        auto it = headers.find("x-microft8-token");
        return it != headers.end() && it->second == kToken;
    };
    const bool write = method == "POST" || method == "PUT" || method == "DELETE";
    if (write && path != "/api/pairing-token" && !token_ok()) {
        *status = 401;
        return "{\"ok\":false,\"why\":\"unauthorized\"}";
    }

    // The sidekick's /api/state is its snapshot of the station now; the simulator
    // keeps every event it played, so the whole log is that state, and the page
    // keeps its own DECODE_SLOTS_MAX of the slots in it.
    if (method == "GET" && path == "/api/state") {
        return events_json(0);
    }
    if (method == "GET" && path == "/api/events") {
        const unsigned long since = query.count("since") ? std::strtoul(query.at("since").c_str(), nullptr, 10) : 0;
        return events_json((uint32_t)since);
    }
    if (method == "GET" && path == "/api/config") {
        return config_json();
    }
    if (method == "PUT" && path == "/api/config") {
        apply_config_body(body);
        return "{\"ok\":true}";
    }
    if (method == "GET" && path == "/api/pairing-token") {
        return std::string("{\"token\":\"") + kToken + "\"}";
    }
    if (method == "POST" && path == "/api/queue/reply") {
        const unsigned long id = std::strtoul(body.c_str(), nullptr, 10);
        if (id == 0 || !scenario_reply(g_scenario, (uint32_t)id)) {
            *status = 400;
            return "{\"ok\":false,\"why\":\"unknown decode\"}";
        }
        return "{\"ok\":true}";
    }
    if (method == "POST" && path == "/api/queue/cancel") {
        const unsigned long id = std::strtoul(body.c_str(), nullptr, 10);
        if (id == 0 || !scenario_cancel_entry(g_scenario, (uint16_t)id)) {
            *status = 400;
            return "{\"ok\":false,\"why\":\"unknown entry\"}";
        }
        return "{\"ok\":true}";
    }
    if (method == "POST" && (path == "/api/radio/read" || path == "/api/radio/connect")) {
        // The simulated radio is always where Micro-FT8 put it.
        g_cfg["radio_fw"] = "1_04_004";
        g_cfg["want_hz"] = "7074000";
        g_cfg["radio_hz"] = "7074000";
        g_cfg["radio_tx"] = "0";
        g_cfg["radio_mode"] = "6";
        g_cfg["radio_split"] = "0";
        return "{\"ok\":true}";
    }
    if (method == "POST" && path == "/api/radio/tune") {
        g_cfg["tune"] = (body == "1" || body == "on") ? "1" : "0";
        return "{\"ok\":true}";
    }
    if (method == "POST" && path == "/api/cq") {
        if (!scenario_call_cq(g_scenario)) {
            return "{\"ok\":false,\"why\":\"a CQ is already queued\"}";
        }
        return "{\"ok\":true}";
    }
    if (method == "POST" && (path == "/api/queue/tries" || path == "/api/queue/parity")) {
        // "<entry_id> <n>", as the sidekick takes them.
        char* end = nullptr;
        const unsigned long id = std::strtoul(body.c_str(), &end, 10);
        const long n = std::strtol(end, nullptr, 10);
        const bool ok = path == "/api/queue/tries"
                            ? scenario_set_entry_tries(g_scenario, (uint16_t)id, (int)n)
                            : scenario_set_cq_parity(g_scenario, (uint16_t)id, (int)n);
        if (id == 0 || !ok) {
            *status = 400;
            return "{\"ok\":false,\"why\":\"refused\"}";
        }
        return "{\"ok\":true}";
    }
    if (method == "POST" && path == "/api/tx/cancel") {
        scenario_cancel_tx(g_scenario);
        swr_sweep_request_cancel();
        return "{\"ok\":true}";
    }
    if (method == "POST" && path == "/api/swr/start") {
        return sim_sweep_start(body);
    }
    if (method == "GET" && path == "/api/log/files") {
        return log_files_json();
    }
    if (method == "GET" && path == "/api/log/entries") {
        const unsigned skip = query.count("skip") ? (unsigned)std::strtoul(query.at("skip").c_str(), nullptr, 10) : 0;
        unsigned take = query.count("take") ? (unsigned)std::strtoul(query.at("take").c_str(), nullptr, 10) : 6;
        if (take == 0) {
            take = 6;
        }
        const std::string file = query.count("file") ? query.at("file") : "";
        return log_entries_json(file, skip, take);
    }
    *status = 404;
    return "{\"ok\":false,\"why\":\"not found\"}";
}

static void send_response(int fd, int status, const std::string& body)
{
    char head[256];
    const char* text = "OK";
    if (status == 400) text = "Bad Request";
    else if (status == 401) text = "Unauthorized";
    else if (status == 404) text = "Not Found";
    const int n = snprintf(head, sizeof(head),
                           "HTTP/1.1 %d %s\r\n"
                           "Content-Type: application/json\r\n"
                           "Cache-Control: no-store\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           status, text, body.size());
    if (n > 0) {
        send(fd, head, (size_t)n, 0);
    }
    send(fd, body.data(), body.size(), 0);
}

static std::string header_get(const std::map<std::string, std::string>& h, const char* key)
{
    auto it = h.find(key);
    return it == h.end() ? "" : it->second;
}

static bool serve_client(int fd)
{
    std::string data;
    char buf[1024];
    while (data.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            return false;
        }
        data.append(buf, (size_t)n);
        if (data.size() > 65536) {
            return false;
        }
    }
    const size_t hdr_end = data.find("\r\n\r\n");
    const std::string head = data.substr(0, hdr_end);
    std::string body = data.substr(hdr_end + 4);

    std::istringstream lines(head);
    std::string request;
    std::getline(lines, request);
    if (!request.empty() && request.back() == '\r') {
        request.pop_back();
    }
    std::string method, url, version;
    {
        std::istringstream rl(request);
        rl >> method >> url >> version;
    }
    std::map<std::string, std::string> headers;
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') {
            value.erase(value.begin());
        }
        for (char& c : key) {
            if (c >= 'A' && c <= 'Z') {
                c = (char)(c - 'A' + 'a');
            }
        }
        headers[key] = value;
    }
    const unsigned long need = std::strtoul(header_get(headers, "content-length").c_str(), nullptr, 10);
    while (body.size() < need) {
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        body.append(buf, (size_t)n);
    }
    if (body.size() > need) {
        body.resize(need);
    }
    int status = 200;
    const std::string response = handle(method, url, headers, body, &status);
    send_response(fd, status, response);
    return true;
}

int main(int argc, char** argv)
{
    int scale = 1;
    int port = 8790;
    const char* scenario_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (!scenario_path) {
            scenario_path = argv[i];
        }
    }
    if (!scenario_path) {
        fprintf(stderr, "usage: sim_station [--scale N] [--port N] scenario.json\n");
        return 1;
    }
    if (scale < 1) {
        scale = 1;
    }
    int interval_ms = 15000 / scale;
    if (interval_ms < 1) {
        interval_ms = 1;
    }

    TestData td;
    if (!load_test_data(scenario_path, td)) {
        fprintf(stderr, "failed to load %s\n", scenario_path);
        return 1;
    }
    seed_config(td);
    scenario_begin(g_scenario, td);
    g_scenario.wall_slots = true;
    g_scenario.check_expect = false;
    scenario_emit_usb(g_scenario, true);

    const int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(0x7f000001);
    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(listen_fd, 8) < 0) {
        perror("listen");
        return 1;
    }
    printf("sim_station listening on 127.0.0.1:%d  scale=%d  %s\n", port, scale, scenario_path);
    fflush(stdout);

    using clock = std::chrono::steady_clock;
    auto next_step = clock::now();
    auto next_beat = next_step + std::chrono::seconds(1);
    bool script_done = false;

    while (true) {
        const auto now = clock::now();
        if (now >= next_step) {
            // Past the script, periods keep running empty, so a CQ called
            // from the page goes out and counts its tries.
            if (script_done) {
                scenario_idle_step(g_scenario);
            } else if (!scenario_step(g_scenario)) {
                script_done = true;
            }
            next_step = clock::now() + std::chrono::milliseconds(interval_ms);
        }
        {
            // The sweep's clock: real time times --scale, but at most 100 ms a turn, so the
            // stall watchdog (a tick over a second late) cannot fire in a fast-forwarded run.
            static auto last_tick = clock::now();
            static int64_t sweep_ms = 0;
            const int64_t real = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - last_tick).count();
            last_tick = clock::now();
            sweep_ms += std::min<int64_t>(real * scale, 100);
            swr_sweep_tick(sweep_ms);
        }
        if (now >= next_beat) {
            scenario_emit_status(g_scenario);
            scenario_emit_hud(g_scenario);
            next_beat = clock::now() + std::chrono::seconds(1);
        }

        auto wake = next_beat;
        if (swr_sweep_busy()) {
            wake = clock::now() + std::chrono::milliseconds(5);
        }
        if (next_step < wake) {
            wake = next_step;
        }
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(wake - clock::now());
        timeval tv;
        tv.tv_sec = wait.count() > 0 ? (long)(wait.count() / 1000) : 0;
        tv.tv_usec = wait.count() > 0 ? (long)((wait.count() % 1000) * 1000) : 0;

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listen_fd, &fds);
        const int ready = select(listen_fd + 1, &fds, nullptr, nullptr, &tv);
        if (ready > 0 && FD_ISSET(listen_fd, &fds)) {
            const int client = accept(listen_fd, nullptr, nullptr);
            if (client >= 0) {
                serve_client(client);
                close(client);
            }
        }
    }
}
