#include <cstdio>
#include <string>
#include <vector>

#include "amp_band_prime.h"

namespace {

int fails = 0;
std::vector<std::string> calls;
amp_band_prime_event_t last_event = {};
int event_count = 0;
bool fail_key = false;

void expect(bool condition, const char* message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++fails;
    }
}

esp_err_t key()
{
    calls.push_back("MD8");
    return fail_key ? ESP_FAIL : ESP_OK;
}

esp_err_t unkey()
{
    calls.push_back("RX");
    return ESP_OK;
}

esp_err_t digital()
{
    calls.push_back("MD6");
    return ESP_OK;
}

void on_event(const amp_band_prime_event_t* event, void*)
{
    last_event = *event;
    ++event_count;
}

radio_swr_io_t io = {unkey, digital, nullptr, nullptr, key, nullptr, nullptr, nullptr, nullptr, nullptr};

void reset_observation()
{
    calls.clear();
    last_event = {};
    event_count = 0;
    fail_key = false;
}

}  // namespace

int main()
{
    expect(amp_band_prime_request_start(nullptr, on_event, nullptr) ==
               AMP_BAND_PRIME_START_UNSUPPORTED,
           "a radio without MD8 is refused");

    reset_observation();
    expect(amp_band_prime_request_start(&io, on_event, nullptr) == AMP_BAND_PRIME_START_OK,
           "prime request accepted");
    expect(amp_band_prime_busy(), "accepted request is busy before its first tick");
    expect(amp_band_prime_request_start(&io, on_event, nullptr) == AMP_BAND_PRIME_START_BUSY,
           "a second request is refused");
    amp_band_prime_tick(1000);
    expect(calls == std::vector<std::string>{"MD8"}, "first tick enters SWR Tune");
    amp_band_prime_tick(1000 + AMP_BAND_PRIME_PULSE_MS - 1);
    expect(calls.size() == 1, "pulse remains keyed for its full duration");
    amp_band_prime_tick(1000 + AMP_BAND_PRIME_PULSE_MS);
    expect(calls == std::vector<std::string>({"MD8", "RX"}), "pulse ends with RX");
    amp_band_prime_tick(1000 + AMP_BAND_PRIME_PULSE_MS + 10);
    expect(calls == std::vector<std::string>({"MD8", "RX", "MD6"}),
           "digital mode is restored after RX");
    expect(event_count == 1 && last_event.kind == AMP_BAND_PRIME_DONE, "completion is reported");
    expect(!amp_band_prime_busy() && !amp_band_prime_active(), "completion returns to idle");

    reset_observation();
    fail_key = true;
    expect(amp_band_prime_request_start(&io, on_event, nullptr) == AMP_BAND_PRIME_START_OK,
           "failed-key request accepted");
    amp_band_prime_tick(2000);
    amp_band_prime_tick(2010);
    amp_band_prime_tick(2020);
    expect(calls == std::vector<std::string>({"MD8", "RX", "MD6"}),
           "a failed key is still followed by RX and MD6");
    expect(event_count == 1 && last_event.kind == AMP_BAND_PRIME_ABORTED &&
               last_event.abort_reason == AMP_BAND_PRIME_ABORT_CAT_LOST,
           "a failed CAT key aborts");

    reset_observation();
    expect(amp_band_prime_request_start(&io, on_event, nullptr) == AMP_BAND_PRIME_START_OK,
           "stall request accepted");
    amp_band_prime_tick(3000);
    amp_band_prime_tick(4101);
    amp_band_prime_tick(4111);
    expect(calls == std::vector<std::string>({"MD8", "RX", "MD6"}),
           "a stalled keyed pulse is de-keyed and restored");
    expect(event_count == 1 && last_event.kind == AMP_BAND_PRIME_ABORTED &&
               last_event.abort_reason == AMP_BAND_PRIME_ABORT_STALLED,
           "a delayed tick reports a stall");

    reset_observation();
    expect(amp_band_prime_request_start(&io, on_event, nullptr) == AMP_BAND_PRIME_START_OK,
           "cancel request accepted");
    amp_band_prime_tick(5000);
    amp_band_prime_request_cancel();
    amp_band_prime_tick(5010);
    amp_band_prime_tick(5020);
    expect(calls == std::vector<std::string>({"MD8", "RX", "MD6"}),
           "cancel de-keys and restores digital mode");
    expect(event_count == 1 && last_event.kind == AMP_BAND_PRIME_ABORTED &&
               last_event.abort_reason == AMP_BAND_PRIME_ABORT_CANCELLED,
           "cancel is reported");

    if (fails) {
        fprintf(stderr, "%d amp band-prime test(s) failed\n", fails);
        return 1;
    }
    printf("amp band-prime tests passed\n");
    return 0;
}
