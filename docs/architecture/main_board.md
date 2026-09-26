# Main boards

The main board decodes, transmits, runs autoseq, keeps the log, and hosts the radio over USB. Two exist: the **P4** (the daily, headless station) and the **ADV** (Cardputer, with its own screen and keyboard). How they fit with the sidekick and the phone: [system.md](system.md). Radio control: [radio.md](radio.md).

Both boards run the same decode, autoseq, TX and Port A code from shared components; how the source is shared between them is a rule in [STYLE.md](../STYLE.md) (**Two boards, one source**).

## P4 (M5Stack Unit PoE-P4, U213)

The daily station. Headless is the only mode: the module has no display, touch or keyboard.

* **ESP32-P4, revision v1.3.** ESP-IDF treats pre-v3 P4 silicon as a separate hardware branch and defaults to v3+, whose bootloader refuses to boot on this board; `poe_p4/sdkconfig.defaults` selects `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`. Newer P4 improvements (400 MHz, better PIE) do not apply to it.
* **16 MB flash. 32 MB PSRAM per the datasheet, not enabled in our build** (`CONFIG_SPIRAM` unset). Unlike on the ADV, plain CPU data (the waterfall buffer, decode working sets) could move there; DMA buffers still cannot.
* **No WiFi and no BLE.** The board pairs the P4 with an Ethernet PHY and 802.3at PoE; **neither is used, by decision** (2026-09-16). The phone reaches the station only through the sidekick ([system.md](system.md)).
* **No RTC chip, battery gauge or GPS.** `external_rtc_stub.cpp` and `board_power_stub.cpp` report them absent. The clock is unset at every boot until the web app sets it from the phone's clock over Port A ([time.md](time.md)).
* **Grove** `G53`/`G54` to the sidekick: same cable and pinout as the ADV's Port A.
* **Storage:** a LittleFS `storage` partition holds `Station.txt` and the daily `.adi` files, as on the ADV (`poe_p4/partitions.csv`).

### Two USB-C ports

* **Host** — the QMX: CAT (USB CDC) and audio (UAC) run as two USB host clients in `poe_p4/main/qmx_transport.cpp`. The QMX takes about 11 s to enumerate after it is plugged in (a QMX-side fact, measured 2026-09-20). Do not run UAC open/start on the task that pumps USB host events: that crashed the P4 on 2026-09-18 ([README.md](../README.md), **Operating notes**).
* **PROG**, next to the RJ45 — flashing, the serial console, and 5 V power in, all at once. Confirmed in daily use (operator, 2026-09-25): the QMX streams on Host while the console runs on PROG.

### Field power

Confirmed by the operator, 2026-09-25: **one laptop power bank runs the whole station.** The QMX takes 12 V from the bank through a USB-C PD trigger cable; the P4 takes straight USB-C from the bank into PROG; the sidekick takes 5 V from the P4 over Grove. The P4 also runs from an iPhone 17 Pro's USB-C port. The sidekick is powered over Grove in every setup.

## ADV (Cardputer, Stamp-S3A)

Everything in this section is `ADV-only` and goes with the ADV if row za6n retires it.

### The RAM that matters is the largest DMA block

The ADV has **no PSRAM** (ESP32-S3FN8, confirmed with `esptool.py flash_id`, 2026-09-04), and the radio's USB CDC (CAT) and UAC (audio) need **internal, DMA-capable RAM in contiguous blocks**. So the figure that decides whether something fits is the **largest free DMA block**, not free heap and not DIRAM left at link time. A session can show tens of KB free and still have no block big enough for CDC; that is exactly how QMX transmit died.

* **Measure** with the PERF screen's **DM** line (**L** = largest block, KiB), or `heap_caps_get_largest_free_block(MALLOC_CAP_DMA)` (`main/main.cpp`). Link-time `idf_size.py` numbers are a static ceiling, not runtime headroom. Only a field session with the QMX streaming gives the real figure.
* **Each large stack wants a ~40 KB contiguous block to start.** Field-measured 2026-08-31: NimBLE (BLE) and the USB host could not both start, in either order. BLE first left CAT dead; USB first made NimBLE fail to initialize.
* **The USB host is held from boot and costs ~6 KB** of the largest block (measured 2026-09-04: 38 KB at boot, against 44 KB before it was held from boot).
* **PSRAM would not help the radio anyway**: it cannot back DMA buffers. Plain CPU data could live there on a board that has it — the `ft8_lib` waterfall buffer (~80 KB, the largest object in the build) is the obvious candidate. That matters for the P4, not the ADV.

### No BLE and no WiFi on the ADV

Neither runs on the ADV while the radio is connected, for the reason above; WiFi and lwIP cost more internal RAM than BLE, not less. The phone link lives on the sidekick instead. **An upstream difference:** Wei's Micro-FT8 V2.0.4 removed BLE because initializing NimBLE before USB CDC silently failed the CDC allocation and killed QMX transmit. Do not re-add either to the ADV to "make room" for a phone feature. The one exception is BLE time sync, which parks the USB host first and cannot run twice in a boot ([time.md](time.md), **BLE CTS**).

### USB-C is one port, held in host mode

The ADV's USB-C is a single PHY with no hub, and the USB host is installed at boot so the radio can stream when connected. Plug and unplug detection (the toasts) is a USB Host Library client (`usb_c_presence`): there is no VBUS or D+ sense line, so detection needs the host stack running. **Detection and an on-demand host are mutually exclusive on this hardware.** It is also why the removed USB Drive mode could never work: the port was in host mode from power-on, so the computer never saw a device.
