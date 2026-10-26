# Micro-FT8 (kb2slo fork)

Jeff KB2SLO’s continuation of Wei AG6AQ’s Micro-FT8, which was written for the Cardputer ADV. This fork runs headless: an M5Stack Unit PoE-P4 decodes, transmits, works the QSOs and keeps the log, with a QMX on its USB host port, and an AtomS3 Lite sidekick on Grove serves the browser app you operate it from. **The Cardputer ADV firmware was dropped on 2026-10-02** because its author no longer uses it. Wei’s original, below the line, describes that firmware and does not apply to this build. How the pieces fit: [docs/architecture/system.md](docs/architecture/system.md).

- **Cancelling the QSO on the air stops it.** In the web app's queue, × on the row that is transmitting ends the transmission at once instead of after it.
- **A slot the radio cannot be keyed for is skipped, not counted.** If the QMX's CAT is down when our slot starts, nothing is sent, no try is used and no QSO is logged for an RR73 that never went out; the message goes out in its next slot once CAT is back. The console and the sidekick's log say `TX slot … skipped, not counted`; the app does not show it yet.
- **Signal reports are on WSJT-X's scale.** The dB on each decode, the report sent in a QSO and the logged RST are signal against noise in 2500 Hz, from −24 to +49, as WSJT-X computes them; they used to read about 16 dB high.
- **A QSO logs the station's grid even when the QSO never carried one.** Answering a station's RR73 or report to someone else, or a QSO that restarted after our replies to its CQ ran out of tries, logs the grid that station last sent in anything heard in the last 4 hours. The main board remembers 128 stations, and a reboot forgets them. A grid sent in the QSO, or in the CQ you tapped, still wins. WSJT-X fills these only from its `CALL3.TXT` file.
- **Logs survive the workflow.** Internal LittleFS append no longer leaves empty QSO files. `Station.txt` and the daily `.adi` logs live on a `storage` LittleFS partition; the phone's Log tab can list them, Download ADI, and **Clear all ADI files** (`DELETE /api/log/files`, pairing required — refuses today's file while TX, decode or audio streaming is active).
- **Port A runs at 2.5 Mbit/s.** The P4 held a 14 MB transfer there. The stress-test controls are still in the Settings page, hidden; [sidekick.md](docs/architecture/sidekick.md) (**Port A**) says how to show them.
- **A CQ with a try count, and tries on every queue row.** **Call CQ**, on the queue's last row, queues one CQ with the Station sheet's **Tries** (6 to start): each CQ sent uses one, a station answering gives none back, and when they are used the CQ ends, so the station never calls on and on by itself. Stations that answer are worked first, and the CQ then carries on with what it had left. The row shows the CQ's slot, **Even ⇄** or **Odd ⇄**, which a tap moves to the other one, and its count, which a tap changes (up to 99); × ends it, stopping one on the air in the same press. Every QSO row's count changes the same way (1–20, at each step), and an entry given its own count keeps it when **Tries** changes. Upstream's beacon (STATUS key 1, Off/Even/Odd) called without limit; there is no beacon here.
- **KH1 support dropped, for now.** QMX and QDX only. I don't own a KH1, so that path was never field-verified. Open to bringing it back if someone with a KH1 wants to pick it up and verify against real hardware.
- **App navigation and network settings (`http://micro-ft8.local/app.html`).** The bottom tab bar is Operate, QSOs, Tools and Settings, with no title row above Operate; a phone that is not paired says so on Operate's radio bar and with a dot on the Settings tab. Settings gained a **Network** section (mode, network name, address, firmware, uptime, **Forget WiFi**, **Switch to AP mode**, **Restore seed pages**) and sets the sidekick's own network: a name and an optional WPA2 password (empty stays open, as before), reset by holding the sidekick's button for about ten seconds — the standalone `/log` raw log/decode viewer and the `/` station-status page are both retired, since Operate already shows the same live decode stream `/log` did and Settings > Network now covers everything `/` used to. Visiting `/` in station mode redirects into the app itself (Operate, the default view). A manual TX abort (not just the automatic cancel a band change already triggers) sits on Operate's radio bar while we transmit, with the power and SWR: **Stop CQ** during our own CQ, which also ends the CQ so the next one doesn't just go out a cycle later; **Stop TX** during a QSO message, which only stops that transmission. Settings › **Amplifier** groups the minimum TX delay with an option, off by default, to recover from the QMX's SWR protection trips: a trip is cleared and the message is sent again in its next slot (the radio bar reads SWR protection tripped), and a second trip in a row ends the CQ and cancels the QSOs. **Call CQ** is on the queue's own **CQ row**, its last entry, showing the exact text the CQ sends. A **Station sheet** opens from the CQ row's gear or the radio drawer's **Station ›** (below): CQ type, one free-text field shared between Free text (up to 13 characters) and a Field Day exchange, Grid (with **Set from GPS →** and a format check, refused in a dialog if it isn't 4/6/8 valid Maidenhead characters), tries per message and the TX offset, each saved the moment it changes rather than through a Save button; the call shows there read-only, changed in Settings instead. **Operate keeps its status on top and its controls at the bottom**, and only the decodes scroll: the slot bar (with the slot's parity, **E** or **O**, red while we transmit, and at an activation each reference against its target) stays at the top, and the queue, the CQ row and a **radio bar** stay at the bottom. The radio bar shows the band and frequency and one state at a time: transmitting (power, SWR and **Stop**), an SWR trip, a radio problem (found, moved, CAT lost, no radio) or ready. Tapping it opens a drawer: band buttons (a second row past seven active bands; a band is two taps, and the drawer closes), a frequency row — MHz or kHz to 0.1 kHz, **−** and **+** by 1 kHz, **Set**; a typed value (e.g. a SOTA calling frequency) holds until the next band tap, since tapping a band always returns to its standard FT8 frequency, and typing a frequency in another active band asks, then switches to it — the CAT, Audio and Link checks, **Station ›**, what the radio reports, **Retune VFO** and **Tune**. **Tap a decode to reply**: a row you can answer, from a slot that ended in the last 30 seconds, ends in a blue ›; its callsign and grid keep their own taps; it shows **Queued**, and **Undo** takes it back for a few seconds. A station heard on a different frequency from the one the radio is on now is refused with a dialog. Decode rows are two lines: the message, then DT and dB, grid, place and distance; a decode's audio offset is on the station's card. Slot headers show each slot's span (e.g. `19:03:30–45z`); a refused value is explained in a dialog. Changing band or frequency mid-QSO asks first, then ends the queued QSOs and stops a transmission on the air, so the radio is never left on a different frequency from the one shown; a frequency the radio can't reach (CAT down) is refused rather than silently accepted. Settings' Station section now only holds Call, Comment and Ignore prefixes (CQ type, the free-text field, Grid, tries and TX offset moved to the Station sheet, and the old "CQ / exchange text" field is gone outright — it never reached the air); its Band section now only picks which bands appear. Settings has no Save station button now: each of its fields saves the moment it changes, and none of them touches those. One-shot free-text TX is removed (I33). **Tools** (third bottom-bar tab) is what the operator consults *instead of* the radio: NOAA space weather (10.7cm flux, A-index, K-index, X-ray flux class, solar wind speed/Bz, the official R/S/G scale) and POTA/SOTA activator spot chasing, filtered to the station's active bands and to FT8, with distance/bearing and a worked/new flag against the on-device log. Every card says whether it's live, degraded, or unreachable rather than showing an old number as current. **Who hears me** (Tools): the stations PSKReporter says heard your call, live from its feed while you transmit and for 15 minutes after: a layer on the map with paths from your grid, and a list you can filter by band and sort by distance, dB or time. The summary names the bands and the farthest station, and any station's card says when it reported hearing you. A stretch when the phone was locked or offline shows as **Not listening** instead of looking like silence. **SWR sweep** (a page linked from Tools; P4 with a QMX, [docs/architecture/radio.md](docs/architecture/radio.md)): tick the bands (the ones enabled in Settings are ticked), see the time it will take, and run it. The main board transmits a short carrier at each point at the QMX's own Tune power and the page plots SWR from 1 to 6 with the band boundaries, so you can tune an antenna in the field and run it again; set the QMX's Tune to 25% or more, since below that the reading is biased high. Starting it asks first if it would end a QSO, a CQ or a transmission, and Stop ends it at once. **The map** (Tools › **Open map**) plots the operator's own grid and every logged QSO's grid on a bundled offline world outline (no tiles, works with no internet at all), with great-circle paths between them. Decodes, POTA and SOTA spots, places, grid squares and PSKReporter are layers you tap on; only QSOs is on when it opens.
- **Install / update the browser app (`http://micro-ft8.local/update`).** After pairing, the page fetches the bundle from Cloudflare (`https://micro-ft8.kalikstein.com/app`) over HTTPS and writes it onto the sidekick; install opens **`/app.html`** (station setup, then Operate). Human landing for the same site: `https://micro-ft8.kalikstein.com/`. **A newly flashed sidekick has no app yet** and needs your home WiFi for this one step; the steps are in [`sidekick/README.md`](sidekick/README.md) (**First-time setup**), and the setup page and a bare `/app.html` now say so. **Settings > Network > Factory reset** (two taps) returns a sidekick to that state: it erases the saved WiFi, the installed app and the pairing, and leaves the main board's settings alone. **Restore seed pages** (same page, or `app.html`'s Settings > Network) rewrites provision/update from firmware without wiping an installed app.

