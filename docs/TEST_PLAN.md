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
| Web bundle build | `npm ci --prefix webapp/tools` once, then `python3 webapp/tools/build.py`; CI's **Web bundle build** step | Every page minifies and every script, inline or not, still compiles; no file over the sidekick's 512 KB limit; the bundle within its 800 KB budget. It does not run the pages: a minifier that changes behaviour without breaking syntax gets past it, so a local `--dist` preview (architecture/webapp.md, **Build**) is the check for that |
| Webapp decode batching | `node webapp/tests/test_decode_batching.js` | Extracts and runs `app.html`'s own `applyEvent`/`flushDecodeBatch`/`decodeSortCmp` — a poll response spanning more than one real FT8 slot renders as separate batches, not merged into one; with Port A v2 fields, a slot is timed at its end on the slot grid, labelled with the host's frequency, and a slot retuned partway through becomes two batches that keep every decode |
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
| Settings saves reach autoseq, ADV half (the ADV pushes web config into autoseq; the P4 half passed 2026-09-28) | ADV, web app. (1) CQ on. In Settings, change Max retry (e.g. 5 → 8) and tap **Save station** while our TX is on the air. Max retry reads the new value (Settings' Save no longer sends band or frequency, so no VFO refusal appears). (2) Answer a CQ: the new queue row reads `retry 0/<new value>`. (3) Change CQ type to POTA and save: the next CQ is `CQ POTA …` without a reboot. | **owed** |
| RR73 once (the recently-worked check moved into autoseq) | Beacon on, either board. A station answers and the QSO runs to our RR73; they send 73. We send **one** RR73, the entry leaves the queue, and the next CQ goes out on our next slot. Before the fix, RR73 repeated on every retry (up to 5 more) with the beacon silent. Then, within 10 minutes, that station calls us again: no QSO starts (P4 console: `worked recently, not starting a new QSO`). | **owed** |
| Parked entries expire after 10 minutes | Answer a station's CQ and finish with our 73. More than 10 minutes later, have the same station call us with their grid. A new QSO starts: our first TX to them is a fresh report (not `73`), and when it completes there is a second log entry for that call. The P4 console shows `Expired parked ctx for <call>`. | **owed** |
| A queue cancel disarms (the head check before the TX trigger) | Either board, web app. Queue a reply to a station and wait for our TX to be armed: the decode has landed and our slot has not started. Tap that queue row's ×. Nothing goes out in our slot; with the beacon on, a CQ goes out instead. Before the fix, the cancelled message went out once, and the next queued QSO lost a retry. Then tap × on a QSO row **while it is transmitting**: the transmission stops at once (the TX HUD ends), and nothing else keys in that slot. On the ADV, the TX screen's drop key (`2`–`6`) on the transmitting entry does the same. | **owed** |
| B76 (fall back to AP mode on mid-session WiFi loss) | Join a real network; Settings > Network shows "Station (joined)". (1) **Permanent loss**: power off the router. The monitor shows `Join failed (...), retry N/20`, then `Lost '<ssid>' and could not rejoin after 20 tries ... -- falling back to the AP`, and the AP comes up. Join it, reach `app.html`, and confirm the credentials survived (right SSID once the network is back). Watch for a crash or hang at the moment of fallback: `esp_restart()` runs from the WiFi event callback, a context nothing else restarts from. (2) **Transient loss**: reboot the router instead. The sidekick rejoins the original network within the retry window, with no fallback. (3) The main board's Port A link survives the fallback restart; a brief gap is expected. | **owed** |
| Operate O1c: the Station sheet ([webapp.md](architecture/webapp.md), **The Station sheet**) | Web app, either board, 375 px phone width, light and dark. Step 1: on Operate, tap the CQ row's **Settings ›** (or the Sends text) — the Station sheet opens over Operate, showing Calling CQ, QSOs and a read-only Call footer in the app's own styling. Step 2: change Type from CQ to POTA — it applies at once, no Save button; the CQ row's Sends text and the dock's second line both update to `CQ POTA …`. Step 3: change Type to Free text — a "CQ message" field appears; type a short message and move to another field — it writes at once and the CQ row's Sends text shows that message verbatim. Step 4: change Type to FD — an "FD exchange" field appears holding the same text just typed (the two types share one key); replace it with something like `1B ENY` — it writes at once. Step 5: in Grid, type something malformed like `AB` and move to another field — a dialog says it is not a valid grid, and nothing is sent (the Sends preview and the dock still show the previous grid). Step 6: type a real grid (e.g. your own) and move to another field — it writes at once, the note below reads "The CQ carries the first four characters", and the CQ row's Sends text and the dock both pick it up. Step 7: tap **Set from GPS →**, confirm the prompt and grant your phone's location — the browser returns to this same sheet (not Settings) with the update-grid prompt. Step 8: in QSOs, change Max retry and TX offset — each writes at once, and the dock's line ("Retries N · … · Station ›") updates to match. Step 9: close the sheet, then open it again from the dock's **Station ›** link instead of the CQ row — it opens the same sheet with the same values. Step 10: open Settings (the header gear): confirm Type, the free-text field, Grid, Max retry, Offset Hz and Offset src are no longer there, change Comment or Ignore prefixes, and tap **Save station** — it saves, and the CQ row's Sends text, the dock line and the sheet's own values (reopen it to check) are unchanged from what Steps 2–8 left them. | **owed** |

### Unverified, no check planned

Shipped code that has never run on hardware, with no check scheduled. Listed so it is not mistaken for
proven, and not counted as owed. Move one into the table when a check is planned.

- **QDX.** Neither main board has ever run against one; the operator has no QDX. The ADV's Radio setting and
  the site's QDX mentions are untested.
- **Port A v2, a slot retuned partway through** (decodes from one slot under two headers, the same time, each
  under the frequency it was heard on). Not reproduced on the bench (2026-09-28, several tries): a decode lands
  on the new band only if the switch comes within a second or two of the slot start and the station is strong,
  or on the old band only if the switch comes after about 11 s and the rest still decodes. Covered by
  `host_test_dial_history` (a retune 3 s into a slot), the decode-batching test (a retuned slot as two batches
  that keep every decode) and the preview. No error appeared in the tries.
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
