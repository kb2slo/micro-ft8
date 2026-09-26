# Time: one soft clock, set by whatever knows UTC

FT8 needs UTC. Slots start on UTC boundaries (15 s for FT8, 7.5 s for FT4), and log stamps and daily log files are UTC dates. Decode tolerates only a couple of seconds of error: the candidate search looks for a signal between 1.6 s before and 3.2 s after capture starts (`time_offset` −10…20 blocks in `ft8_lib`'s `decode.c`). So a clock that is grossly wrong produces no decodes, and nothing on the air can correct it. This doc covers where the clock comes from on each main board and how it stays right. Code: `components/rtc_core/`, plus the callers named below.

## One clock

* **`rtc_now_ms()` is the clock.** It is an epoch base plus the time elapsed on `esp_timer` since that base was set. Everything that needs wall time reads it: the capture task's slot boundaries (`ft8_audio_pipeline.cpp`), the TX trigger and tone schedule, the decode slot index, the ADIF `qso_date`/`time_on` and daily file name (`adif_logger.cpp`), and Port A event stamps (`rtc_epoch_secs_or_zero()`).
* **Setting the clock is a step, not a slew.** `rtc_seed_epoch()` replaces the base and tags the source. Every full set also mirrors the time into the chip's own RTC (`rtc_sync_to_esp_rtc()`, `settimeofday`) and into a DS3231 when one is present (`rtc_write_external_from_soft()`).
* **UTC throughout.** Strings come from `gmtime_r`. `mktime` and `localtime_r` are safe only because the device TZ is never set; do not set one.
* **Unset is a real state.** Until a source sets it, `rtc_valid` is false and `rtc_now_ms()` returns uptime, so slot phase is arbitrary and decode will not work. Event stamps go out as 0, which the web app shows as `--:--:--` rather than a 1970 time. The ADV's STATUS date and time defaults exist for typing in, and are deliberately never used to seed the clock: doing so once made a fake 10:10 look valid. **Nothing refuses to transmit on an unset clock,** and a QSO logged on one is stamped with uptime as a 1970 date.

## Sources

**Last writer wins.** Sources are not ranked: any full set replaces the clock, whatever set it before. Only the boot order is fixed.

| Source | Board | Sets the clock | Precision |
|---|---|---|---|
| Browser | both | Automatically while a paired web app is open (**Browser sync** below) | milliseconds |
| DS3231 | ADV | At boot, if the chip reads valid (**DS3231** below) | whole seconds |
| ESP RTC | ADV | At boot, if there is no valid DS3231 and the chip's RTC holds a year of 2020 or later: it survives a soft reset or deep sleep, not a power loss | microseconds |
| GPS | ADV | The LoRa-1262 cap's GNSS: the first RMC with a valid fix, then again in the first 5 s of each UTC hour unless a TX or decode is running | whole seconds, stamped by a 1 Hz poll |
| Phone, BLE CTS | ADV | On demand, from the BT screen (**BLE CTS** below) | whole seconds |
| Manual | ADV | STATUS date or time edit, validated before it touches the clock (`datetime_field.cpp`) | whole seconds |
| Decode auto-sync | both | Never sets, only nudges (**Decode auto-sync** below) | ≤320 ms per slot |

**The ADV at boot:** DS3231, else the ESP RTC, else unset. **The P4 at boot is always unset.** It has no DS3231 (`external_rtc_stub.cpp`) and does not read back the ESP RTC it mirrors into, so it runs on uptime until a paired browser syncs it, which an open web app does within seconds.

GPS is the only source that also sets the radio's clock (`radio_control_set_time()`, for radios with `can_set_time`).

Whole-second sources can land up to about a second off. Decode auto-sync removes that residue once there is traffic to decode.

## The STATUS suffix (ADV)

The ADV's STATUS Time line ends with the source of the last full set (`rtc_time_source_suffix()` in `main.cpp`):

| Suffix | Means |
|---|---|
| ` R` | Read from the DS3231 at boot, or a manual entry that was written to a DS3231 that is present |
| ` G` | GPS |
| ` P` | Phone, over BLE CTS |
| ` W` | Browser: it set the clock, **or it found the clock already within 2 s**. That relabels the source as ` W` even if GPS set the time |
| none | ESP RTC at boot, or a manual entry with no DS3231 |

Auto-sync nudges do not change the suffix. The P4 tracks the source but shows it nowhere; nothing on Port A carries it.

## Browser sync over Port A

On the headless station the browser is the primary source, and has to be. GPS needs a cap and a view of the sky, a DS3231 must already have been set, manual entry needs a keyboard, and NTP on the sidekick needs internet, which is missing in exactly the field case where a cold radio will not decode until UTC is right. The phone knows the time without any of that.

1. **The web app** (`autoSyncClock()` in `webapp/app/app.html`) syncs only when paired: setting the clock is a write ([system.md](system.md), **Who may key the transmitter**). It syncs on load, on pairing, and then every 3 minutes, riding the 5 s status poll so there is no extra timer. After a failure it retries in 3 s. It sends `Date.now()` in milliseconds as the request body. There is no latency correction: the code's "round-trip halving" is computed before the request is sent, so the body is effectively the time the request started. At LAN latency that error does not matter (step 3).
2. **The sidekick** (`post_time()` in `sidekick/main/host_link.c`, `POST /api/time`) encodes `PORTA_ACT_SET_CLOCK`, with epoch seconds plus milliseconds, and returns the main board's ACK or NAK.
3. **The main board** (`porta_host_set_clock()`) refuses anything before epoch 1,700,000,000 (a plausibility floor, since this is the one path that sets the clock with no human looking). If the clock is valid and within 2 s, it changes nothing except the source label. Otherwise it seeds the clock with the milliseconds applied, mirrors it, and logs `Clock set from browser (+N s)` (in the ADV's debug log, or the P4's console).

**Why the 2 s gate.** The browser syncs on every page load and every 3 minutes. Acting every time would fill the log with entries about nothing changing and keep rewriting a DS3231 that is already right. The gate also leaves decode auto-sync's sub-second offset alone. The cost: a set has no TX guard, but it only fires when the clock is 2 s or more off, when a TX would be mistimed anyway.

`porta_host_set_clock()` exists twice, in `main/main.cpp` and `poe_p4/main/porta_host_stubs.cpp` (**not yet merged: row j4xh**).

The Operate slot bar runs on the **phone's** clock (`Date.now()` modulo the slot period), not the main board's. The two differ by auto-sync's offset and are not reconciled.

## Decode auto-sync

After each decoded slot with more than three decodes, `decode_pipeline.cpp` takes the median `time_s` (where each signal starts, relative to the start of capture). If the median is more than 0.3 s from zero, it moves the clock by that amount, capped at 320 ms per slot (`rtc_apply_decode_sync()`). Each nudge also rewrites the ESP RTC and the DS3231. It runs on both boards.

* **It fixes fine error, never gross error.** It needs decodes, and a clock off by more than a couple of seconds produces none. Gross error needs a full set. Making it visible is row I15.
* **Where it settles** (derived from the code, **unmeasured**). FT8 transmitters start 0.5 s after the UTC boundary, and capture starts at the main board's boundary. So a clock set exactly to UTC sees a median of about +0.5 s plus any audio-path latency, and auto-sync pulls it back until the median is within ±0.3 s. After a browser or GPS set, then, the main board's clock runs a few tenths of a second **behind** UTC. Consequences, all inferred:
  * Capture starts roughly when signals do, so the decode trigger at 12.64 s ([decode.md](decode.md)) cuts less of a typical signal than the "last ~0.5 s" figure there, which assumes a clock exactly on UTC.
  * TX starts on the main board's slot boundary (`tx_start` in `main.cpp`, `tx_scheduler.cpp`), so it goes out that same few tenths after UTC. That is near the convention, not early.
  * The DT the web app shows is raw `time_s`, not WSJT-X's DT with 0.5 s subtracted, yet it reads near zero for a typical station because auto-sync centres the median there.

  To measure it: look at the DT spread on Operate during a busy slot. If DTs cluster near +0.5, auto-sync is not running.

## DS3231 (ADV)

`main/time/external_rtc.cpp`. A generic DS3231 breakout at `0x68` on the ADV's shared internal I2C bus (`G8`/`G9`, which the ES8311 codec also uses), at 100 kHz. The driver is DS3231-only: M5Stack's Grove RTC Unit is a different chip (HYM8563) and is not supported.

* **Read at boot only.** A read is refused if the oscillator-stop flag is set (the chip lost power, so its time is meaningless), if it is in 12-hour mode or the century bit is set, or if the year is outside 2024–2099.
* **Written by every full set and every auto-sync nudge,** in whole seconds. The write clears the oscillator-stop flag.
* Persistence of an auto-sync correction across a power cycle has never been checked on hardware ([TEST_PLAN.md](../TEST_PLAN.md), **Unverified**).
* The `external_rtc` seam in shared code (`components/external_rtc/`, `rtc_core`'s DS3231 write, the P4's stub) outlives the ADV only until row za6n.

## BLE CTS (ADV)

`main/time/cts_ble.cpp` (NimBLE) and `cts_time.cpp` (pure parsing, host-tested in `host_mock/test_cts_time.cpp`). The keys and phone steps are in [README.md](../../README.md).

* **Flow.** The ADV advertises as `Micro-FT8-<call>`. The phone connects from nRF Connect or LightBlue, and the ADV starts pairing (Just Works, bonded, Secure Connections). After a 1 s delay it discovers the phone's Current Time Service (`0x1805`) and reads Current Time (`0x2A2B`) and Local Time Information (`0x2A0F`).
* **Current Time is local civil time on iPhones,** so UTC needs the timezone and DST from `0x2A0F`. A phone that does not expose it gets `No 2A0F`, and nothing is applied. The fraction of a second in the reply is dropped when the clock is seeded.
* **The radio has to be off USB.** NimBLE and the USB host cannot both have the DMA RAM on the ADV, which has no PSRAM ([main_board.md](main_board.md), **The RAM that matters is the largest DMA block**). Starting a sync uninstalls the USB host, and it is restored when the sync ends.
* **Once per boot.** After the read, NimBLE is torn down and its controller memory is released (`esp_bt_mem_release`), which cannot be undone without a reboot. A second attempt shows `Reboot sync`.
* Advertising times out after 90 s. A TX starting aborts the sync.

## Why not

* **NTP on the sidekick.** It needs internet, and operating must not ([system.md](system.md), constraint 2). The phone supplies the same time with no network at all.
* **BLE CTS on the headless station.** The P4 has no BLE, and the phone reaches the station through the browser, which on iOS has no Web Bluetooth ([system.md](system.md), **Why not**). Browser sync does the same job with no pairing and no USB interruption.