## Clone and CI firmware

To build from source, clone with the `ft8_lib` submodule. GitHub’s default clone leaves `components/ft8_lib/vendor` empty, and host `tx_e2e` / `idf.py build` will fail.

```
git clone --recurse-submodules git@github.com:kb2slo/micro-ft8.git
```

Existing trees: `git submodule update --init`.

Pushes and pull requests run GitHub Actions (ESP-IDF **v5.5.5**): the P4 (`poe_p4/`, target **esp32p4**) and the sidekick (`sidekick/`, target **esp32s3**). When firmware sources change, the jobs upload flashable merged images as artifacts. Docs-only commits (roadmap, README, and similar) skip the IDF builds and do not refresh the rolling images.

- Each merge to `main` that changes the firmware updates a prerelease at tag [`continuous`](https://github.com/kb2slo/micro-ft8/releases/tag/continuous), holding the P4 image, the sidekick image and a source archive, each named `YYYYMMDD-microft8-<kind>-<commit>.bin` so it still says what it is after it lands in your Downloads folder. The tag always points at the tip of `main`. Flash at `0x0`.
- Tags matching `v*` also create a versioned GitHub Release (`MicroFT8-PoE-P4-<tag>-Merged.bin`). How a release is cut: [`docs/RELEASE_PROCESS.md`](docs/RELEASE_PROCESS.md).

```
esptool.py --chip esp32p4 write_flash 0x0 20261002-microft8-poe-p4-60b64e3.bin
```

Host autoseq (`host_mock`) and `tests/tx_e2e` CTest run in a separate job. Hardware CAT and flashing are still local.

---

# Original README (Wei AG6AQ)

The hardware list, thanks, and operation manual below are Wei’s text as of the split from [wcheng95/Mini-FT8](https://github.com/wcheng95/Mini-FT8). Use the fork notes above where they disagree (Charge Mode, USB Drive, Band config, Station.txt clock, clone/CI).

Subscribe to [https://freelists.org/list/qrp-portable](https://freelists.org/list/qrp-portable) for announcements, discussions, and updates about my Mini-series apps for the Cardputer ADV.

### Dean, KD3AN had added support for IC705: https://github.com/hamrec/cp705
### Michael, 2E0MIK had added support for FTX-1: https://github.com/mapoby/Mini-FT8

# First POTA activation (v1.0 2025-12-31)
![First POTA Activation](IMG_6087.jpeg)

## Mini-FT8 Release Notice
Mini-FT8 is built on Karlis Goba’s ft8_lib. It’s also a joint adventure between Zhenxing (N6HAN) and Wei (AG6AQ), with inspiration from DXFT8 by Barb (WB2CBA) and Charley (W5BAA). It has been a great learning platform for me, and I hope you find it just as fun to use. It supports FT8 and FT4 with QMX, QDX, and KH1.

### Thanks

- The [DX FT8](https://github.com/WB2CBA/DX-FT8-FT8-MULTIBAND-TABLET-TRANSCEIVER) team — Barb (WB2CBA), Charley (W5BAA), and Paul (G8KIG) — for the inspiration.
- Zhenxing (N6HAN) — for helping build the audio/DSP path (UAC) and autoseq. This project would not have been possible without his help.
- Karlis Goba — for [ft8_lib](https://github.com/kgoba/ft8_lib). Thanks also to Shawn Rutledge for non-standard callsign support.
- OpenAI and Anthropic — for their incredible coding assistance.
  
### Hardware
(I have no affiliation with the vendors.)
  - Must order: https://shop.m5stack.com/products/m5stack-cardputer-adv-version-esp32-s3 or from digikey: https://www.digikey.com/en/products/detail/m5stack-technology-co-ltd/K132-ADV/27685158
  - Optional: M5Stack LoRa-1262 cap GNSS for Date/Time/Grid — the only supported GPS in this fork; only the GNSS side is used. No setting to enable it: fit it and it works
  - **Not supported in this fork:** a PORTA/Grove GPS puck. That port is the companion sidekick's
  - Optional: DS3231 RTC module on I2C `G8/G9` (for retained UTC date/time without GPS)
  - For KH1 TX: https://shop.m5stack.com/products/4pin-buckled-grove-cable, for a custom serial cable
  - For KH1-USBC RX: [USB-C microphone adapter](https://www.amazon.com/dp/B0FWC9ZFC4?ref=ppx_yo2ov_dt_b_fed_asin_title&th=1). Other adapters may also work, but this one is confirmed. KH1-MIC uses the Cardputer built-in microphone, so the USB-C adapter is optional.

73, Wei AG6AQ

# Mini-FT8 Operation Manual

## Quick Mode Map

| Key | Mode | Purpose |
|---|---|---|
| `R` | RX | View decoded messages and tap one to start a QSO. |
| `T` | TX Queue | View and manage the transmit queue. |
| `S` | STATUS | Access beacon, connect/sync, band step, tune, and date/time functions. |
| `G` | GPS | View GPS telemetry and synchronization status. |
| `M` | MENU P1 | Configure core station and operator settings. |
| `N` | MENU P2 | Configure radio, input, and comment settings. |
| `O` | MENU P3 | Configure logging, active bands, copy-to-SD, and retry settings. |
| `Q` | QSO | Browse QSO and log files, and view entries. |
| `D` | Delete Files | Browse and delete files stored in internal FATFS. |
| `B` | BAND | Edit per-band frequencies. |
| `C` | USB Drive | Toggle internal FATFS ownership between Mini-FT8 and the PC. |
| `P` | Performance | View A Simple Performance Monitor. (added in V2.0.4)|

## Global Keys and Navigation

- `R` / `T` / `B` / `S` / `G` / `Q` / `D` / `C`: switch to the selected mode. Press the same mode key again to return to `RX`.
- `M` / `N` / `O`: jump to MENU page 1 / 2 / 3. Press the current page key again to return to `RX`.
- `` ` ``: cancel TX globally in `RX`, `TX`, and `STATUS` when not editing.
- `▲` / `▼`: page up / page down in `RX`, `TX`, `BAND`, `MENU`, `QSO`, and `Delete`.
- `◀` / `▶`: move left / right in `QSO-SNR`, `STATUS` date/time, `MENU P2` (N->2).
- `1`..`6`: always select the currently visible row in the active mode.

## Per-Mode Controls

- ` acts as ESC where applicable.
- Text Edit: Backspace deletes, ` cancels, Enter saves.
  
| Mode | Item | Notes |
|---|---|---|
| `R` (RX) | `1..6` | Select a decoded line to reply to. CQ messages are sorted from strongest to weakest. If selected within 4 s, TX starts immediately. |
|  | `▲` `▼` | Page up/down is available when line 1 or line 6 is cyan. |
| `T` (TX Queue) | `1` | Rotate the queue to the next same-parity entry. |
|  | `2..6` | Drop the queue item on the current page. |
|  | `` ` `` | Cancel TX immediately. |
| `G` (GPS) |  | View live GPS telemetry including active source, 3D fix, satellites, UTC time, grid square, and last synchronization age. |
| `S` (STATUS) | `1` | Cycle Beacon mode. Applies when leaving STATUS mode. |
|  | `2` | Run connect/sync now; starts audio and follows the CAT sync path. |
|  | `3` | Step to the next active band. Applies after key 2 is pressed or when leaving STATUS. |
|  | `4` | Toggle Tune. |
|  | `5` | Edit Date (in place). On the Time line, `G` means GPS time and `R` means DS3231 RTC time. |
|  | `6` | Edit Time (in place). |
| `M` (MENU P1) | `1` | Cycle CQ Type. For CQ FD, enter operating class and ARRL/RAC section in FreeText, for example `1B SCV`. |
|  | `2` | Send FreeText once. |
|  | `3` | Edit FreeText (Long Edit). Used for SOTAMAT, park/summit reference, ARRL Field Day exchange, CQ modifiers (`CQ EU`, `CQ ASIA`), and similar text. |
|  | `4` | Edit Call (in place). |
|  | `5` | Edit Grid (in place). Supports 4/6/8-character grid. If GPS is available, the GPS grid is shown and used, but not saved. |
|  | `6` | Enter Sleep. Shows battery info. |
| `N` (MENU P2) | `1` | Select offset source: Random / RX / Fixed. Random values are within 500-2500 Hz. |
|  | `2` | Edit fixed cursor offset (in place). Enter directly or use `▲` `▼` `◀` `▶`. |
|  | `3` | Select radio (`QMX` / `QDX` / `KH1-USBC` / `KH1-MIC`). |
|  | `4` | Edit ignore list (Long Edit). Prefixes are separated by spaces; maximum 64 characters. |
|  | `5` | Edit comment (Long Edit). Used for ADIF logging. Supports `/Radio` and `/Grid` macro expansion. |
|  | `6` | Select FT8 / FT4 protocol. Reboot to apply the change. |
| `O` (MENU P3) | `1` | Turn RxTx log on/off. Note: RxTxLog has been renamed to `RT[YYMMDD].txt`. |
|  | `2` | Turn SkipTX1 on/off. Skips `dxcall mycall mygrid` and replies with the SNR report. |
|  | `3` | Edit active bands (Long Edit). Used by STATUS -> Band. |
|  | `4` | Copy files to SD. Feedback is `Copied OK` or `Missed [n]`. **Was `5`** — the `GNSS_LoRa` row above it was removed in this fork, and MENU positions are derived from the row order. |
|  | `5` | Edit max retry (in place). Accepts any natural number or `0`. **Was `6`.** |
| `Q` (QSO) | `1..6` | Open the selected ADIF file. |
|  | `◀` `▶` | Switch columns (Default view or SNR view). |
| `D` (Delete Files) | `1..6` | Delete the selected file immediately, without confirmation. |
| `B` (BAND) | `1..6` | Choose a band slot to edit. |
| `C` (USB Drive) |  | Stop radio audio and expose FATFS to the PC. Safely eject it on the PC, then press `C` again to remount storage and return to RX. |
| `P` (PERFORMANCE) | | A Simple Performance Monitor. (added in V2.0.4) |

## Download Logs

- Mini-FT8 and Mini-CW share the `fatfs` partition. Their files can coexist,
  and current M5Launcher installs/reinstalls can switch between the applications
  while preserving an existing compatible FATFS partition. Both applications
  use 512-byte FATFS and wear-levelling sectors.

- Use SD
  - Insert a FAT/FAT32-formatted SD card.
  - In MENU P3 (`O`), press `5` (Copy files to SD). All files will be copied to the SD card.
  - If the result shows `Missed`, a reboot will usually fix it.

## GPS Connections

**GPS over PORTA was removed in this fork, along with the `GNSS_LoRa` setting.** There is now one GPS source and no choice to make: the M5Stack LoRa-1262 cap GNSS on UART2 (`RX=G15`, `TX=G13`) at 115200 baud. The LoRa/SX1262 radio side is not used and never was. Fit the cap and GPS works; leave it off and there is simply no fix — presence is observed, not configured.

Wei's PORTA GPS wiring below therefore does not apply to this fork. A Grove GPS puck on `G1`/`G2` will not be read.

**Why:** PORTA and the cap were alternatives, and selecting the cap meant the PORTA port was never opened at all. That port carries the companion sidekick, so a GPS setting could silently disable the companion link — and in the headless direction this fork is heading, where the sidekick *is* the user interface, it would have removed the only UI along with the only control able to restore it. One device per port, chosen at build time rather than by a menu item, removes the whole class of problem.

Two consequences worth stating plainly:

- **The G4/G5 debug UART is gone.** `G5` is the cap's SX1262 chip-select, so the console and the cap could never coexist; the setting existed to pick one. Those pins are now always left to the cap. Use the on-screen log (`P` then `.`) instead — and the console is coming back on better wiring: once the companion protocol carries log events, the sidekick prints them to **its** USB-C, which is idle while the Cardputer's own USB-C is busy hosting the radio. That is the case the G4/G5 console existed for and never served well.
- **MENU P3 renumbered.** Copy-to-SD moved from `5` to `4`, max retry from `6` to `5`.

The GPS view shows the active source on its first line.
```text
┌──────────────────┐                 ┌─────────────────────────────┐
│ GPS              │                 │ Cardputer ADV               │
│                  │                 │ PORTA                       │
│ GND ─────────────┼─────────────────┤ GND                         │
│ VDD ─────────────┼─────────────────┤ 5V                          │
│ RX  ─────────────┼<──(Not Used)────┤ TX (G2)                     │
│ TX  ─────────────┼────────────────>┤ RX (G1)                     │
└──────────────────┘                 │                             │
                                     │ SW: 5VOUT (Left)            │
                                     └─────────────────────────────┘
```

## DS3231 RTC Connections

Mini-FT8 can use an optional DS3231 module as an external UTC clock. Connect it
to the Cardputer Adv shared I2C bus: `SDA=G8`, `SCL=G9`, plus module power and
ground. On boot, a valid DS3231 time is used before the ESP RTC or saved
`Station.txt` time. Status `S -> 6` appends `R` when the active time came from
the DS3231, and appends `G` after a full GPS time sync. GPS and manual time
updates write the DS3231 when it is present; FT8 decode fine corrections do not.

## KH1 Connections
![KH1 Cables](kh1_cables.jpeg)

 - TX Only ([sotamat](https://sotamat.com/))
```text
┌──────────────────┐                 ┌────────────────────────────┐
│ KH1 RS232        │                 │ Cardputer ADV              │
│                  │                 │ PORTA                      │
│ GND ─────────────┼─────────────────┤ GND                        │
│                  │                 │ 5V (NC)                    │
│ Tip(Rx) ─────────┼<────────────────┤ TX (G2)                    │
│ Ring(TX) ────────┼───(Not Used)───>┤ RX (G1)                    │
└──────────────────┘                 │                            │
                                     │ SW: NA                     │
                                     └────────────────────────────┘
```
- TX + RX (FT8/FT4 QSO)
  - Choose `KH1-USBC` for USB-C audio adapter RX. Tested adapter: Amazon `B0FWC9ZFC4`. Other adapters may also work, but this one is confirmed.
  - Choose `KH1-MIC` for Cardputer microphone RX. No USB-C audio adapter is needed.
  - For `KH1-USBC`, supply 5 V to PORTA; otherwise, the USB-C OTG port will not be powered. **Make sure the micro switch is on the right**
```text
┌──────────────────┐
│ Power Cable      │
│ GND ─────────────┼─────────┐
│ 5V  ─────────────┼─────┐   │
└──────────────────┘     |   |
┌──────────────────┐     |   |       ┌────────────────────────────┐
│ KH1 RS232        │     |   |       │ Cardputer ADV              │
│                  │     |   |       │ PORTA                      │
│ GND ─────────────┼─────)───┴───────┤ GND                        │
│                  │     └───────────┤ 5V                         │
│ Tip(Rx) ─────────┼<────────────────┤ TX (G2)                    │
│ Ring(TX) ────────┼── (Not Used)───>┤ RX (G1)                    │
└──────────────────┘                 │                            │
                                     │ SW: 5VIN (Right)           │
                                     └────────────────────────────┘
```

- Mini-FT8 automatically sets KH1 TX power to 2 W.
- For best RX performance, reduce AF volume to `05` or `06`.
