# Main boards

The main board decodes, transmits, runs autoseq, keeps the log, and hosts the radio over USB. Two exist: the **P4** (the daily, headless station) and the **ADV** (Cardputer, with its own screen and keyboard). How they fit with the sidekick and the phone: [system.md](system.md). Radio control: [radio.md](radio.md).

The P4 section comes from [RFC 0005](../rfcs/0005-p4-poe-headless-port.md) as it graduates (row k7gc).

## ADV (Cardputer, Stamp-S3A)

Everything in this section is `ADV-only` and goes with the ADV if row za6n retires it.

### The RAM that matters is the largest DMA block

The ADV has **no PSRAM** (ESP32-S3FN8, confirmed with `esptool.py flash_id`, 2026-09-04), and the radio's USB CDC (CAT) and UAC (audio) need **internal, DMA-capable RAM in contiguous blocks**. So the figure that decides whether something fits is the **largest free DMA block**, not free heap and not DIRAM left at link time. A session can show tens of KB free and still have no block big enough for CDC; that is exactly how QMX transmit died.

* **Measure** with the PERF screen's **DM** line (**L** = largest block, KiB), or `heap_caps_get_largest_free_block(MALLOC_CAP_DMA)` (`main/main.cpp`). Link-time `idf_size.py` numbers are a static ceiling, not runtime headroom. Only a field session with the QMX streaming gives the real figure.
* **Each large stack wants a ~40 KB contiguous block to start.** Field-measured 2026-08-31: NimBLE (BLE) and the USB host could not both start, in either order. BLE first left CAT dead; USB first made NimBLE fail to initialize.
* **The USB host is held from boot and costs ~6 KB** of the largest block (measured 2026-09-04: 38 KB at boot, against 44 KB before it was held from boot).
* **PSRAM would not help the radio anyway**: it cannot back DMA buffers. Plain CPU data could live there on a board that has it — the `ft8_lib` waterfall buffer (~80 KB, the largest object in the build) is the obvious candidate. That matters for the P4, not the ADV.

### No BLE and no WiFi on the ADV

Neither runs on the ADV while the radio is connected, for the reason above; WiFi and lwIP cost more internal RAM than BLE, not less. The phone link lives on the sidekick instead. **An upstream difference:** Wei's Micro-FT8 V2.0.4 removed BLE because initializing NimBLE before USB CDC silently failed the CDC allocation and killed QMX transmit. Do not re-add either to the ADV to "make room" for a phone feature.

### USB-C is one port, held in host mode

The ADV's USB-C is a single PHY with no hub, and the USB host is installed at boot so the radio can stream when connected. Plug and unplug detection (the toasts) is a USB Host Library client (`usb_c_presence`): there is no VBUS or D+ sense line, so detection needs the host stack running. **Detection and an on-demand host are mutually exclusive on this hardware.** It is also why the removed USB Drive mode could never work: the port was in host mode from power-on, so the computer never saw a device.
