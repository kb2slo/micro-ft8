#include <cstdio>
#include <cstring>
#include <string>

#include "station.h"

static int g_fails = 0;

static void fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    ++g_fails;
}

static void expect_true(bool cond, const char* msg)
{
    if (!cond) {
        fail(msg);
    }
}

static void expect_int(int got, int want, const char* msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %d want %d)\n", msg, got, want);
        ++g_fails;
    }
}

static void expect_str(const std::string& got, const char* want, const char* msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got '%s' want '%s')\n", msg, got.c_str(), want);
        ++g_fails;
    }
}

static void expect_float(float got, float want, const char* msg)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s (got %f want %f)\n", msg, got, want);
        ++g_fails;
    }
}

int main()
{
    {
        StationSettings s;
        station_settings_init(&s);
        station_parse("", &s);
        expect_str(s.call, "", "empty call");
        expect_int(s.radio, 2, "default radio QMX");
        expect_int(s.gps_baud, 115200, "default gps baud");
    }

    {
        StationSettings s;
        station_settings_init(&s);
        station_parse(
            "call= kb2slo\n"
            "grid=fn30pr\n"
            "date=2026-08-19\n"
            "time=18:04:01\n"
            "rtc_comp=120\n"
            "rtc_sleep_epoch=1735689600\n"
            "band5=14074\n"
            "radio=QDX\n"
            "cq_type=4\n"
            "beacon=1\n"
            "unknown_key=nope\n",
            &s);
        expect_str(s.call, "KB2SLO", "call upper");
        expect_str(s.grid, "FN30pr", "grid maidenhead");
        expect_true(s.band_freq_set[5], "band5 set");
        expect_float(s.band_freq[5], 14074.0f, "band5 freq");
        expect_int(s.radio, 5, "radio QDX");
        expect_int(s.cq_type, 4, "cq FD");
        expect_true(!s.protocol_ft4, "beacon must not imply ft4");
        {
            const std::string text = station_serialize(s);
            expect_true(text.find("date=") == std::string::npos, "old date= not rewritten");
            expect_true(text.find("time=") == std::string::npos, "old time= not rewritten");
            expect_true(text.find("rtc_comp=") == std::string::npos, "old rtc_comp not rewritten");
            expect_true(text.find("rtc_sleep_epoch=") == std::string::npos,
                        "old rtc_sleep_epoch not rewritten");
        }
    }

    {
        StationSettings s;
        station_settings_init(&s);
        station_parse("protocol_mode=FT4\nft4_band5=14080\nband5=14074\n", &s);
        expect_true(s.protocol_ft4, "ft4 mode");
        expect_true(s.ft4_band_freq_set[5], "ft4 band5");
        expect_float(s.ft4_band_freq[5], 14080.0f, "ft4 freq");
        expect_true(s.band_freq_set[5], "ft8 band5 also parsed");
        expect_float(s.band_freq[5], 14074.0f, "ft8 freq kept");
        s.serialize_ft4_band_keys = true;
        const std::string text = station_serialize(s);
        expect_true(text.find("protocol_mode=FT4") != std::string::npos, "serialize ft4");
        expect_true(text.find("ft4_band5=14080") != std::string::npos, "serialize ft4 band");
        expect_true(text.find("band5=14074") == std::string::npos, "serialize omits inactive prefix");
        expect_true(text.find("beacon=") == std::string::npos, "no beacon line");
        expect_true(text.find("date=") == std::string::npos, "no date line");
        expect_true(text.find("time=") == std::string::npos, "no time line");
        expect_true(text.find("rtc_comp=") == std::string::npos, "no rtc_comp");
        expect_true(text.find("rtc_sleep_epoch=") == std::string::npos, "no rtc_sleep_epoch");
    }

    {
        StationSettings s;
        station_settings_init(&s);
        s.call = "KB2SLO";
        s.grid = "FN30";
        s.band_freq[0] = 1840.0f;
        s.band_freq_set[0] = true;
        s.radio = 2;
        const std::string text = station_serialize(s);
        StationSettings round;
        station_settings_init(&round);
        station_parse(text, &round);
        expect_str(round.call, "KB2SLO", "round-trip call");
        expect_str(round.grid, "FN30", "round-trip grid");
        expect_float(round.band_freq[0], 1840.0f, "round-trip band0");
        expect_int(round.radio, 2, "round-trip radio");
    }

    {
        StationSettings s;
        station_settings_init(&s);
        // gps_source / gnss_lora are gone with Grove GPS; an old Station.txt
        // that still carries them must load without complaint rather than
        // failing, so the surviving legacy key is checked alongside them.
        station_parse("gps_source=2\ngnss_lora=1\nactive_band=5\n", &s);
        expect_str(s.active_bands, "5", "legacy active_band");
    }

    {
        // act_refs (RFC 0008 §4.2): round-trips, upper-cases both halves of
        // a token, and drops a malformed token on its own rather than
        // rejecting the whole write -- a list, unlike normalize_grid_maidenhead()'s
        // all-or-nothing single value.
        StationSettings s;
        station_settings_init(&s);
        station_parse("act_refs=pota:us-1234,sota:w7o/cc-001\n", &s);
        expect_str(s.act_refs, "POTA:US-1234,SOTA:W7O/CC-001", "act_refs upper-cased both halves");

        station_parse("act_refs=POTA:US-1234,garbage,SOTA:W7O/CC-001\n", &s);
        expect_str(s.act_refs, "POTA:US-1234,SOTA:W7O/CC-001", "a malformed token is dropped, not the whole write");

        station_parse("act_refs=DXCC:K1ABC\n", &s);
        expect_str(s.act_refs, "", "an unknown program is dropped, leaving nothing");

        station_parse("act_refs=POTA:US-9999\n", &s);
        station_parse("act_refs=\n", &s);
        expect_str(s.act_refs, "", "an explicit empty value clears the activation");

        const std::string text = station_serialize(s);
        expect_true(text.find("act_refs=") != std::string::npos, "act_refs is serialized");
    }

    // station_key_names_operating_band (docs/architecture/webapp.md, "The
    // dock"): does this CONFIG key need the VFO already on the operating band
    // to take effect right now.
    expect_true(station_key_names_operating_band("band_sel", 5, false), "band_sel always names the operating band");
    expect_true(station_key_names_operating_band("band_sel", 0, true), "band_sel regardless of protocol");
    expect_true(station_key_names_operating_band("band5", 5, false), "FT8 freq for the operating band");
    expect_true(!station_key_names_operating_band("band5", 5, true), "FT8 key while FT4 is active is not operating");
    expect_true(station_key_names_operating_band("ft4_band5", 5, true), "FT4 freq for the operating band");
    expect_true(!station_key_names_operating_band("ft4_band5", 5, false), "FT4 key while FT8 is active is not operating");
    expect_true(!station_key_names_operating_band("band3", 5, false), "a different band's frequency is not operating");
    expect_true(!station_key_names_operating_band("active_bands", 5, false), "active_bands never names the operating band");
    expect_true(!station_key_names_operating_band("call", 5, false), "an unrelated key is never the operating band");
    expect_true(!station_key_names_operating_band(nullptr, 5, false), "a null key is never the operating band");

    // station_vfo_refusal: a refused band change says "band", a refused
    // frequency write says "frequency", each with the reason.
    expect_str(station_vfo_refusal("band_sel", true), "band saved, VFO not moved (TX active)", "band_sel, TX active");
    expect_str(station_vfo_refusal("band_sel", false), "band saved, VFO not moved (CAT not ready)", "band_sel, CAT not ready");
    expect_str(station_vfo_refusal("band5", true), "frequency saved, VFO not moved (TX active)", "frequency, TX active");
    expect_str(station_vfo_refusal("ft4_band5", false), "frequency saved, VFO not moved (CAT not ready)", "FT4 frequency, CAT not ready");

    if (g_fails) {
        fprintf(stderr, "%d FAIL(s)\n", g_fails);
        return 1;
    }
    expect_true(station_key_known("call"), "call known");
    expect_true(station_key_known("band0"), "band0 known");
    expect_true(station_key_known("ft4_band3"), "ft4_band3 known");
    expect_true(!station_key_known("beacon"), "beacon not a CONFIG key");
    expect_true(!station_key_known("streaming"), "live streaming is GET-only");
    expect_true(!station_key_known("cat_ready"), "live cat_ready is GET-only");
    expect_true(!station_key_known("tune"), "live tune is GET-only");
    expect_true(!station_key_known("band_name"), "live band_name is GET-only");
    expect_true(!station_key_known("freq_khz"), "live freq_khz is GET-only");
    expect_true(!station_key_known(""), "empty unknown");
    printf("PASS: station parse/serialize\n");
    return 0;
}
