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
        expect_int(s.tx_min_delay_ms, 50, "default min TX delay");
    }

    {
        StationSettings s;
        station_settings_init(&s);
        station_parse("tx_min_delay_ms=200\n", &s);
        expect_int(s.tx_min_delay_ms, 200, "min TX delay parsed");
        station_parse("tx_min_delay_ms=501\n", &s);
        expect_int(s.tx_min_delay_ms, 200, "above 500 ignored");
        station_parse("tx_min_delay_ms=-1\n", &s);
        expect_int(s.tx_min_delay_ms, 200, "negative ignored");
        station_parse("tx_min_delay_ms=0\n", &s);
        expect_int(s.tx_min_delay_ms, 0, "zero is allowed");
        station_parse("tx_min_delay_ms=abc\n", &s);
        expect_int(s.tx_min_delay_ms, 0, "non-number ignored");
        station_parse("tx_min_delay_ms=500\n", &s);
        StationSettings t;
        station_settings_init(&t);
        station_parse(station_serialize(s), &t);
        expect_int(t.tx_min_delay_ms, 500, "min TX delay round-trips");
        expect_true(station_key_known("tx_min_delay_ms"), "key known to CONFIG");
    }

    {
        StationSettings s;
        station_settings_init(&s);
        // The amplifier switch: off by default, and off, nothing an amplifier adds is in force
        // whatever is saved; on, the saved values are.
        expect_true(!s.amp, "no amplifier by default");
        expect_true(station_key_known("amp"), "amp known to CONFIG");
        station_parse("amp_swr_guard=1\namp_prime_on_band_change=1\ntx_min_delay_ms=200\n", &s);
        expect_true(!station_amp_swr_guard(s), "amp off: no SWR trip recovery though saved on");
        expect_true(!station_amp_prime_on_band_change(s), "amp off: no band-change prime though saved on");
        expect_int(station_tx_min_delay_ms(s), kStationTxMinDelayDefaultMs, "amp off: the default TX delay");
        station_parse("amp=1\n", &s);
        expect_true(s.amp && station_amp_swr_guard(s) && station_amp_prime_on_band_change(s), "amp on: the saved options are in force");
        expect_int(station_tx_min_delay_ms(s), 200, "amp on: the saved TX delay");
        {
            StationSettings a;
            station_settings_init(&a);
            station_parse(station_serialize(s), &a);
            expect_true(a.amp, "amp round-trips");
            expect_int(a.tx_min_delay_ms, 200, "and the saved delay with it");
        }
        station_parse("amp=0\n", &s);
        expect_true(!station_amp_swr_guard(s) && s.amp_swr_guard, "amp off again: kept, not in force");
        station_settings_init(&s);
        expect_true(!s.amp_swr_guard, "SWR protection recovery is off by default");
        expect_true(!s.amp_prime_on_band_change, "amp band-change prime is off by default");
        station_parse("amp_swr_guard=1\n", &s);
        expect_true(s.amp_swr_guard, "amp_swr_guard parsed");
        station_parse("amp_prime_on_band_change=1\n", &s);
        expect_true(s.amp_prime_on_band_change, "amp_prime_on_band_change parsed");
        StationSettings t;
        station_settings_init(&t);
        station_parse(station_serialize(s), &t);
        expect_true(t.amp_swr_guard, "amp_swr_guard round-trips");
        expect_true(t.amp_prime_on_band_change, "amp_prime_on_band_change round-trips");
        station_parse("amp_swr_guard=0\n", &s);
        expect_true(!s.amp_swr_guard, "and turns off");
        expect_true(station_key_known("amp_swr_guard"), "key known to CONFIG");
        expect_true(s.amp_ptt == kStationAmpPttNone, "amp PTT keying is none by default");
        station_parse("amp_ptt=1\n", &s);
        expect_true(s.amp_ptt == kStationAmpPttGround, "amp_ptt=1 is TX PTT grounded");
        StationSettings p;
        station_settings_init(&p);
        station_parse(station_serialize(s), &p);
        expect_true(p.amp_ptt == kStationAmpPttGround, "amp_ptt round-trips");
        station_parse("amp_ptt=2\n", &s);
        expect_true(s.amp_ptt == kStationAmpPtt5v, "amp_ptt=2 is TX PTT +5V");
        station_parse("amp_ptt=7\n", &s);
        expect_true(s.amp_ptt == kStationAmpPttNone, "an unknown amp_ptt reads as none");
        expect_true(station_key_known("amp_ptt"), "amp_ptt known to CONFIG");
        // The channels each band listens to (listen<N>): FT8 alone by default, and left unwritten then.
        for (int b = 0; b < kStationBandCount; ++b) {
            expect_true(station_listen(s, b) == kStationListenFt8, "FT8 alone on every band by default");
        }
        expect_true(station_serialize(s).find("listen") == std::string::npos, "the default is not written");
        station_parse("listen5=3\nlisten7=2\n", &s);
        expect_true(station_listen(s, 5) == kStationListenBoth, "listen5=3: FT8 and FT4 on 20 m");
        expect_true(station_listen(s, 7) == kStationListenFt4, "listen7=2: FT4 alone on 15 m");
        StationSettings u;
        station_settings_init(&u);
        station_parse(station_serialize(s), &u);
        expect_true(station_listen(u, 5) == kStationListenBoth && station_listen(u, 7) == kStationListenFt4,
                    "listen<N> round-trips");
        station_parse("listen5=0\nlisten7=9\nlisten12=2\n", &u);
        expect_true(station_listen(u, 5) == kStationListenFt8 && station_listen(u, 7) == kStationListenFt8,
                    "anything but 1, 2 or 3 reads as FT8 alone");
        expect_true(station_listen(u, 12) == kStationListenFt8 && station_listen(u, -1) == kStationListenFt8,
                    "a band out of range is FT8 alone");
        expect_true(station_key_known("listen5") && !station_key_known("listen12") && !station_key_known("rx_ft4"),
                    "listen<N> known to CONFIG for a band that exists; rx_ft4 is gone");
        expect_true(station_key_names_operating_band("listen5", 5) && !station_key_names_operating_band("listen4", 5),
                    "the operating band's channels can move its dial");
        // FT4's own dial per band (ft4band<N>): unset by default, and left unwritten then.
        StationSettings f;
        station_settings_init(&f);
        expect_true(!f.ft4_freq_set[5] && station_serialize(f).find("ft4band") == std::string::npos,
                    "no FT4 dial is set by default");
        station_parse("ft4band5=14083.5\n", &f);
        expect_true(f.ft4_freq_set[5] && f.ft4_freq[5] > 14083.4f && f.ft4_freq[5] < 14083.6f, "ft4band5 parsed");
        expect_true(!f.band_freq_set[5], "and not taken for FT8's band5");
        StationSettings g;
        station_settings_init(&g);
        station_parse(station_serialize(f), &g);
        expect_true(g.ft4_freq_set[5] && g.ft4_freq[5] > 14083.4f && g.ft4_freq[5] < 14083.6f, "ft4band<N> round-trips");
        expect_true(station_key_known("ft4band7") && !station_key_known("ft4band12"), "ft4band<N> known to CONFIG");
        expect_true(station_key_names_operating_band("ft4band5", 5) && !station_key_names_operating_band("ft4band4", 5),
                    "the operating band's FT4 dial can move its dial");
        station_parse("amp_prime_on_band_change=0\n", &s);
        expect_true(!s.amp_prime_on_band_change, "amp prime turns off");
        expect_true(station_key_known("amp_prime_on_band_change"), "amp prime key known to CONFIG");
    }

    {
        // The HL2 (row mg9c, docs/roadmap/hl2-support.md): by name, as the bench sets it, and by the number Station.txt stores.
        StationSettings s;
        station_settings_init(&s);
        station_parse("radio=hl2\n", &s);
        expect_int(s.radio, kStationRadioHl2, "radio HL2 by name");
        station_settings_init(&s);
        station_parse("radio=6\n", &s);
        expect_int(s.radio, kStationRadioHl2, "radio HL2 as stored");
        station_settings_init(&s);
        station_parse("radio=7\n", &s);
        expect_int(s.radio, kStationRadioQmx, "an unknown radio is the QMX");
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
            "cq_mod= dx\n"
            "beacon=1\n"
            "unknown_key=nope\n",
            &s);
        expect_str(s.call, "KB2SLO", "call upper");
        expect_str(s.grid, "FN30pr", "grid maidenhead");
        expect_true(s.band_freq_set[5], "band5 set");
        expect_float(s.band_freq[5], 14074.0f, "band5 freq");
        expect_int(s.radio, 2, "radio=QDX reads as the QMX: no QDX backend");
        expect_str(s.cq_mod, "DX", "cq_mod trimmed and upper");
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
        // The CQ's modifier: what a standard message can carry between CQ and
        // the call. The same vectors hold the web app's cqModOk() to this rule
        // (webapp/tests/test_cq_mod.js).
        const char* ok[] = {"", "DX", "POTA", "SOTA", "QRP", "HI", "AK", "A", "TEST", "123", "000"};
        const char* bad[] = {"POTAS", "D X", "DX1", "12", "1234", "1A", "dx", "CQ DX", "-", "US-1"};
        for (const char* m : ok) expect_true(station_cq_mod_ok(m), m);
        for (const char* m : bad) expect_true(!station_cq_mod_ok(m), m);

        StationSettings s;
        station_settings_init(&s);
        station_parse("cq_mod=pota\n", &s);
        expect_str(s.cq_mod, "POTA", "a modifier is stored");
        station_parse("cq_mod=CQ DX\n", &s);
        expect_str(s.cq_mod, "POTA", "a bad modifier keeps the old");
        station_parse("cq_mod=\n", &s);
        expect_str(s.cq_mod, "", "empty is a plain CQ");
        s.cq_mod = "DX";
        StationSettings t;
        station_settings_init(&t);
        station_parse(station_serialize(s), &t);
        expect_str(t.cq_mod, "DX", "and it round-trips");
    }

    {
        StationSettings s;
        station_settings_init(&s);
        station_parse("band5=14074\n", &s);
        expect_true(s.band_freq_set[5], "band5 parsed");
        expect_float(s.band_freq[5], 14074.0f, "band5 freq");
        const std::string text = station_serialize(s);
        expect_true(text.find("band5=14074") != std::string::npos, "serialize band5");
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
        // act_refs (architecture/webapp.md, "Activation"): round-trips,
        // upper-cases both halves of a token, and drops a malformed token
        // on its own rather than
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

    {
        // act_refs_prefer_sota (architecture/logging.md, "CQ type follows
        // the activation"): drives the empty->non-empty act_refs
        // transition's CQ-type pick in both boards' porta_host_config_set().
        // A mixed activation defaults to SOTA.
        expect_true(act_refs_prefer_sota("SOTA:W7O/CC-001"), "a pure SOTA activation prefers SOTA");
        expect_true(!act_refs_prefer_sota("POTA:US-1234"), "a pure POTA activation does not prefer SOTA");
        expect_true(act_refs_prefer_sota("POTA:US-1234,SOTA:W7O/CC-001"),
                    "a mixed activation defaults to SOTA");
        expect_true(!act_refs_prefer_sota(""), "no activation does not prefer SOTA");
    }

    {
        // The web app's settings kept on the station (docs/architecture/webapp.md, Settings that
        // live on the station): defaults, a round trip, a bad value kept out, and the mask.
        StationSettings s;
        station_settings_init(&s);
        expect_str(s.dist_unit, "km", "kilometres to start");
        expect_str(s.cq_mode, "FT8", "the CQ row calls in FT8 to start");
        expect_true(!s.show_bearing, "no bearing on decode rows to start");
        expect_true(s.queue_marks, "queue marks on to start");
        expect_str(s.qrz_key, "", "no QRZ key to start");

        station_parse("qrz_key=ABCD-EFGH-IJKL-MNOP\nqrz_book=kb2slo\nqrz_live=20261122\n"
                      "dist_unit=mi\ncq_mode=FT4\nshow_bearing=1\nqueue_marks=0\n", &s);
        StationSettings back;
        station_settings_init(&back);
        station_parse(station_serialize(s), &back);
        expect_str(back.qrz_key, "ABCD-EFGH-IJKL-MNOP", "the QRZ key round-trips whole");
        expect_str(back.qrz_book, "KB2SLO", "the logbook's callsign, upper-cased");
        expect_str(back.qrz_live, "20261122", "the day sending by itself was turned on");
        expect_str(back.dist_unit, "mi", "miles");
        expect_str(back.cq_mode, "FT4", "the CQ row calls in FT4");
        expect_true(back.show_bearing, "bearing on");
        expect_true(!back.queue_marks, "queue marks off");

        station_parse("dist_unit=furlongs\ncq_mode=FT2\nqrz_live=yesterday\nqrz_key=a key\nshow_bearing=yes\n", &s);
        expect_str(s.dist_unit, "mi", "a bad unit keeps the old");
        expect_str(s.cq_mode, "FT4", "a bad mode keeps the old");
        expect_str(s.qrz_live, "20261122", "a bad day keeps the old");
        expect_str(s.qrz_key, "ABCD-EFGH-IJKL-MNOP", "a key with a space keeps the old");
        expect_true(s.show_bearing, "a bad switch value keeps the old");
        station_parse("qrz_key=\nqrz_book=\nqrz_live=\n", &s);
        expect_str(s.qrz_key, "", "an empty key clears it");
        expect_str(s.qrz_live, "", "an empty day turns sending by itself off");

        expect_true(station_pref_refusal("dist_unit", "furlongs") != nullptr, "a bad unit is refused");
        expect_true(station_pref_refusal("cq_mode", "FT2") != nullptr, "a bad mode is refused");
        expect_true(station_pref_refusal("qrz_key", "a key") != nullptr, "a key with a space is refused");
        expect_true(station_pref_refusal("qrz_live", "2026112") != nullptr, "a short day is refused");
        expect_true(station_pref_refusal("qrz_key", "") == nullptr, "an empty key clears, not refused");
        expect_true(station_pref_refusal("queue_marks", "0") == nullptr, "a switch is 0 or 1");
        expect_true(station_pref_refusal("grid", "nonsense") == nullptr, "other keys are checked elsewhere");
        expect_true(station_key_known("qrz_key") && station_key_known("cq_mode") && station_key_known("queue_marks"),
                    "the new keys are settable");

        expect_true(station_key_secret("qrz_key"), "the QRZ key is a secret");
        expect_true(!station_key_secret("qrz_book"), "its logbook's callsign is not");
        expect_str(station_mask_secret("ABCD-EFGH-IJKL-MNOP"), "xxxxxxxxxxxxxxxMNOP", "all but the last 4 as x");
        expect_str(station_mask_secret("ABCDEFGH"), "xxxxxxxx", "8 characters or fewer: all of it");
        expect_str(station_mask_secret(""), "", "no key, nothing to mask");
        StationSettings k;
        station_settings_init(&k);
        k.qrz_key = "ABCD-EFGH-IJKL-MNOP";
        const std::string open_text = station_serialize(station_masked(k));
        expect_true(open_text.find("ABCD") == std::string::npos, "the open read never carries the key whole");
        expect_true(open_text.find("qrz_key=xxxxxxxxxxxxxxxMNOP\n") != std::string::npos, "it carries the mask");
        expect_str(k.qrz_key, "ABCD-EFGH-IJKL-MNOP", "masking leaves the station's own copy whole");
    }

    // station_key_names_operating_band (docs/architecture/webapp.md, "The
    // dock"): does this CONFIG key need the VFO already on the operating band
    // to take effect right now.
    expect_true(station_key_names_operating_band("band_sel", 5), "band_sel always names the operating band");
    expect_true(station_key_names_operating_band("band5", 5), "the operating band's frequency");
    expect_true(!station_key_names_operating_band("band3", 5), "a different band's frequency is not operating");
    expect_true(!station_key_names_operating_band("active_bands", 5), "active_bands never names the operating band");
    expect_true(!station_key_names_operating_band("call", 5), "an unrelated key is never the operating band");
    expect_true(!station_key_names_operating_band(nullptr, 5), "a null key is never the operating band");
    expect_true(!station_key_known("protocol_mode"), "protocol_mode is no longer a key");
    expect_true(!station_key_known("ft4_band5"), "nor ft4_band<N>");

    // station_vfo_refusal: a refused band change says "band", a refused
    // frequency write says "frequency", each with the reason.
    expect_str(station_vfo_refusal("band_sel", true), "band saved, VFO not moved (TX active)", "band_sel, TX active");
    expect_str(station_vfo_refusal("band_sel", false), "band saved, VFO not moved (CAT not ready)", "band_sel, CAT not ready");
    expect_str(station_vfo_refusal("band5", true), "frequency saved, VFO not moved (TX active)", "frequency, TX active");
    expect_str(station_vfo_refusal("band5", false), "frequency saved, VFO not moved (CAT not ready)", "frequency, CAT not ready");

    if (g_fails) {
        fprintf(stderr, "%d FAIL(s)\n", g_fails);
        return 1;
    }
    expect_true(station_key_known("call"), "call known");
    expect_true(station_key_known("band0"), "band0 known");
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
