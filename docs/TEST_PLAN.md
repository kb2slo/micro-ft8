# Micro-FT8 test plan (kb2slo fork)

A ledger, not a manual: what automation checks, what it cannot see, and which field checks are still owed.
How a field check is asked for, run and closed is in [README.md](README.md) (**Test plan matches the work
too**). Git history records every check that passed.

## Automated

An agent runs these itself before saying work is done. CI runs them too. No hardware.

| Check | Command | Covers |
| --- | --- | --- |
| `host_mock` | `make -C host_mock`, then run `host_test` over each `test_*.json` and every `host_test_*` binary. CI's **Autoseq host_mock** step is the reference loop | Pure logic: parsers, formatters, policies, the Port A codec, pairing, the page templater, the bundle manifest. What each binary pins is in its own source |
| `tests/tx_e2e` | `cmake -S tests/tx_e2e -B tests/tx_e2e/build && cmake --build tests/tx_e2e/build -j4 && ctest --test-dir tests/tx_e2e/build --output-on-failure` | Encoder, TX state machine and timing, golden-WAV RX decode. It is a separate CMake project and the easiest one to forget |
| ADV build | `idf.py build` | `main/` under `-Werror`; must be warning-free |
| P4 build | `idf.py -C poe_p4 build`; CI job **PoE-P4 (ESP32-P4)** | The P4 image compiles and links |
| Sidekick build | `idf.py -C sidekick build`; CI job **Sidekick (ESP32-S3)** | The sidekick compiles and stages into the ADV image |
| Web bundle manifest | `python3 webapp/tools/gen_manifest.py --check` | `manifest.json` matches every `webapp/app/` file |
| README audit | CI job **README audit** (PRs) | A removed `UIMode` enumerator or a changed key binding under `main/` must touch `README.md` |
| Decode benchmark | `test_apps/decode_bench/README.md`. Local only; the device half needs a board on USB | Decode time per configuration against the 2.36 s guard; host and device decode the same messages |

## What automation cannot see

A green run is not coverage of these:

- **What any screen paints.** No test runs a `draw_*` function. Screen and MENU *navigation* are host-tested;
  whether a MENU row's label matches what it does is checked only by eye.
- **The main loop and slot timing.** Slot alignment, decode deadlines, TX start latency.
- **Decode tuning.** `golden_rx` catches decoder regressions. It uses its own candidate and LDPC limits, not
  the firmware's, so a tuning change moves none of its output. The decode benchmark measures tuning.
- **Autoseq transcripts.** `host_test` fails a scenario only on a crash, not on a wrong exchange.
- **Every hardware path.** USB host and UAC audio, CAT, display, GPS, RTC, SD, flash, WiFi, the sidekick's
  HTTP server and button.

## Pending

Field checks owed on shipped work. Only the operator can run these. Add one when a change touches a
field-only path, naming the change and the exact check. **Drop it when the check passes.**

| From | Check | Status |
| --- | --- | --- |
| RR73 once (the recently-worked check moved into autoseq) | Beacon on, either board. A station answers and the QSO runs to our RR73; they send 73. We send **one** RR73, the entry leaves the queue, and the next CQ goes out on our next slot. Before the fix, RR73 repeated on every retry (up to 5 more) with the beacon silent. Then, within 10 minutes, that station calls us again: no QSO starts (P4 console: `worked recently, not starting a new QSO`). | **owed** |
| Parked entries expire after 10 minutes | Answer a station's CQ and finish with our 73. More than 10 minutes later, have the same station call us with their grid. A new QSO starts: our first TX to them is a fresh report (not `73`), and when it completes there is a second log entry for that call. The P4 console shows `Expired parked ctx for <call>`. | **owed** |
| A queue cancel disarms (the head check before the TX trigger) | Either board, web app. Queue a reply to a station and wait for our TX to be armed: the decode has landed and our slot has not started. Tap that queue row's ×. Nothing goes out in our slot; with the beacon on, a CQ goes out instead. Before the fix, the cancelled message went out once, and the next queued QSO lost a retry. | **owed** |
| B76 (fall back to AP mode on mid-session WiFi loss) | Join a real network; Settings > Network shows "Station (joined)". (1) **Permanent loss**: power off the router. The monitor shows `Join failed (...), retry N/20`, then `Lost '<ssid>' and could not rejoin after 20 tries ... -- falling back to the AP`, and the AP comes up. Join it, reach `app.html`, and confirm the credentials survived (right SSID once the network is back). Watch for a crash or hang at the moment of fallback: `esp_restart()` runs from the WiFi event callback, a context nothing else restarts from. (2) **Transient loss**: reboot the router instead. The sidekick rejoins the original network within the retry window, with no fallback. (3) The main board's Port A link survives the fallback restart; a brief gap is expected. | **owed** |

### Unverified, no check planned

Shipped code that has never run on hardware, with no check scheduled. Listed so it is not mistaken for
proven, and not counted as owed. Move one into the table when a check is planned.

- **QDX.** Neither main board has ever run against one; the operator has no QDX. The ADV's Radio setting and
  the site's QDX mentions are untested.
- **B27, decode auto-sync persisting to a DS3231** (ADV only). Needs a generic DS3231 breakout (I2C 0x68; not
  M5Stack's Grove RTC Unit, a HYM8563): offset the clock, let auto-sync fire (`SYNC: Applied RTC sync…`),
  power-cycle without re-syncing, and the Time line reads ` R` with the corrected time.
- **B36, a failed USB host install recovering** (ADV). The retry path exists, but the race does not reproduce
  on the bench, so a clean start proves nothing. If `S` then `2` ever fails, press it again before
  power-cycling, and capture the reason the log (`P` then `.`) names.
- **TX HUD abort reason, sourced from firmware** (`PORTA_EVT_TX_HUD`'s `reason`). Needs a live low-battery TX
  abort to show the real reason on Operate. Only one abort reason exists in `main.cpp` today, so even a pass
  cannot show two reasons are distinguished.
- **The cap's GNSS and the sidekick together** (ADV). They were mutually exclusive before Grove GPS was
  dropped: `G` reads `Src:cap GNSS`, the sidekick's HELLO still arrives (`P` then `.`) with the cap fitted,
  and a `Station.txt` still carrying `gnss_lora=` loads.
- **Android.** Nothing has been checked on an Android phone, including whether `micro-ft8.local` resolves over mDNS (on the sidekick's AP with mobile data on, and on a home router). It should work on Android 12+. Low priority at this project's scale: not every client gets tested.
- **Sidekick HTTP socket count.** `max_open_sockets = 10` in both servers (`sidekick/main/wifi_prov.c`) was
  chosen without an on-device heap reading.
