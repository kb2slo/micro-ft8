# Micro-FT8 test plan (kb2slo fork)

A ledger, not a manual: what automation checks, what it cannot see, and which field checks are still owed.
How a field check is asked for, run and closed is in [README.md](README.md) (**Test plan matches the work
too**). Git history records every check that passed.

## Automated

An agent runs these itself before saying work is done. CI runs them too. No hardware.

| Check | Command | Covers |
| --- | --- | --- |
| `host_mock` | `make -C host_mock`, then run `host_test` over each `test_*.json` and every `host_test_*` binary. CI's **Autoseq host_mock** step is the reference loop | Pure logic: parsers, formatters, policies, the Port A codec, pairing, the page templater, the bundle manifest. Each scenario's `expect.tx` (and `expect.log` / `expect.cabrillo` when a QSO is logged) fails a wrong exchange. `test_qso.events` pins the event JSON that run emits. What each other binary pins is in its own source |
| Webapp SWR sweep | `node webapp/tests/test_swr_core.js` | `swr_core.js` under node: the host's point cost and caps equal `swr_sweep.h`'s; bands offered and defaulted from Settings; the plan, its estimate and request body; the event reducer; the countdown; the plot's geometry (band boundaries, 1 to 6 with an over-range clamp, a gap where nothing was measured, a skipped band, portrait and landscape drawing the same readings) |
| Station simulator SWR sweep | `node webapp/tests/test_sim_sweep.js` (same binary) | `sim_station` runs the real `swr_sweep` against a fake QMX: `/api/swr/start` needs the token, refuses a malformed body, a zero step and a second start; a band the fake radio lacks is reported skipped; the points, their SWR and the DONE arrive on `/api/events`; `/api/tx/cancel` stops a long sweep as ABORTED |
| Station simulator | `node webapp/tests/test_sim_station.js` (builds `host_mock/sim_station` via the host_mock step) | `sim_station` playing `test_qso.json` serves `/api/events`, and that poll contains the scripted decode and our TX text |
| `tests/tx_e2e` | `cmake -S tests/tx_e2e -B tests/tx_e2e/build && cmake --build tests/tx_e2e/build -j4 && ctest --test-dir tests/tx_e2e/build --output-on-failure` | Encoder, TX state machine and timing, golden-WAV RX decode. It is a separate CMake project and the easiest one to forget |
| P4 build | `idf.py -C poe_p4 build`; CI job **PoE-P4 (ESP32-P4)** | The P4 image compiles and links |
| Sidekick build | `idf.py -C sidekick build`; CI job **Sidekick (ESP32-S3)** | The sidekick compiles and CI merges its 0x0 image |
| Web bundle build | `npm ci --prefix webapp/tools` once, then `python3 webapp/tools/build.py`; CI's **Web bundle build** step | Every page minifies and every script, inline or not, still compiles; no file over the sidekick's 512 KB limit; the bundle within its 800 KB budget. It does not run the pages: a minifier that changes behaviour without breaking syntax gets past it, so a local `--dist` preview (architecture/webapp.md, **Build**) is the check for that |
| Webapp decode batching | `node webapp/tests/test_decode_batching.js` | Extracts and runs `app.html`'s own `applyEvent`/`flushDecodeBatch`/`decodeSortCmp` — a poll response spanning more than one real FT8 slot renders as separate batches, not merged into one; with Port A v2 fields, a slot is timed at its end on the slot grid, labelled with the host's frequency, and a slot retuned partway through becomes two batches that keep every decode |
| Decode benchmark | `test_apps/decode_bench/README.md`. Local only; the device half needs a board on USB | Decode time per configuration against the 2.36 s guard; host and device decode the same messages |
| Decode recall | `python3 tools/decode_recall/report.py`. CI's **Decode recall** step | Today's firmware settings against committed `jt9` truth (in-tree and `20m_busy`) and the synthetic AWGN grid. Fails if recall drops under the floors in `tools/decode_recall/baseline/today.txt`, or a synthetic signal at −6 dB or stronger is missed. It does not see USB audio or the stride decimator |

## What automation cannot see

A green run is not coverage of these:

- **What any screen paints.** No test runs a `draw_*` function. Screen and MENU *navigation* are host-tested;
  whether a MENU row's label matches what it does is checked only by eye.
