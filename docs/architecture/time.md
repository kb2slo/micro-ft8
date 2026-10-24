# Time: one soft clock, set by whatever knows UTC

FT8 needs UTC. Slots start on UTC boundaries (15 s for FT8, 7.5 s for FT4), and log stamps and daily log files are UTC dates. Decode tolerates only a couple of seconds of error: the candidate search looks for a signal between 1.6 s before and 3.2 s after capture starts (`time_offset` −10…20 blocks in `ft8_lib`'s `decode.c`). So a clock that is grossly wrong produces no decodes, and nothing on the air can correct it. This doc covers where the clock comes from and how it stays right. Code: `components/rtc_core/`, plus the callers named below.

## One clock

* **`rtc_now_ms()` is the clock.** It is an epoch base plus the time elapsed on `esp_timer` since that base was set, held as one atomic value (UTC minus uptime, `rtc_core.cpp`), so every task reads it with no lock: a read never sees half a set, and a decode step never writes over a set (`host_mock/test_rtc_core.cpp`). Everything that needs wall time reads it: the capture task's slot boundaries (`ft8_audio_pipeline.cpp`), the TX trigger and tone schedule, the decode slot index, the ADIF `qso_date`/`time_on` and daily file name (`adif_logger.cpp`), and Port A event stamps (`rtc_epoch_secs_or_zero()`).
* **Setting the clock is a step, not a slew.** `rtc_seed_epoch()` replaces the base. Every full set also mirrors the time into the chip's own RTC (`rtc_sync_to_esp_rtc()`, `settimeofday`).
* **UTC throughout.** Strings come from `gmtime_r`. `mktime` and `localtime_r` are safe only because the device TZ is never set; do not set one.
* **Unset is a real state.** Until a source sets it, `rtc_is_set()` is false and `rtc_now_ms()` returns uptime, so slot phase is arbitrary and decode will not work. Event stamps go out as 0, which the web app shows as `--:--:--` rather than a 1970 time. **Nothing refuses to transmit on an unset clock,** and a QSO logged on one is stamped with uptime as a 1970 date.

## Sources

**The P4 at boot is always unset.** It has no RTC chip and does not read back the ESP RTC it mirrors into, so it runs on uptime until a paired browser syncs it, which an open web app does within seconds.

| Source | Sets the clock | Precision |
|---|---|---|
| Browser | Automatically while a paired web app is open (**Browser sync** below) | milliseconds |
| Decode auto-sync | Never sets, only nudges (**Decode auto-sync** below) | ≤320 ms per slot |

The DS3231, GPS, BLE CTS and manual-entry sources went with the ADV (row za6n); `git log --diff-filter=D -- main/time` finds them.

Decode auto-sync removes the residue a whole-second set leaves, once there is traffic to decode.

## Browser sync over Port A

On the headless station the browser is the primary source, and has to be. GPS needs a cap and a view of the sky, a DS3231 must already have been set, manual entry needs a keyboard, and NTP on the sidekick needs internet, which is missing in exactly the field case where a cold radio will not decode until UTC is right. The phone knows the time without any of that.

1. **The web app** (`autoSyncClock()` in `webapp/app/app.html`) syncs only when paired: setting the clock is a write ([system.md](system.md), **Who may key the transmitter**). It syncs on load, on pairing, and then every 3 minutes, riding the 5 s status poll so there is no extra timer. After a failure it retries in 3 s. It sends `Date.now()` in milliseconds as the request body. There is no latency correction: the code's "round-trip halving" is computed before the request is sent, so the body is effectively the time the request started. At LAN latency that error does not matter (step 3).
2. **The sidekick** (`post_time()` in `sidekick/main/host_link.c`, `POST /api/time`) encodes `PORTA_ACT_SET_CLOCK`, with epoch seconds plus milliseconds, and returns the main board's ACK or NAK.
3. **The main board** (`porta_host_set_clock()`) refuses anything before epoch 1,700,000,000 (a plausibility floor, since this is the one path that sets the clock with no human looking). If the clock is valid and within 2 s, it changes nothing. Otherwise it seeds the clock with the milliseconds applied, mirrors it, and logs `Clock set from browser (+N s)` (on the P4's console).

**Why the 2 s gate.** The browser syncs on every page load and every 3 minutes. Acting every time would fill the log with entries about nothing changing. The gate also leaves decode auto-sync's sub-second offset alone. The cost: a set has no TX guard, but it only fires when the clock is 2 s or more off, when a TX would be mistimed anyway.

The Operate slot bar runs on the **phone's** clock (`Date.now()` modulo the slot period), not the main board's. The two differ by auto-sync's offset and are not reconciled.

## Decode auto-sync

After each decoded slot with more than three decodes, `decode_pipeline.cpp` takes the median `time_s` (where each signal starts, relative to the start of capture). If the median is more than 0.3 s from zero, it moves the clock by that amount, capped at 320 ms per slot (`rtc_apply_decode_sync()`, on the decode task). An unset clock is not moved. Each nudge also rewrites the ESP RTC, as a set on the main loop does. The two can cross and leave the ESP RTC holding the time from before a set until the next nudge or set rewrites it; nothing reads it back (**Sources**).

* **It fixes fine error, never gross error.** It needs decodes, and a clock off by more than a couple of seconds produces none. Gross error needs a full set. Making it visible is row I15.
* **Where it settles** (derived from the code, **unmeasured**). FT8 transmitters start 0.5 s after the UTC boundary, and capture starts at the main board's boundary. So a clock set exactly to UTC sees a median of about +0.5 s plus any audio-path latency, and auto-sync pulls it back until the median is within ±0.3 s. After a browser set, then, the main board's clock runs a few tenths of a second **behind** UTC. Consequences, all inferred:
  * Capture starts roughly when signals do, so the decode trigger at 12.64 s ([decode.md](decode.md)) cuts less of a typical signal than the "last ~0.5 s" figure there, which assumes a clock exactly on UTC.
  * `TX;` goes out on the main board's slot boundary (`tx_scheduler.cpp`) and the first tone 0.5 s later, WSJT-X's own start, so a QMX transmission starts where the convention puts it, a few tenths after UTC on a board that is that late.
  * The DT the web app shows is raw `time_s`, not WSJT-X's DT with 0.5 s subtracted, yet it reads near zero for a typical station because auto-sync centres the median there.

  To measure it: look at the DT spread on Operate during a busy slot. If DTs cluster near +0.5, auto-sync is not running.

## Why not

* **NTP on the sidekick.** It needs internet, and operating must not ([system.md](system.md), constraint 2). The phone supplies the same time with no network at all.
* **BLE CTS on the headless station.** The P4 has no BLE, and the phone reaches the station through the browser, which on iOS has no Web Bluetooth ([system.md](system.md), **Why not**). Browser sync does the same job with no pairing and no USB interruption.
