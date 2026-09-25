# Micro-FT8 test plan (kb2slo fork)

What is verified, by whom, and what is still owed. Companion to [ROADMAP.md](ROADMAP.md): the roadmap says
what we are building, this file says how we know it works. How to use both is in [README.md](README.md).

The split that matters: **an agent can prove some things and cannot prove others.** Everything under
[Automated](#automated) an agent runs itself and must run before saying work is done. Everything under
[Operator field checks](#operator-field-checks) needs real hardware — a Cardputer ADV, a QMX, an antenna,
sometimes a second station — and an agent must never claim it passed. Green host tests and a green build are
evidence about parsing, policy, and linkage. They are not evidence that the radio transmitted.

## Automated

Run by the agent locally, and by CI on every PR. No hardware.

**Run all three harnesses, not two.** `tx_e2e` is a separate CMake project and is easy to forget: a B22
include-path miss in it survived an entire branch of local `idf.py build` + `host_mock` runs and only
surfaced in CI. The commands below are the full local set.

| Harness | Command | Covers |
| --- | --- | --- |
| `host_mock` (26 binaries) | `make -C host_mock && host_mock/host_test*` | See table below. CI globs the binaries rather than listing them, so a new test runs as soon as the Makefile builds it |
| `tests/tx_e2e` | `cmake -S tests/tx_e2e -B tests/tx_e2e/build && cmake --build tests/tx_e2e/build -j4 && (cd tests/tx_e2e/build && ctest --output-on-failure)` | L1 encoder, TX state machine, poll timing, timer isolation, golden WAV RX decode, telemetry decode overflow, TA format — 7 tests |
| Decode benchmark (RFC 0006 P0) | `python3 test_apps/decode_bench/tools/make_slots.py`, then host: `make -C test_apps/decode_bench/host` and run `decode_bench_host data/slot_*.raw`; device: see `test_apps/decode_bench/README.md` (needs the board on USB, no radio; the P4 resets itself, the ADV needs G0/reset presses; nvs and storage are never written) | Firmware decode path timed per configuration against the 2.36 s guard; host/device decode-set parity. Local only, not in CI. |
| Firmware build | `idf.py build` | `main/` under `-Werror`; merged image; must be **warning-free** |
| Sidekick build | CI job **Sidekick (ESP32-S3)** | Companion firmware compiles and stages |
| README audit | CI job **README audit** | A PR removing a `UIMode` enumerator or changing a key binding under `main/` must touch `README.md` |

### `host_mock` coverage

| Binary | Covers |
| --- | --- |
| `host_test` | Autoseq engine: JSON-driven QSO, Field Day, beacon, reincarnation, deadlock, freetext, tail-end-touch scenarios. **This runner only asserts via exit code, not scenario content** — `for f in test_*.json; do ./host_test "$f" \|\| failed=1; done` in `ci.yml` fails only on a crash, not a wrong transcript. `test_rr73_touch.json`'s regression coverage was verified by hand (reverted the fix, confirmed the scenario reproduces `grid=RR73`; reapplied, confirmed `grid=` empty) — CI runs it every time but won't independently catch a future regression here the way an assertion-based `host_test_*` binary would |
| `host_test_unique_callsign` | Unique-callsign touch dedupe and promote. **`QsoContext::entry_id` (I28d, [architecture/webapp.md](architecture/webapp.md) (**Screens**), 2026-09-14):** distinct per real entry, survives being moved by a reshuffle that keeps the same contact, gets a fresh id when a re-touch actually drops and re-creates the context, and `autoseq_drop_by_entry_id` cancels by that stable id — finds an entry regardless of its current position, rejects the `0` sentinel and an id nothing carries without mutating the queue, and fails the second time on an id already dropped |
| `host_test_beacon_cancel` | Beacon-off cancels a queued CQ |
| `host_test_adif_merge` | ADIF merge export, the logger's 10-minute dedupe window, and record formatting: byte-for-byte layout against a real field record, the omit rules for empty grid / unset reports / empty comment, `<tag:N>` lengths matching their values, and a round-trip back through `adif_parse()` |
| `host_test_station` | `Station.txt` parse / serialize round-trip |
| `host_test_station_save_queue` | Save coalescing off the slot loop |
| `host_test_qso_browse` | Daily `.adi` filter, record page, list lines |
| `host_test_adif_delete` | Port A / Clear-all delete plan: clear-all vs single file, LOCK skip, Station/path/missing refusals |
| `host_test_file_list` | Directory listing / paging |
| `host_test_decode_sort` | Decode list ordering |
| `host_test_radio_profile` | Radio profile table; retired `KH1_USBC`/`KH1_MIC` values fall back to QMX |
| `host_test_band_config` | Band config parse and toggle policy |
| `host_test_power_hysteresis` | Battery hold / low-batt hysteresis |
| `host_test_copy_block` | Copy-to-SD blocking policy and menu line |
| `host_test_cts_time` | CTS / phone time parsing to `timeval` |
| `host_test_tx_hud_banner` | TX HUD banner state |
| `host_test_rx_list_stale` | RX list staleness marking |
| `host_test_usb_c_presence` | USB-C presence detection policy |
| `host_test_datetime_field` | STATUS date/time editor: cursor movement over separators, digit overwrite, and strict range validation. Carries regression cases for the dates `mktime` used to silently roll over, plus an exhaustive sweep of every day in a leap and non-leap year |
| `host_test_screen_model` | Screen navigation: key to screen, R never toggling, the seven plain toggles, M/N/O sharing MENU across three pages, P cycling stats to log to RX, and `C` staying inert after B23 |
| `host_test_porta_proto` | Sidekick/main frame codec (I28a): encode/decode round-trip, zero-length and 255-byte payloads, and a sync byte appearing *inside* a payload — which it will, since payloads are binary. Sweeps **every single-bit flip** in every framed byte and requires the CRC to reject all of them, which a sum-of-bytes checksum (what the old beacon used) does not. Pins recovery after truncation and after leading garbage, and pins the one deliberate limitation: a corrupt frame is dropped without rescanning the bytes it consumed, so a stray sync byte in noise costs exactly one real frame. Counters survive the decoder's own resync but not an explicit init — a distinction that started as a bug, since preserving them meant reading uninitialised memory on first use. Also pins **TX cancel** ACTION verb (I33 dropped `TX_FREE`), **CONFIG_GET/SET/VALUE** key/value framing, and **CONNECT / TUNE** (`PORTA_ACT_TX_CANCEL` / `CONNECT` / `TUNE`): cancel/connect are verb-only, tune is verb + on byte. **I28d protocol slice, [architecture/webapp.md](architecture/webapp.md) (**Screens**) (2026-09-14):** `BEACON` / `QUEUE_CANCEL` / `QUEUE_REPLY` ACTION verbs and `QUEUE_ENTRY` / `SLOT_STATE` EVENT subtypes, plus `FILE_LIST` / `FILE_READ` / `FILE_DATA`. Pins that `QUEUE_ENTRY`'s `entry_id` round-trips independently of state/retry/dxcall (it is a stable identity, not a queue position — the queue re-sorts by priority, so `QUEUE_CANCEL` must not address by index), that `IDLE` round-trips like any other state rather than needing a separate removal flag, that a dxcall/filename length byte claiming more than the frame carries is rejected rather than read past the payload, and that the `-99` no-report sentinel on `FILE_DATA` entry rows survives round-trip like any other value. Codec only — dispatch in `main.cpp`/`porta.cpp`, the decode-id-to-message lookup, and the sidekick HTTP relay are not built yet. **`TX_HUD.offset_hz` (2026-09-15):** round-trips at 1500 Hz, and the "unknown sentinels" case pins it at 0 (not -1 -- it's unsigned, unlike `power_dw`/`swr_c`/`battery_pct`) when there's no pending TX |
| `host_test_pairing_token` | Sidekick API authorization (I28b, [architecture/system.md](architecture/system.md) (**Who may key the transmitter**)) — the module that decides who may key a transmitter, so the cases pinned are the ones that would fail *open*. The sharpest is that a guarded route with **no token minted denies**: NVS returns either `NULL` or `""` for a missing key, and both are checked, because "not yet configured" meaning "open to everyone" is the worst failure available here. An unminted token also refuses a real one, and a malformed stored token cannot be matched even by itself. Open routes are checked to ignore pairing state entirely — including with no token minted and with a wrong one presented — since reads are open on principle rather than as a function of pairing. Token comparison does not short-circuit and rejects a token differing only in its last character. Hex is high-nibble-first, checked against a known byte pattern, since an inverted pair would still round-trip through the comparison and only show up against an externally captured value. Two deliberate limitations are pinned as decisions rather than left to be rediscovered: uppercase hex is rejected rather than folded, and the header accepts one shape only (no `Bearer` prefix). There is deliberately **no route-table test** — which routes are open is a required argument where each route is registered, so that property is a build error rather than a default a test would have to police. The rest of the module needs hardware and is covered by S2e: NVS minting and persistence, the 401 path, and the disclosure window |
| `host_test_web_page` | Bootstrap page templating (`web_page.{c,h}`) — the pages are real `.html` files now, and this is the substitution that replaced `snprintf`. The load-bearing property is that **values escape by default**: the status page used to interpolate the stored SSID raw, and an SSID is not the operator's own text but whatever a nearby access point broadcast, chosen from a scan list — a network named with a `<script>` tag executed on the origin that holds the pairing token in `localStorage`. So escaping is the default and passing markup through takes an explicit `already_html`, set only for the scan list, whose SSIDs are escaped per item. The hostile SSID is pinned in the shape it would actually arrive. Overflow is **refused, not truncated**, and the buffer is left empty: a truncation could cut an entity in half or cut off an attribute's closing quote and swallow the rest of the tag, so exact-fit and one-byte-short are both checked. An existing `&amp;` escapes once rather than being interpreted, which is what stops output doubling on a second pass. Unknown and unterminated placeholders pass through verbatim, so a typo renders as `{{ssdi}}` on the page instead of silently becoming an empty field — the failure is visible rather than invisible. Expansion is chunked rather than buffered, so a failed emit mid-page aborts instead of writing into a closed socket, and the tests reassemble the chunks to catch a boundary off-by-one. A key that prefixes another must not match it. There is deliberately no test that the page *files* are well-formed — that is what pointing a browser or a validator at them is for, which is the whole reason they left the C source |
| `host_test_web_manifest` | I28c bundle manifest parse + API version gate ([architecture/sidekick.md](architecture/sidekick.md) (**Installing the app bundle**)) — constrained JSON (`api_min`, `assets[]` with `path`/`sha256`/`size`), path jail (no `..`, no leading `/`, no dotfiles), lowercase-only SHA-256 hex, duplicate paths refused, unknown fields refused. Load-bearing: a future `api_min` still **parses** so begin can say “too new”, then `web_manifest_api_ok` refuses it against `WEB_API_VERSION`. Device stage/verify/promote is field-checked, not host-tested |
| `host_test_bundle_host` | I28c bundle-host URL gate — `https://` only, no trailing slash, no whitespace/quotes, length bounded. Soft identity: the default string is today’s Pages base and is not part of the validation rule |
| `host_test_menu_model` | MENU layout arithmetic (page/key round-trip for all 17 rows), row identity and order, inline-edit character classes and filter, and the long-edit rules (per-kind case handling and the ignore-list cap). Carries regression cases for both defects B32 fixed |

### What automation cannot see

Named explicitly so nobody mistakes a green run for coverage. None of the following has a harness:

- **The UI rendering.** No test covers any `draw_*` function; what each screen actually *paints* is
  unverified. Screen *navigation* is covered by `host_test_screen_model`, and the MENU is now
  partly covered: `host_test_menu_model` pins layout, row order, and the edit filter, and
  `menu_assert_model_in_sync()` logs at startup if the label/action table drifts off the model. What stays
  uncovered is whether a row's *label* and *action* actually belong together — that pairing is still only
  verifiable by eye, which is why section 2 below checks label and effect together.
- **The main loop and slot state machine.** `app_task_core0`, `tx_tick`, `check_slot_boundary`.
- **The decode *parameters*, though not the decode *path*.** `tests/tx_e2e`'s `golden_rx` does run
  `monitor_process` → `ftx_find_candidates` → `ftx_decode_candidate` over seven committed WAVs, so decoder
  regressions are caught. What it does not catch is anything about our *tuning*: `decode_pcm()` hardcodes its
  own `max_cand`, `min_score` and LDPC iteration count rather than the firmware's, reads `time_osr`/`freq_osr`
  as literals instead of the firmware's `s_time_osr`/`s_freq_osr` (`decode_tx_state.cpp`), stops at the first successful decode so it cannot count
  yield, and skips the firmware's per-block AGC and decimation entirely. Every parameter in I26 could change
  without moving one bit of its output. B25 is the harness that would notice.
- **Any field-only path**: USB host / UAC audio, CAT, CDC, display and SPI, GPS, DS3231, SD card, flash.
- **Timing.** Slot alignment, decode-window deadlines, TX start latency.

## Operator field checks

Real hardware. An agent proposes these and never marks them passed.

Standing rig: Cardputer ADV + QMX (or QMX+) over USB-C. The Cardputer's USB-C is **either** ESP serial/JTAG
**or** USB host for the radio, never both — so with the radio attached there is **no serial console at all**.
The G4/G5 console went with the `GNSS_LoRa` setting: `G5` is the cap's SX1262 chip-select, the two could
never coexist, and the setting existed only to choose between them. The on-screen log is the console now.

**Where the on-screen log lives: `P`, then `.`** — the PERF screen's second page. `;` returns to the stats
page, `.` pages further down the log. Every "the log shows …" expectation below means that screen.

Naming trap, and it has already cost bench time: the `D` screen is **Delete Files**, not a debug log, even
though its enum is `UIMode::DEBUG`. Pressing `1`–`6` there **deletes a file immediately, with no
confirmation**. Do not go looking for log output on `D`.

### 0. Smoke — do this first, it gates the rest

**Sync the clock before expecting any decode.** FT8 slots are 15 s and the decoder
needs UTC within roughly a second; an unsynced clock produces an empty R list that looks exactly like a dead
audio path. On a cold unit with no DS3231 backup, decodes will not appear no matter what else is right.

There is an ordering constraint here that is easy to trip over: **phone sync needs the radio unplugged from
USB-C** (BLE and USB host cannot both own that port), while **audio needs it plugged in**. So the clock step
comes first, with the radio disconnected.

| # | Do | Expect |
|---|---|---|
| 0.1 | Power on, radio **not** connected | Boots to the `R` screen, no crash loop |
| 0.2 | `S`, read the Time line suffix | ` R` = DS3231, ` G` = GPS, ` P` = phone. **No suffix means the clock is not from a trusted source** — continue to 0.3 |
| 0.3 | If unsynced, `H` then `1` ("Start sync"), pair from nRF Connect / LightBlue to `Micro-FT8-<call>` | Sync completes; `S` Time line now reads correct UTC with a ` P` suffix |
| 0.4 | Alternatives if no phone: `G` and wait for a GPS fix, or `S` `5` / `6` to set date and time by hand | Time line shows correct UTC (` G` for GPS; manual entry shows no suffix) |
| 0.5 | Confirm the time against a known-good clock | Within about a second of UTC |
| 0.6 | Connect the radio to USB-C | — |
| 0.7 | `S` then `2` | Audio starts; waterfall moves; status shows `Sync to QMX` |
| 0.8 | Return to `R`, wait 1–2 slots | Decodes appear; countdown bar animates in step with the slot |
| 0.9 | **If `S`→`2` ever fails, press it again before power-cycling** | It must be able to succeed on a retry. Before B36 one failed USB host install was terminal for the boot: `Audio start fail` repeated until a power cycle. The log (`P` then `.`) now names the reason — `USB host retry after …`, `USB host timeout (…)`, `USB host install: …`, `UAC already started` — instead of only `Audio start fail` |

If 0.7 fails, stop — everything below assumes audio. If 0.7 works but 0.8 shows nothing, **re-check 0.2**
before suspecting the audio or decode path; an unsynced clock is the more common cause and looks identical.

Once a DS3231 is fitted and set, it holds time across power cycles, so 0.2 should read ` R` on later boots
and steps 0.3–0.5 can be skipped.

### 1. Screen reachability

Press each key from the `R` screen and confirm the screen appears and its title is right. Then press the
same key again (or `R`) and confirm you return to `R`.

`R` · `T` · `B` · `M` · `N` · `O` · `Q` · `D` · `S` · `G` · `P` · `H`

`C` must do **nothing** — USB Drive was removed (B23). A visible reaction to `C` is a bug.

### 2. MENU — every item, every page

**This section is the one that matters most right now.** The whole menu was rewritten from a positional
list plus a page/key `if` chain into a single table (B32). Each item's text, action, and edit behaviour used
to be defined in three separate places; if any pairing got crossed in the rewrite, it shows up here as a row
whose *label* is right but whose *action* belongs to a different row. **Check the label and the effect
together, item by item — not just that something happened.**

For each row: press the key, confirm the effect, and confirm no *other* row changed.

#### MENU P1 (`M`)

| Key | Label reads | Press it and expect |
|---|---|---|
| `1` | `CQ Type:<type>` | Cycles CQ / SOTA / POTA / QRP / FD / FreeText, wrapping after 6 presses back to `CQ` |
| `2` | `F:<text>` | Opens the long-edit screen for FreeText; typed letters appear **upper case**; `` ` `` cancels leaving the old value, Enter saves. **Was `3`** — Send FreeText removed (I33) |
| `3` | `Call:<call>` | Inline edit; **typed letters appear uppercase**; Enter saves, `` ` `` cancels. **Was `4`** |
| `4` | `Grid:<grid>` | Inline edit; **uppercase**; accepts 4/6/8 char; a bad grid logs `Grid format: AA00/AA00aa/AA00aa00` and does not save. **Was `5`** |
| `5` | battery/sleep line | Enters Charge Mode. **Was `6`** |
| `6` | — | **Nothing.** Blank pad row (I33); keeps MENU P2/P3 from sliding. Must not cycle CQ or open FreeText |

#### MENU P2 (`N`)

| Key | Label reads | Press it and expect |
|---|---|---|
| `1` | `Offset:<src>` | Cycles Random / RX / Cursor, wrapping after 3 |
| `2` | `Fixed:<hz>` | Inline edit, **digits only** — letters must be rejected with no visible change. `▲``▼``◀``▶` step ±100/±10 and clamp to 200–3000. `` ` `` restores the value you started with |
| `3` | `Radio:<name>` | Cycles QMX / QDX. If audio was streaming and the backend differs, audio stops and log (`P` then `.`) shows `Audio stop <radio>` |
| `4` | `IgnoreList:<prefixes>` | Long edit; **upper case**; space-separated prefixes; stops accepting at 64 characters |
| `5` | `C:<comment>` | Long edit; **keeps lower case, unlike FreeText and IgnoreList**; `/Radio` and `/Grid` macros expand in the displayed line |
| `6` | `Mode: FT8` or `FT4` | Toggles FT8/FT4 and appends `*` when it differs from the running mode. Reboot applies it |

#### MENU P3 (`O`)

| Key | Label reads | Press it and expect |
|---|---|---|
| `1` | `RxTxLog:ON/OFF` | Toggles and persists |
| `2` | `SkipTX1:ON/OFF` | Toggles and persists |
| `3` | `Band config` | Enters the BAND config screen |
| `4` | copy-to-SD line | `Copied OK`, or `Missed [n]`, or a blocked message if TX/decode/streaming is active. **Moved from `5`** when the `GNSS_LoRa` row was removed |
| `5` | `Max Retry:<n>` | Inline edit, **digits only**; `0` is accepted. **Moved from `6`** |
| `6` | — | **Nothing.** Page 3 has five rows now; this key must do nothing at all. A derived layout that wrapped it round to another row would be the failure mode to catch |

#### Menu paging and edit-mode guards

| # | Do | Expect |
|---|---|---|
| 2.1 | `M`, then `.` twice | P1 → P2 → P3; `;` walks back; no page 4 |
| 2.2 | `M` on P1, press `M` | Leaves to `R`. On P2 or P3, `M` returns to P1 first |
| 2.3 | `N` from `R`, then `N` again | Enters P2, then leaves to `R` |
| 2.4 | `O` from `R`, then `O` again | Enters P3, then leaves to `R` |
| 2.5 | Start editing Call (`M` `4`), then press `R`, `T`, `S` | **Screen must not switch** — mode keys are disabled during edit |
| 2.6 | While editing Call, press `` ` `` | Cancels the edit; original call intact; mode keys work again |
| 2.7 | Edit Call, press Enter | Saves; reboot and confirm it persisted in `Station.txt` |
| 2.8 | Edit Max Retry (`O` `6`), type letters | Letters rejected, **screen does not flicker or redraw** |

### 3. RX (`R`)

| # | Do | Expect |
|---|---|---|
| 3.1 | `1`–`6` on a decoded line | Line flashes; a reply is queued; `T` shows the new QSO |
| 3.2 | Tap a line while a TX is in flight | The in-flight TX is **not** replaced; the tap queues behind it |
| 3.3 | Tap while a QSO with that station is already active | log (`P` then `.`) shows `QSO in progress`; no duplicate queued |
| 3.4 | `▲` / `▼` when line 1 or 6 is cyan | Pages the decode list |
| 3.5 | `` ` `` during a TX | TX stops; PTT drops; HUD clears |

### 4. TX queue (`T`)

| # | Do | Expect |
|---|---|---|
| 4.1 | `T` with ≥1 active QSO | Queue lists entries with state and parity colour |
| 4.2 | `1` | Rotates to the next same-parity entry |
| 4.3 | `2`–`6` | Drops that entry; **list redraws immediately** |
| 4.4 | `;` / `.` | Pages when more than 5 entries |
| 4.5 | `` ` `` | Cancels TX |

### 5. STATUS (`S`)

| # | Do | Expect |
|---|---|---|
| 5.1 | `1` | Cycles Beacon mode; applies on leaving STATUS |
| 5.2 | `2` | Starts audio and runs the CAT sync path |
| 5.3 | `3` | Steps to the next active band |
| 5.4 | `4` | Toggles Tune; radio keys and unkeys |
| 5.5 | `5` | Edits Date in place; digits only; Enter applies to the RTC |
| 5.6 | `6` | Edits Time in place. The Time line's source suffix is ` G` GPS, ` R` DS3231, ` P` phone, or blank for saved / ESP-RTC / manual |
| 5.7 | Enter `2026-02-30` and press Enter | log (`P` then `.`) shows `Invalid date/time`; **date line unchanged**; clock not set |
| 5.8 | Enter `2026-13-45`, then `2026-00-00` | Both rejected the same way. Before B34 all three of these were silently accepted and rolled over (to 2026-03-02, 2027-02-14 and 2025-11-30) |
| 5.9 | Enter `2024-02-29`, then `2025-02-29` | Leap day accepted in 2024, rejected in 2025 |
| 5.10 | While editing the date, hold `/` to the end then `,` back | Cursor steps over the `-` separators in both directions and stops at each end without sticking |
| 5.11 | Enter `24:00:00` as the time | Rejected; time line unchanged |
| 5.12 | Enter a **valid** date and time and press Enter | Accepted and applied. B34 changed this path, so confirm a good setting still reaches the clock — not only that bad ones are refused |
| 5.13 | Power-cycle after 5.12, with a DS3231 fitted | Time survives; suffix reads ` R` |

### 6. BAND (`B`), QSO (`Q`), Delete (`D`), GPS (`G`), PERF (`P`), BT (`H`)

| # | Do | Expect |
|---|---|---|
| 6.1 | `B`, `1`–`6` | Selects a band slot; edit kHz; persists |
| 6.2 | `Q`, `1`–`6` | Opens that ADIF file; `◀` `▶` switch Default / SNR columns |
| 6.3 | `D` | Lists files. `1`–`6` deletes **immediately, no confirmation** |
| 6.4 | `D` on today's active log | Row shows `LOCK <file>`; the file is **not** deleted |
| 6.5 | `G` | Live GPS telemetry: source, fix, satellites, UTC, grid |
| 6.6 | `P` | Performance stats. Press `P` again for the debug log, `P` again returns to `R` |
| 6.7 | `H` | Shows `1: Start sync`, and `2: Flash Sidekick` when a Nano is present. `1` runs the phone time sync (radio unplugged from USB-C first — see 0.3); `2` field-flashes the sidekick |

### 7. End-to-end

| # | Do | Expect |
|---|---|---|
| 7.1 | Complete one full QSO | Sequence runs to signoff without manual help |
| 7.2 | `Q` | The QSO is in today's `.adi` and readable |
| 7.5 | Work a station that never sends a grid, then read the `.adi` | The record has **no** `gridsquare` field at all — not `<gridsquare:0>`. Two such QSOs appeared in the 2026-09-08 log (N2FSM, W4MAA) |
| 7.3 | `O` `5` | Copy to SD reports `Copied OK`; files land on the card |
| 7.4 | Reboot | Call, grid, band, and every menu toggle survived |

### 8. Decode-yield measurement protocol

Any change claiming to improve decoding is measured this way, not by impression. Cited by B53, B54, and
every I26 experiment.

- **Metric is unique decodes per slot.** Not total rows, not "it felt better".
- **A/B at the same time of day, never the same evening.** Band conditions move faster than any change we
  would make.
- **Build the comparison version in a separate worktree**, so the two arms are not one tree at two times.
- **Full chip erase before each flash.** NVS carryover has changed behaviour between arms before.
- **60 slots (15 minutes) per arm.** Report **mean and standard deviation**, not the mean alone.
- **Change one thing at a time.**

σ is not decoration. Worked example from an independent project on this decoder: two arms at 15.38
decodes/slot each, σ 5.70 against 6.65 — a suspected regression that was band fading. The statistics that
stop you reporting a false regression are the same ones that stop you reporting a false improvement.

## Sidekick (AtomS3 Lite)

A second device with its own build and its own USB-C. `sidekick/` is a separate `idf.py` project — ESP-IDF
locks target and sdkconfig per project, so it cannot share the ADV's configure even though both are
`esp32s3` now.

### S0. Back up the part before flashing it, once per physical device

Irreversible otherwise: a factory AtomS3 Lite ships with M5Stack firmware, and nothing else restores it.

```bash
esptool.py --chip esp32s3 -b 460800 \
    read_flash 0x0 0x800000 sidekick/stock_backup/atoms3_stock_backup.bin
```

`0x800000`, not the Nano's `0x400000` — a 4 MB read of an 8 MB part restores garbage over the upper half.
`stock_backup/` is gitignored on purpose: a snapshot of one specific unit, not a repo asset.

### S1. Build and flash

Commands below carry no `-p` port: `idf.py` and `esptool.py` both autodetect,
and a placeholder like `/dev/cu.usbmodemXXXX` turns every line into one you have
to edit before it runs. Add `-p` only to disambiguate two boards plugged in at
once.

```bash
cd sidekick && idf.py build && idf.py flash monitor
```

| # | Expect |
|---|---|
| S1.1 | Builds for `esp32s3` with no `set-target` step |
| S1.2 | Boot banner `Micro-FT8 sidekick booting (IDF …)` over the AtomS3's own USB-C |
| S1.3 | `alive: N` every ~5 s, counting up |

### S2. Grove pin mapping — the one that fails silently

`main.c` uses `GPIO_NUM_1` / `GPIO_NUM_2` for the PORTA TX/RX pair, carried over from the NanoC6. The
AtomS3 Lite's silkscreen was checked 2026-09-08 and matches (`G`, `5V`, `G2`, `G1`), and M5Stack labels Grove
pins with GPIO numbers, so the constants are *expected* to be right — but expected is not verified, and a
wrong mapping fails silently: the beacon transmits into nowhere and it looks exactly like "the companion
link isn't implemented yet".

Connect the sidekick's Grove port to the ADV's PORTA with a straight-through cable. Nothing else on PORTA:
role arbitration locks to GPS *or* companion, so an attached GPS wins and masks the result.

| # | Do | Expect |
|---|---|---|
| S2.1 | Sidekick powered and beaconing, then open the log on the ADV — **`P` then `.`** | Within about ten seconds, **one** of: `OK <version>`, or the pair `X R:<remote>` / `X L:<local>`. The beacon is every 5 s now, not every 1 s — it shares the wire with the host event stream, where 1 Hz was noise |
| S2.2 | Read which one appeared | **Either proves the pins are right.** `OK` means the two builds are the same commit; `X R:`/`X L:` means the link works and the builds differ, which is expected whenever the ADV is running an older image than the sidekick you just flashed |
| S2.3 | If **no** line appears at all | The Grove pins are wrong, or the cable is. Check the AtomS3 Lite's Grove GPIO numbers against `PORTA_TX_PIN`/`PORTA_RX_PIN` before suspecting anything else |
| S2.4 | Confirm the ADV did not instead log `PORTA: GPS` | That means it locked onto the GPS role — something else is on the bus, or the beacon is being misread |

### S2c. WiFi provisioning (SoftAP)

New in I3d. The sidekick raises its own access point when it has no working
credentials, serves a one-page form, stores what you enter, and restarts to join.

| # | Do | Expect |
|---|---|---|
| S2c.1 | Flash and watch the monitor on a device with no stored credentials | `No stored credentials`, then `Provisioning AP '<name>' up — open http://192.168.4.1/` |
| S2c.1a | **iPhone hotspot?** Settings → Personal Hotspot → **Maximize Compatibility ON** | Without it recent iPhones run the hotspot on 5 GHz, which this radio cannot see or join — it will be absent from the scan and fail manual entry |
| S2c.2 | On a phone, look for an open network `micro-ft8.local` (was `MicroFT8-SK-XXXX` — renamed 2026-09-20 to hint at the URL to visit) | Appears |
| S2c.3 | Join it, then navigate to `http://micro-ft8.local/` or `http://192.168.4.1/` yourself | **B74, 2026-09-21: no auto-popping OS sign-in sheet any more** — the DHCP option 114 / DNS-hijack machinery that tried to trigger one never reliably resolved to "Done" on iOS and was removed along with the rest of the captive-portal logic; standard DHCP is back (router + DNS both offered), which was also the actual fix for the cellular-retention question B55/B69 were fighting over DHCP options. Manually navigating lands on a form with a **Network dropdown listing nearby WiFi**, a "or type one not listed" box, and Password |
| S2c.3a | Check the dropdown | Your network is there. Duplicates collapsed (same SSID on 2.4/5 GHz or a mesh shows once). Open networks are marked `(open)` |
| S2c.3b | Pick from the dropdown **and** leave the manual box empty | Joins the selected network |
| S2c.3c | Look at the form before touching the dropdown | **No free-text box.** It used to sit there permanently, asking a second question the operator has only one answer to |
| S2c.3c1 | Pick **Other…**, the last entry in the list | A `Network name` box appears and takes focus. Type a name and it joins that network — the path for a hidden SSID, or one out of range right now |
| S2c.3c2 | Pick **Other…**, leave the box empty, submit | A `Not saved` page: "Network required — pick one from the list, or choose Other… and type a name", with a working `Back to the form` link. Check the dash and ellipsis render, and that no literal `<b>` tags show — error pages do not go through the normal response path and were mangled once already. No join attempt: the sentinel must never reach the driver as an SSID |
| S2c.3c3 | Pick **Other…**, then switch back to a listed network | The box disappears and the listed network is what gets joined |
| S2c.3d | Turn a hotspot **on after** the sidekick booted, disconnect from the AP, wait ~15 s, reconnect | It appears without pressing anything. The cache refreshes every 10 s while no client is associated — field-hit on the first run, where the list predated the hotspot |
| S2c.3e | Click "Scan again" | The list refreshes. Your phone may briefly drop; the scan takes the radio off the AP's channel. Warned in the page text |
| S2c.3f | Read the intro line: "**2.4 GHz only** — this radio cannot see…" | A real em dash, not `â€”`. The served pages declare `charset=utf-8`; without it the browser guesses Latin-1. Our punctuation is the visible symptom, but the case that matters is an SSID with non-ASCII characters rendering into the dropdown |
| S2c.3g | In a private window (never paired), look at **Save and join**, then press the sidekick button | Save is greyed out, with `Press the sidekick button to pair, then save.` under it. Within ~1–2 s of the press the line reads `Paired` and Save enables. Saving credentials is a write, so it needs the token even on the AP |
| S2c.3h | With the sidekick in AP mode (`alive:` reads `wifi=ap`) and the laptop joined to its network, unpaired: `curl -i --connect-timeout 5 -X POST http://192.168.4.1/provision -d 'ssid=x&pass=y'` | `401 Unauthorized` and `{"error":"pairing required"}` at once; the monitor logs `POST /provision` then `Refused /provision (no valid token)`. **No restart, and the AP stays up** — nothing was stored. A connect timeout instead means the laptop is not on the sidekick's AP: 192.168.4.1 exists only there, and in station mode `/provision` is not registered at all |
| S2c.4 | Submit your WiFi details | A "Saved" page warning the AP will disappear, then the device restarts |
| S2c.5 | Watch the monitor | `Joining '<ssid>'...` then `Online as <ip>`. The `alive:` line now reads `wifi=up` |
| S2c.6 | Power-cycle | Rejoins from NVS without the AP appearing — credentials persisted |
| S2c.6a | `esptool.py --chip esp32s3 erase_region 0x9000 0x6000` (esptool, not `idf.py`; close the monitor first), then reset | Comes up in the **provisioning AP**, not reconnected. Before I3g the WiFi driver kept its own credential copy in `nvs.net80211` and the device rejoined anyway |
| S2c.7 | Throughout all of the above, check the beacon on the ADV (`P` then `.`) | Still arriving. WiFi must not disturb the companion link |
| S2c.8 | Enter a **wrong** password at S2c.4 | Five attempts logged **with a reason** (`wrong password`), then `falling back to the AP`. The AP must come back **without crashing** — that path panicked before I3f — and the page shows "Last attempt failed: wrong password" |
| S2c.8a | Enter a network that is out of range or 5 GHz | Same fallback, reason reads `network not found (5 GHz? this radio is 2.4 GHz only)` |
| S2c.8b | After the fallback AP is up, leave it a minute | No repeating join attempts in the log. The AP runs in APSTA to scan, and its station side must stay idle rather than re-attacking the failed network |
| S2c.9 | With a password containing a space or `%` | Round-trips correctly — the form is URL-encoded and the field is decoded before storage |

### S2d. Station mode: mDNS and the app's Network settings

Until now a joined sidekick served nothing. The HTTP server ran only while the provisioning AP was up, so
once the device was on your network the only way to see what it was doing was a serial console — precisely
the console the finished product will not have attached. mDNS and the station-mode server are one feature
for that reason: the name would otherwise resolve to a port with nothing behind it.

**Preconditions:** joined (S2c.5 passed), and the laptop or phone you browse from on the **same subnet**.
mDNS does not cross subnets, and many guest networks isolate clients from each other. If `.local` fails,
rule that out before suspecting the firmware — S2d.5 is the control.

| # | Do | Expect |
|---|---|---|
| S2d.1 | Watch the monitor at the moment of join | `mDNS: http://micro-ft8.local/` after `Online as <ip>` |
| S2d.2 | From a laptop on the same network: `ping -c 3 micro-ft8.local` | Resolves to the IP reported in S2c.5 |
| S2d.3 | Browse `http://micro-ft8.local/` | **B74:** `status.html` is retired — this now 303s to plain `/app.html`, landing on Operate (the default view), same as any other visit to the app. **Amended 2026-09-21, operator call:** originally landed on Settings > Network directly; changed to Operate since that's the more useful default for a bare `/` visit — `/app.html#network` still deep-links straight to Network for anything that wants that |
| S2d.3a | From Operate, tap the gear icon → Settings > Network | Mode = Station (joined), Network = your SSID, Address = that IP, Firmware = the running version, Uptime readable (not raw seconds) |
| S2d.4 | Leave Settings > Network open for ~30 s | Uptime advances on its own — it rides the same 5 s `pollLiveStatus()` tick as the rest of Settings (`sk_uptime` key), not a per-request-generated static page any more |
| S2d.5 | Browse `http://<ip>/` directly | Same redirect, same landing. This separates "the server is down" from "the name did not resolve" — do it before blaming mDNS |
| S2d.6 | `dns-sd -B _http._tcp` on macOS, or any network browser | `micro-ft8` appears as an HTTP service. Hostname resolution alone would not list it; this is the `_http._tcp` advertisement |
| S2d.7 | **Before** joining, while on the provisioning AP, browse `http://micro-ft8.local/` | The provisioning **form** (`provision.html`), not the redirect. mDNS is advertised in both modes |
| S2d.7a | Immediately after the join, if `micro-ft8.local` hangs from the phone or laptop you used on the AP | **Expected, and not a firmware fault.** The same hostname pointed at `192.168.4.1` while the AP was up, and mDNS records carry a 120 s TTL — a client that resolved it on the AP keeps that answer until it expires. Try the station IP directly to confirm, then wait ~2 min, toggle WiFi, or on macOS `sudo killall -HUP mDNSResponder` |
| S2d.8 | Unpaired: open `/` (lands on Operate per S2d.3, then gear icon → Settings > Network), press **Forget WiFi** without pairing; then press the sidekick button and retry | **B74:** button was `status.html`'s, now Network's `netForget`. First attempt asks you to pair (`netMsg`: "Press the sidekick button to pair first.", no `prompt()`) and the button stays disabled (`enableWrites`) until paired. After the button, `/pairing.js` claims the token, the button enables, and Forget succeeds: `netMsg` shows "Forgotten…", monitor `Credentials forgotten — restarting into provisioning`, AP back within ~10 s |
| S2d.9 | Power-cycle after the forget | Still provisioning. Nothing rejoined — this is the first operator-reachable control that depends on I3g's single credential store, and the erase test in S2c.6a is the same guarantee reached a harder way |
| S2d.10 | Repeat S2d.8 but **cancel** the dialog | Nothing happens; still online. The confirm is the only thing standing between a stray tap and a re-provision |
| S2d.11 | Check the ADV beacon (`P` then `.`) at the end | Still arriving |

Not testable with one device, so it is a known limitation rather than a row: the hostname is fixed, so a
second sidekick on the same network loses the name and mDNS silently renames it `micro-ft8-2.local`. Tracked as ROADMAP f36a.

### S2e. Pairing token: an unpaired browser can watch and cannot transmit

This is I28b's Done-when, and [architecture/system.md](architecture/system.md) (**Who may key the transmitter**) is now the whole security design — image signing was rejected ([architecture/system.md](architecture/system.md), **Why not signing**) on the
grounds that the browser's own TLS already authenticates the source, leaving authorization on the LAN as the
only real gap. So what these rows prove is a *policy*, not a feature: reads are open because anyone may
listen to amateur transmissions on licensed spectrum, and writes need the token because the licensee answers
for them.

The half that can be host-tested already is (`host_test_pairing_token`). What cannot is everything touching
NVS, the HTTP server and the button, which is all of the below.

**Preconditions:** joined and reachable (S2d.3 passed), and a browser whose site data for the sidekick you
are willing to clear. Use a private window for the "unpaired" rows — clearing `localStorage` is the only way
back to unpaired once you have pasted a token.

| # | Do | Expect |
|---|---|---|
| S2e.1 | First boot after flashing, watch the monitor | `Pairing token minted; press the button to read it`. On every later boot, `Pairing token loaded` — the token is in NVS and survives a reboot. **The token itself must never appear in the log**: the sidekick's USB-C carries the ADV's log stream, so anything printed there is visible to whoever is watching it |
| S2e.2 | Unpaired browser on `/app.html` (**B74:** `/log`'s own copy of this check is gone with the page — pairing badge reads "Unpaired", tap it to reveal "Press the sidekick button to pair"). Press the **user button** (side, not reset) | Within ~1–2 s the badge flips to "Paired" (green) and the hint auto-hides; Connect/Tune/Save/Cancel TX/Network actions all enable together (`enableWrites`). Monitor: `Disclosure window open` then `Disclosure window closed (token retrieved)`. Up to 1 s of lag before open is expected — the loop ticks once a second. Manual `GET /api/pairing-token` still works if no page is polling, but is no longer the operator path |
| S2e.3 | After a successful claim (S2e.2), or after waiting out 120 s with **no** page claiming: `GET /api/pairing-token` | **404**, and `Disclosure window closed (token retrieved)` or `(timed out)` on the monitor. One-shot: a second browser cannot read the token after the first claim. 404 rather than 403 on purpose |
| S2e.4 | Browse the same URL **before** any press, from a fresh boot | 404. A press is the only thing that opens it |
| S2e.5 | Tap the button several times quickly | One window, not several. The ISR debounces at 250 ms, and re-arming extends rather than stacks |
| S2e.6 | **Unpaired** (private window): browse `/app.html`, Operate tab | **B74:** `/log`'s own version of this check is retired with the page. Decodes stream into Operate's decode list normally with no token. This is the principle — watching needs no credential |
| S2e.7 | Unpaired, tap the pairing badge | Reveals `Press the sidekick button to pair`. Events still stream. Clock sync (`autoSyncClock`) waits until paired — it is a write |
| S2e.8 | Unpaired: `curl -i -X POST http://micro-ft8.local/api/time -d 1` | `401 Unauthorized` and `{"error":"pairing required"}`. The monitor logs `Refused /api/time (no valid token)` |
| S2e.9 | Unpaired: `curl -i -X POST http://micro-ft8.local/forget` | 401, and **still online afterwards** — the write did not happen, it was not merely unreported |
| S2e.10 | With a token that is right except for its last character | 401. The comparison is constant-time and full-length; a near miss must not pass |
| S2e.11 | With a well-formed token in the header: `curl -i -X POST http://micro-ft8.local/api/time -H 'X-MicroFT8-Token: <token>' -d $(date +%s)000` | `{"ok":true}`, and the viewer's timestamps become correct |
| S2e.12 | Complete S2e.2 on `/log` (button only; no paste) | Clock syncs on its own after `Paired`; timestamps become correct UTC. Reload keeps the token in `localStorage` (survives reload, not site-data clear). A second press is needed only after a clear or a 401 from a rotated/wrong token |
| S2e.13 | `curl -i http://micro-ft8.local/api/events` and `http://micro-ft8.local/` with **no** token | Both 200. Reads stay open, and a guarded-by-default mistake would show up here as a 401 |
| S2e.14 | Reboot the sidekick and reuse the same token | Still accepted. Minting happens once, not per boot — the opposite would silently unpair every device on every power cycle |
| S2e.15 | Check the ADV beacon (`P` then `.`) at the end | Still arriving. The button shares no pins with Grove (GPIO41 against G1/G2) |
| S2e.16 | Unpaired: `curl -i -X POST http://micro-ft8.local/api/tx -d 'CQ TEST'` | `404` (route removed, I33) or connection error — not a queued TX. No `Queued:` on the ADV |
| S2e.17 | Paired: with a TX active or armed, Operate's TX HUD panel shows **Cancel TX** (**B74:** moved here from `/log`, which is retired — no free-text box ever existed on this button); `POST /api/tx/cancel` with token still works. `POST /api/tx` is gone | Cancel aborts in-flight / armed TX; no free-text queue path |

Known limit, from [architecture/system.md](architecture/system.md) (**Who may key the transmitter**) and not a defect: the token crosses the LAN in clear over plain HTTP, so
anyone who can read the traffic can replay it. WPA2/WPA3 per-station encryption makes that materially
harder and an open network does not. This is scoped as a nuisance control, not a targeted-attacker one.

## Pending

Field checks owed on shipped work. Add a row the moment a change touches a field-only path, naming the change and the exact check. **Drop a row once it passes** — git is the record.

| From | Check | Status |
| --- | --- | --- |
| B89 (decoder built at `-O2`) | Take a busy-band log **before** reflashing, then reflash and take one **after**: FT8 on 20 m at a busy hour, ~10 slots each, read B53's per-slot line on `P` then `.`. Expect `SLOT … dec=…ms` to drop by roughly a third to a half on the ADV (bench: 1,122 → 718 ms on busy slots) with `msg=` counts unchanged within band-to-band variation. Same on the P4 if it has a radio (bench: 585 → 315 ms). A `dec=` that does not move means the flag did not take; a `msg=` that collapses means something other than speed changed. | **owed**: timing and identical output are proven on the bench (`test_apps/decode_bench`, RFC 0006 §5 P0), but not inside the firmware with USB audio running. |
| Android: `micro-ft8.local` over mDNS (day-one phone goal, never checked) | On a real Android phone (record model and Android version): (1) join the sidekick's own AP with **mobile data left on**, accept any "no internet" prompt with *stay connected*, and browse `http://micro-ft8.local/` in Chrome — the app loads. If it does not, try `http://192.168.4.1/` (loads = server fine, name did not resolve), then retry `.local` with mobile data off (loads = Android routed the lookup over cellular). Note Private DNS setting. (2) Repeat with phone and sidekick both joined to a home router. Expected pass on Android 12+; record which step failed if not, since the public page tells operators to use the name. | pending |
| B76 (fall back to AP mode on mid-session WiFi loss) | Reflash. Join a real network, confirm station mode is up (Settings > Network shows "Station (joined)"). Then: (1) **permanent loss** — power off the router (or take the sidekick out of range) and watch the monitor: expect `Join failed (...), retry N/20` climbing, then `Lost '<ssid>' and could not rejoin after 20 tries ... -- falling back to the AP`, then the sidekick's own AP should come up within roughly the time 20 retries take (not measured — this is what the check establishes); confirm you can join that AP and reach `app.html`, and confirm the original credentials are still there (not erased) by checking Settings > Network still shows the right SSID once the real network is available and you rejoin or reboot; (2) **transient loss (the case this row is calibrated for, not just the permanent one)** — reboot the router instead of leaving it off, and confirm the sidekick reconnects to the *original* network on its own within the retry window rather than needlessly falling back to AP for an outage that was always going to clear; (3) confirm the ADV's own Port A beacon survives the fallback restart (`P` then `.` on the ADV) — a brief gap during the restart is expected, a permanently dead beacon is not. | **owed** — entirely field-only, no host-test substitute (this is real WiFi-radio and real-router timing); also specifically flagged as unverified in the ROADMAP row: calling `esp_restart()` from inside the WiFi event-callback context (rather than an HTTP handler task, the only place this codebase has called it from before) has not been checked for reentrancy issues on real hardware — watch for a crash or hang at the exact moment of fallback, not just whether the AP eventually comes up |

### Unverified, no check planned

Shipped code that has never run on hardware, with no check scheduled — listed so it is not mistaken for proven, and not counted as owed. Move one back into the table when a check is planned.

- **B27 — decode auto-sync persisting to a DS3231** (ADV only). Needs a generic DS3231 breakout (I2C 0x68; not M5Stack's Grove RTC Unit, a HYM8563): offset the clock, let auto-sync fire (`SYNC: Applied RTC sync…`), power-cycle without re-syncing, and the Time line reads ` R` with the corrected time.
- **TX HUD abort reason, sourced from firmware** (`PORTA_EVT_TX_HUD`'s `reason`). Needs a live low-battery TX abort to show the real reason on Operate. Only one abort reason exists in `main.cpp` today, so even a pass cannot show two reasons are distinguished.
- **Grove GPS dropped: the cap's GNSS and the sidekick together.** They were mutually exclusive before, so both at once has never run: `G` reads `Src:cap GNSS` with no setting, the sidekick's HELLO still arrives (`P` then `.`) with the cap fitted, MENU P3 renumbering holds, and a `Station.txt` still carrying `gnss_lora=` loads.

### Standing watch — B36, unproven

One item did **not** clear, and cannot be cleared by a passing run. B36 made a failed USB host install
retryable instead of terminal for the boot, but the race is not reproducible on the bench, so a clean start
proves only that the retry path was never needed. It stays unproven until a failure recovers.

**If `S`→`2` ever fails, press it again before power-cycling.** It should be able to succeed. The on-screen
log (`P` then `.`) now names the reason — `USB host retry after …`, `USB host timeout (…)`, `USB host install: …`,
`UAC already started` — instead of only `Audio start fail`. That line is the evidence; capture it.