- **The main loop and slot timing.** Slot alignment, decode deadlines, TX start latency.
- **Decode tuning on the air.** The recall harness pins the firmware's settings given a WAV. `golden_rx` uses its own candidate and LDPC limits, so a tuning change moves none of its output. Whether a busier candidate list still finishes inside the guard is the decode benchmark, on a board.
- **The board slot cycle.** The scenario stepper injects decodes and takes whatever autoseq has pending. It does not run the head check before the trigger, IgnoreList, or arming. Those stay board code.
- **Every hardware path.** USB host and UAC audio, CAT, display, GPS, RTC, SD, flash, WiFi, the sidekick's
  HTTP server and button.

## Pending

Field checks owed on shipped work. Only the operator can run these. Add one when a change touches a
field-only path, naming the change and the exact check. **Drop it when the check passes.**

| From | Check | Status |
| --- | --- | --- |
| 256 candidates (`DECODE_MAX_CANDIDATES`, was 50) | Flash the P4: `idf.py -C poe_p4 build flash` on the PROG port. Step 1: FT8 on 20 m at a busy hour, about 10 slots, serial console open. Each `DECODE: SLOT cand=… msg=… dec=…ms` has `dec` under 2000. Expected: decode finishes inside the 2.36 s guard, so the next slot's audio is not eaten. Step 2: answer a station, or let a QSO start, on the slot after one of those decodes. Expected: TX starts at the boundary. The HUD shows transmitting then, and the other station copies the start of the message. | **owed** |
| RR73 once (the recently-worked check moved into autoseq) | Beacon on. A station answers and the QSO runs to our RR73; they send 73. We send **one** RR73, the entry leaves the queue, and the next CQ goes out on our next slot. Before the fix, RR73 repeated on every retry (up to 5 more) with the beacon silent. Then, within 10 minutes, that station calls us again: no QSO starts (P4 console: `worked recently, not starting a new QSO`). | **owed** |
| Parked entries expire after 10 minutes | Answer a station's CQ and finish with our 73. More than 10 minutes later, have the same station call us with their grid. A new QSO starts: our first TX to them is a fresh report (not `73`), and when it completes there is a second log entry for that call. The P4 console shows `Expired parked ctx for <call>`. | **owed** |
| A queue cancel disarms (the head check before the TX trigger) | P4, web app. Queue a reply to a station and wait for our TX to be armed: the decode has landed and our slot has not started. Tap that queue row's ×. Nothing goes out in our slot; with the beacon on, a CQ goes out instead. Before the fix, the cancelled message went out once, and the next queued QSO lost a retry. Then tap × on a QSO row **while it is transmitting**: the transmission stops at once (the TX HUD ends), and nothing else keys in that slot. | **owed** |
| Sidekick factory reset (Settings > Network, `POST /factory-reset`) | Sidekick with the app installed and your home WiFi saved, phone paired. Step 1: Settings > Network > **Factory reset**. The first tap only asks you to tap again; nothing changes. Step 2: tap again. The page says it is restarting. Step 3: the sidekick's own `micro-ft8.local-…` network appears (your WiFi is no longer joined). Step 4: join it, open `http://192.168.4.1/`. The page shows **First-time setup**, and no "Operate in AP mode". Step 5: `/app.html` shows "The app is not installed yet". Step 6: press the sidekick button; the page pairs with a new token (the old browser pairing no longer works). Step 7: the main board still shows your callsign and grid, so its settings were kept. Step 8: repeat Step 1–2 while the sidekick is in AP mode with the app installed (Switch to AP first). It resets the same way. | **owed** |
| First-run setup pages (a flashed sidekick with no app shows the install steps; `/app.html` and AP-mode `/update.html` explain themselves) | Steps 1, 2, 4, 5 and 6 passed on build c9ad313. Step 7 (2026-10-01) found that `/update.html` left Install enabled and fetched `{{bundle_host}}/manifest.json` (HTTP 404); fixed in `update.html`, unproven. Owed, after reflashing the sidekick (`idf.py -p PORT flash`, which keeps the app and WiFi): Step 3: join the AP, pair (press the button), open `http://192.168.4.1/update.html`. It says installing needs the internet, Install stays greyed (including after pairing), and tapping it does nothing. Step 7: with the phone on the sidekick's AP and cellular on, note whether the Install on `/update.html` ever works. It should not, and the expected result is the Step 3 message. Not pass/fail; record what happens. | **owed** |
| B76 (fall back to AP mode on mid-session WiFi loss; permanent-loss fallback observed 2026-10-01) | The AP came up after a sustained loss and was usable, with no crash or hang at the `esp_restart()` from the WiFi callback. Still owed: (1) Credentials survived — once the network is back, Settings > Network shows the original SSID. (2) **Transient loss**: reboot the router. The sidekick rejoins that network within the retry window, with no fallback. (3) The main board's Port A link survives the fallback restart; a brief gap is expected. | **owed** |
| TX-arm re-checks a mid-slot CQ-type switch; P4 cancel disarms immediately (row `j4xh`'s Extraction A) | P4, web app. (1) Once a slot has been armed (a decode has landed, TX has not started yet) but before it fires, switch CQ type in the Station sheet (e.g. CQ → CQ POTA). The keyed message reflects the new type, not the one armed when the slot was picked. (2) While a message is transmitting, tap its queue row's ×. The radio de-keys and the TX HUD ends immediately, not on the tick after -- before the fix, the P4 deferred both to its next `tx_tick()`. | **owed** |
| P4 expands comment macros; P4 file-delete guard blocks during audio streaming (row `j4xh`'s Extraction B) | P4. (1) With `comment1` set to something with `/Radio`/`/Grid` (Station.txt), log a QSO. The ADIF record's `<comment>` has the radio name and grid substituted, not the literal `/Radio`/`/Grid` text -- previously the P4 skipped this entirely. (2) Start audio streaming (or leave decoding active) and try to delete today's log file from the web app. The delete is refused; before this fix the P4 only blocked on TX-active-or-decoding. | **owed** |
| `act_refs` derives CQ SOTA/POTA once, on activation start (RFC 0008 A1, Steps 2/3/5) | P4, web app, Station sheet Activation section. (1) With no active reference, add a POTA reference. CQ type switches to `CQ POTA` on its own, and the CQ text on the dock reads `CQ POTA <call> <grid>`. (2) With that reference still active, manually switch Type to something else in the sheet, then add a second POTA reference. Type stays what was manually chosen -- a later add must not re-derive it. (3) Remove all references, then add one POTA and one SOTA reference together (empty straight to a mixed activation). Type switches to `CQ SOTA` (a mixed activation defaults to SOTA). | **owed** |
| Port A v3: band/`MY_POTA_REF`/`MY_SOTA_REF` reach the export; a version mismatch cannot link (RFC 0008 A1, Step 4/6) | The P4 plus the sidekick, reflashed together (`PORTA_PROTO_VERSION` 2 → 3, a breaking wire change). (1) After flashing, the sidekick still pairs/links (HELLO succeeds) -- a board left on the old v2 image would not. (2) Work a QSO with an active POTA or SOTA reference. The QSOs tab's day link grows a count, e.g. `Download ADI (1 POTA)`. (3) Tap it: one `.adi` file downloads per distinct reference that day, named `CALL@REF-YYYYMMDD.adi`, each carrying `<band:...>` plus `MY_SIG`/`MY_SIG_INFO` (POTA) or `MY_SOTA_REF` (SOTA). (4) A day with no activation still shows plain `Download ADI` and downloads one unsplit file, exactly as before. | **owed** |

### Unverified, no check planned

Shipped code that has never run on hardware, with no check scheduled. Listed so it is not mistaken for
proven, and not counted as owed. Move one into the table when a check is planned.

- **QDX.** The P4 has never run against one; the operator has no QDX. The site's QDX mentions are untested.
- **Port A v2, a slot retuned partway through** (decodes from one slot under two headers, the same time, each
  under the frequency it was heard on). Not reproduced on the bench (2026-09-28, several tries): a decode lands
  on the new band only if the switch comes within a second or two of the slot start and the station is strong,
  or on the old band only if the switch comes after about 11 s and the rest still decodes. Covered by
  `host_test_dial_history` (a retune 3 s into a slot), the decode-batching test (a retuned slot as two batches
  that keep every decode) and the preview. No error appeared in the tries.
- **Android.** Nothing has been checked on an Android phone, including whether `micro-ft8.local` resolves over mDNS (on the sidekick's AP with mobile data on, and on a home router). It should work on Android 12+. Low priority at this project's scale: not every client gets tested.
- **Sidekick HTTP socket count.** `max_open_sockets = 10` in both servers (`sidekick/main/wifi_prov.c`) was
  chosen without an on-device heap reading.
