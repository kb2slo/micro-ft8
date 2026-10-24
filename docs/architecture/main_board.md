# Main board

The main board decodes, transmits, runs autoseq, keeps the log, and hosts the radio over USB. The **P4** is the only one: the daily, headless station. (The Cardputer ADV was dropped 2026-10-02, row za6n; `git log --diff-filter=D -- main/main.cpp` finds its last tree.) How it fits with the sidekick and the phone: [system.md](system.md). Radio control: [radio.md](radio.md).

Both boards run the same decode, autoseq, TX and Port A code from shared components; how the source is shared between them is a rule in [STYLE.md](../STYLE.md) (**Two boards, one source**).

## P4 (M5Stack Unit PoE-P4, U213)

The daily station. Headless is the only mode: the module has no display, touch or keyboard.

* **ESP32-P4, revision v1.3.** ESP-IDF treats pre-v3 P4 silicon as a separate hardware branch and defaults to v3+, whose bootloader refuses to boot on this board; `poe_p4/sdkconfig.defaults` selects `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`. Newer P4 improvements (400 MHz, better PIE) do not apply to it.
* **16 MB flash. 32 MB PSRAM per the datasheet, not enabled in our build** (`CONFIG_SPIRAM` unset). Plain CPU data (the waterfall buffer, decode working sets) could move there; DMA buffers still cannot.
* **No WiFi and no BLE.** The board pairs the P4 with an Ethernet PHY and 802.3at PoE; **neither is used, by decision** (2026-09-16). The phone reaches the station only through the sidekick ([system.md](system.md)).
* **No RTC chip, battery gauge or GPS.** `external_rtc_stub.cpp` and `board_power_stub.cpp` report them absent. The clock is unset at every boot until the web app sets it from the phone's clock over Port A ([time.md](time.md)).
* **Grove** `G53`/`G54` to the sidekick. A `porta_rx` task blocks in the UART read and decodes the byte stream there (`components/porta/porta.cpp`). Link-test bulk frames are checked in that task. Every other frame is queued for the main loop, which still owns autoseq and the radio. The loop's old 128-byte read every 10 ms was about 12.8 kB/s: clean at 115200, and CRC errors plus sequence drops at 230400 (measured on the P4, 2026-10-01). The task is not pinned.
* **Tasks.** The main loop (`app_main`, core 0, a pass every 10 ms) owns autoseq, the slot cycle and the armed TX, the radio, the decode ids a Port A reply names, the dial history, and the recently-worked and heard-grid tables. None of them is locked, so no other task may call into them.
  * **Decode** runs in `decode_audio`, pinned to core 1 (`poe_p4/main/main.cpp`). It decodes and steps the clock, then hands each slot's decodes to the main loop: a one-slot hand-off under a mutex, as Port A's frames are, which `decode_pipeline_apply()` (`components/decode_pipeline`) takes on the main loop's next pass. That call does the rest: the two tables, the sort, `autoseq_on_decodes()`, arming, the decode ids and the DECODE events. The TX trigger waits until the main loop has applied the previous slot, not just until it is decoded ([autoseq.md](autoseq.md), **The slot cycle**).
  * **Logged:** a slot applied 250 ms or more after its decode (`DECODE slot <n> applied <ms> ms late`), and one never applied because the next slot's decode came first (`DECODE slot <n> dropped, never applied`). Both mean the main loop stalled, and both go to the console and the sidekick's log.
  * **Why not a lock around autoseq.** A QSO's ADIF flash write would run under it, autoseq's callbacks call back into the board, and the armed TX would need the same lock.
  * **Still shared across the two tasks, unlocked:**
    * The station call (`g_call`). Any Station change rewrites it on the main loop, at any moment, while the decode task reads it to tell which decodes are addressed to us. A callsign fits the string's inline buffer, so the worst case is one slot tested against a half-written call, not a freed buffer.
    * The soft clock (`rtc_ms_start`, [time.md](time.md)). The decode task steps it from decode timing; the main loop reads it on every pass and sets it from the phone.
    * The callsign hash table. Decode fills it and TX encoding reads it, kept apart only because the TX trigger waits for the decode.
* **Storage:** a LittleFS `storage` partition holds `Station.txt` and the daily `.adi` files (`poe_p4/partitions.csv`).

### Two USB-C ports

* **Host** — the QMX: CAT (USB CDC) and audio (UAC) run as two USB host clients in `poe_p4/main/qmx_transport.cpp`. The QMX takes about 11 s to enumerate after it is plugged in (a QMX-side fact, measured 2026-09-20). A P4 reboot with the QMX already attached can log `ENUM: CHECK_SHORT_DEV_DESC FAILED` (and `QMX CDC open attempt 1 failed`) in the first two seconds, from ESP-IDF's USB host catching the QMX before it is ready; it recovers on its own, with `QMX CDC-ACM opened` and `VFO sync ok` a few seconds later (seen 2026-09-28). Do not run UAC open/start on the task that pumps USB host events: that crashed the P4 on 2026-09-18 ([README.md](../README.md), **Operating notes**).
* **PROG**, next to the RJ45 — flashing, the serial console, and 5 V power in, all at once. Confirmed in daily use (operator, 2026-09-25): the QMX streams on Host while the console runs on PROG.

### Field power

Confirmed by the operator, 2026-09-25: **one laptop power bank runs the whole station.** The QMX takes 12 V from the bank through a USB-C PD trigger cable; the P4 takes straight USB-C from the bank into PROG; the sidekick takes 5 V from the P4 over Grove. The P4 also runs from an iPhone 17 Pro's USB-C port. The sidekick is powered over Grove in every setup.
