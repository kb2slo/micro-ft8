// The TX watchdog (components/tx_watchdog/include/tx_watchdog.h; docs/architecture/radio.md, The TX watchdog):
// a key held past its cap, and a QSO that keeps transmitting across its steps and reactivations.
//
// Pins: a message key is stopped past an FT8 slot and Tune past 30 s, not before; a QSO whose station keeps
// missing our RR73 gets one per resend until its 40th transmission, then nothing, and its next resend revives
// nothing; a CQ is bounded by its own tries, not by the QSO cap.

#include <cstdio>
#include <string>

#include "autoseq.h"
#include "host_mocks.h"
#include "tx_watchdog.h"

static int g_fails = 0;

static void expect(bool ok, const char* msg)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    }
}

static void test_key_caps()
{
    // A whole FT8 transmission, lead and tail included, is inside the cap; a message keyed for longer is not.
    expect(!tx_watchdog_key_over(TxWatchdogKey::Message, 1000, 1000 + 13500), "a whole FT8 message is not stopped");
    expect(!tx_watchdog_key_over(TxWatchdogKey::Message, 1000, 1000 + kTxWatchdogMessageMs),
           "a message at its cap is not yet stopped");
    expect(tx_watchdog_key_over(TxWatchdogKey::Message, 1000, 1000 + kTxWatchdogMessageMs + 1),
           "a message keyed past an FT8 slot is stopped");
    expect(kTxWatchdogMessageMs == 15000, "the message cap is an FT8 slot");

    expect(!tx_watchdog_key_over(TxWatchdogKey::Tune, 0, 20000), "Tune runs past a message's cap");
    expect(!tx_watchdog_key_over(TxWatchdogKey::Tune, 0, kTxWatchdogTuneMs), "Tune at its cap is not yet stopped");
    expect(tx_watchdog_key_over(TxWatchdogKey::Tune, 0, kTxWatchdogTuneMs + 1), "Tune past 30 s is stopped");
}

static bool adif_cb(const std::string&, const std::string&, int, int, RxChannel) { return true; }

static UiRxLine to_me(const char* dx, const char* f3)
{
    UiRxLine m;
    m.field1 = "KB2SLO";
    m.field2 = dx;
    m.field3 = f3;
    m.text = std::string("KB2SLO ") + dx + " " + f3;
    m.snr = -8;
    m.offset_hz = 1200;
    m.slot_id = 1;
    m.is_to_me = true;
    return m;
}

static void decode(const UiRxLine& m) { autoseq_on_decodes(std::vector<UiRxLine>{m}); }

static std::string transmit()
{
    AutoseqTxEntry pending;
    if (!autoseq_fetch_pending_tx(pending)) return "";
    autoseq_on_tx_starting();
    autoseq_mark_sent(0);
    autoseq_tick(0, 0, 0);
    return pending.text;
}

static void reset()
{
    mock_clock_set(1000);  // parked entries stamp this; 0 reads as "not parked"
    autoseq_init();
    autoseq_set_station("KB2SLO", "FN30");
    autoseq_set_tries(AUTOSEQ_TRIES);
    autoseq_set_adif_callback(adif_cb);
    autoseq_set_recent_qso_callback(nullptr);
}

// The QSO that keeps changing step: K1ABC never hears our RR73 and resends R+report each time. Each resend
// reactivates the parked entry with fresh tries (autoseq.md, Inactive zone), so tries alone never end it.
static void test_qso_that_keeps_reviving_ends_at_the_cap()
{
    reset();
    autoseq_call_cq(RxChannel{}, 0);
    autoseq_set_entry_tries(autoseq_cq_entry_id(), 1);
    expect(transmit() == "CQ KB2SLO FN30", "the CQ");
    decode(to_me("K1ABC", "FN42"));
    expect(transmit() == "K1ABC KB2SLO -8", "the report: the QSO's first transmission");
    int sent = 1;
    for (int i = 0; i < 100; ++i) {
        decode(to_me("K1ABC", "R-10"));
        const std::string tx = transmit();
        if (tx.empty()) break;
        expect(tx == "K1ABC KB2SLO RR73", "each resend gets RR73");
        ++sent;
    }
    expect(sent == kTxWatchdogQsoTransmissions, "the QSO makes the cap's transmissions and no more");
    expect(autoseq_active_count() == 0, "and is not in the queue");
    decode(to_me("K1ABC", "R-10"));
    expect(transmit().empty(), "its next resend revives nothing: the entry ended, not parked");
}

// The cap is a QSO's: a CQ's own tries bound it, past the QSO cap.
static void test_cq_is_bounded_by_its_tries_not_the_cap()
{
    reset();
    autoseq_call_cq(RxChannel{}, 0);
    autoseq_set_entry_tries(autoseq_cq_entry_id(), 60);
    int sent = 0;
    while (!transmit().empty() && sent < 100) ++sent;
    expect(sent == 60, "a CQ of 60 tries sends 60");
}

int main()
{
    test_key_caps();
    test_qso_that_keeps_reviving_ends_at_the_cap();
    test_cq_is_bounded_by_its_tries_not_the_cap();
    if (g_fails) {
        fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    printf("tx_watchdog: ok\n");
    return 0;
}
