# Main board

The main board decodes, transmits, runs autoseq, keeps the log, and hosts the radio over USB. The **P4** is the only one: the daily, headless station. (The Cardputer ADV was dropped 2026-10-02, row za6n; `git log --diff-filter=D -- main/main.cpp` finds its last tree.) How it fits with the sidekick and the phone: [system.md](system.md). Radio control: [radio.md](radio.md).

Both boards run the same decode, autoseq, TX and Port A code from shared components; how the source is shared between them is a rule in [STYLE.md](../STYLE.md) (**Two boards, one source**).

## P4 (M5Stack Unit PoE-P4, U213)

The daily station. Headless is the only mode: the module has no display, touch or keyboard.

* **ESP32-P4, revision v1.3.** ESP-IDF treats pre-v3 P4 silicon as a separate hardware branch and defaults to v3+, whose bootloader refuses to boot on this board; `poe_p4/sdkconfig.defaults` selects `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`. Newer P4 improvements (400 MHz, better PIE) do not apply to it.
* **16 MB flash. 32 MB PSRAM** (`CONFIG_SPIRAM`, caps-only so `malloc` stays internal). The waterfall, the decode working sets and the capture path (ingest ring, FFT buffers) live there as `EXT_RAM_BSS` ([memory.md](memory.md)); DMA buffers stay in internal RAM.
* **Internal RAM** holds the statics, the heap and some code, and the static part is budgeted at build time: [memory.md](memory.md).
* **No WiFi and no BLE.** The board pairs the P4 with an Ethernet PHY and 802.3at PoE; **neither is used, by decision** (2026-09-16). The phone reaches the station only through the sidekick ([system.md](system.md)).
* **No RTC chip, battery gauge or GPS.** `external_rtc_stub.cpp` and `board_power_stub.cpp` report them absent. The clock is unset at every boot until the web app sets it from the phone's clock over Port A ([time.md](time.md)).
* **Grove** `G53`/`G54` to the sidekick. A `porta_rx` task blocks in the UART read and decodes the byte stream there (`components/porta/porta.cpp`). Link-test bulk frames are checked in that task. Every other frame is queued for the main loop, which still owns autoseq and the radio. The loop's old 128-byte read every 10 ms was about 12.8 kB/s: clean at 115200, and CRC errors plus sequence drops at 230400 (measured on the P4, 2026-10-01). The task is not pinned.
* **Tasks.** The main loop (`app_main`, core 0, a pass every 10 ms) owns autoseq, the slot cycle and the armed TX, the radio, the decode ids a Port A reply names, the dial history, and the recently-worked and heard-grid tables. None of them is locked, so no other task may call into them.
  * **Decode** runs in `decode_audio`, pinned to core 1 (`poe_p4/main/main.cpp`). It decodes and steps the clock, then hands each slot's decodes to the main loop: a one-slot hand-off under a mutex, as Port A's frames are, which `decode_pipeline_apply()` (`components/decode_pipeline`) takes on the main loop's next pass. That call does the rest: the two tables, the sort, `autoseq_on_decodes()`, arming, the decode ids and the DECODE events. The TX trigger waits until the main loop has applied the previous slot, not just until it is decoded ([autoseq.md](autoseq.md), **The slot cycle**).
  * **Logged:** a slot applied 250 ms or more after its decode (`DECODE slot <n> applied <ms> ms late`), and one never applied because the next slot's decode came first (`DECODE slot <n> dropped, never applied`). Both mean the main loop stalled, and both go to the console and the sidekick's log.
  * **Why not a lock around autoseq.** A QSO's ADIF flash write would run under it, autoseq's callbacks call back into the board, and the armed TX would need the same lock.
  * **The station call** is the main loop's (`g_call`). The decode task tests decodes against its own copy, which the main loop sets whenever the call changes (`decode_pipeline_set_my_call()`, under a mutex), never against `g_call` itself.
  * **The soft clock** is one atomic value, safe from any task ([time.md](time.md)). The decode task steps it; the main loop sets it from the phone.
  * **Still shared, unlocked:** the `decode_tx_state` flags, the designed hand-shake between the two tasks (one writer per field, none wider than one 32-bit write); and the callsign hash table, which decode fills and TX encoding reads, kept apart only because the TX trigger waits for the decode.
* **Storage:** a LittleFS `storage` partition holds `Station.txt` and the daily `.adi` files (`poe_p4/partitions.csv`).

### Two USB-C ports

* **Host** — the QMX: CAT (USB CDC) and audio (UAC) run as two USB host clients in `poe_p4/main/qmx_transport.cpp`. The QMX takes about 11 s to enumerate after it is plugged in (a QMX-side fact, measured 2026-09-20). **A P4 reset or flash while the QMX streams can wedge the QMX's USB:** the boot logs `ENUM: Unexpected (8) device response length (expected 16)` and `CHECK_SHORT_DEV_DESC FAILED` once (the QMX acknowledges `GET_DESCRIPTOR` and sends no bytes), then CDC open fails until the QMX restarts. Three resets out of three wedged it on 2026-10-31, QMX 1_04_003; a reboot on 2026-09-28 recovered on its own. Only the QMX can clear it: switching the QMX off and on does, and on this unit so does unplugging its USB, which restarts it. The P4 cannot. The Host port's 5 V runs through an AW32901 switch whose enable is tied to ground (schematic `SCH_StickPoE_SCH_Main_V0.2`, `U8`), so no GPIO cuts it, and cycling the root port (`usb_host_lib_set_root_port_power()`) re-enumerated the QMX into the same empty descriptor (2026-10-31). qmx-panadapter reports the same wedge on QMX 1_03_002 and 1_04_004, also proof against an 8 s VBUS cut and an orderly close before the reset ([its report](https://github.com/SteffenLav/qmx-panadapter/blob/main/docs/qmx-usb-wedge-report-for-hans.md)); an orderly close would still be worth doing before any restart our firmware starts itself, of which there are none today. Do not run UAC open/start on the task that pumps USB host events: that crashed the P4 on 2026-09-18 ([README.md](../README.md), **Operating notes**).
* **PROG**, next to the RJ45 — flashing, the serial console, and 5 V power in, all at once. Confirmed in daily use (operator, 2026-09-25): the QMX streams on Host while the console runs on PROG.

### Field power

Confirmed by the operator, 2026-09-25: **one laptop power bank runs the whole station.** The QMX takes 12 V from the bank through a USB-C PD trigger cable; the P4 takes straight USB-C from the bank into PROG; the sidekick takes 5 V from the P4 over Grove. The P4 also runs from an iPhone 17 Pro's USB-C port. The sidekick is powered over Grove in every setup.

### On the bench

What a session with a P4 and a QMX runs into, observed 2026-10-24 with QMX firmware 1_04_003. These are facts about the hardware, so nothing here is a procedure to follow.

* **The QMX has to be power-cycled after the P4 resets or flashes.** The USB host reads a short device descriptor (`ENUM: CHECK_SHORT_DEV_DESC FAILED`) and never retries, so `QMX CDC open attempt N failed` repeats until the radio is powered off and on (row `qdcm`). A cable replug while the QMX stays powered fails the same way; a QMX power cycle enumerates in about 10 to 15 s. Plan for one power cycle after every flash.
* **The PROG console link can drop when the QMX keys.** Once, on Tune, the serial console went silent and the P4 stopped answering the flasher (`Failed to connect to ESP32-P4: No serial data received`) while the station kept working, until the PROG cable was unplugged and replugged. Cause unconfirmed; RF on the cable is the suspect. A log capture that stops mid-transmission is this, not a P4 hang.
* **When the console is silent, the sidekick still has the P4's `debug_log_line` lines**, in `/api/events` as `d:0` rows (for example `DECODE pipeline could not start`). Only one program can hold the serial port at a time, so a monitor has to be stopped before flashing.
* **`VN;` answers with the release and the model run together** (`VN1_04_003QMX;`), and `SR;` needs 1_04_004 or later, so the SWR-protection recovery does nothing on 1_04_003 ([radio.md](radio.md)).
