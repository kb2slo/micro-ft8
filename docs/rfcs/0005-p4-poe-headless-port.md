# RFC 0005: ESP32-P4 Port (M5Stack Unit PoE-P4, U213)

* **Status:** Draft. Opened 2026-09-16 after the operator accepted [B26](../ROADMAP.md)'s feasibility
  question as answered by external evidence (SteffenLav's `qmx-panadapter`, shipping USB-host-QMX on an
  ESP32-P4-family board in production) rather than running B26's own bench test — see §3 for exactly what
  that does and does not establish. Hardware (M5Stack Unit PoE-P4, SKU U213) is purchased and in hand.
  **Amended same day** on operator feedback: PoE is not a target power path (§4, §7 Phase 4) — Plan A is 5V
  over USB-C into the Download/OTG (diagnostic) port; the sidekick binary must not fork per main board (§6,
  §7 Phase 3); both ADV and P4 targets build from every build going forward, not one-or-the-other (§7
  Phase 0); code sharing between the two builds is an explicit goal (§5), not an incidental outcome; and
  `qmx-panadapter` is to be read and cited as an implementation reference, not just treated as closed
  feasibility evidence (§3, §7 Phase 0).
  **Amended 2026-09-17:** while shrinking `main.cpp` during Phase 2, found that the porta/sidekick pump
  (`porta_tick()`, `porta_file_read_tick()`, `porta_queue_state_tick()`, `porta_tx_hud_tick()`, plus the
  `porta_start()` call in `apply_radio_profile_binding()`) is currently called only from `gps_runtime_tick()`/
  `app_task_core0()`, both of which are otherwise entirely `M5Cardputer`-coupled. §5 already names the
  sidekick protocol client as a shared-code candidate; this pump is where that candidate actually lives in
  the source tree today, and it has no path to a headless board as currently wired — see §7 Phase 3.
  **Amended 2026-09-17:** operator direction — Phase 2 is the forcing function to port `decode_monitor_results`
  and its dependencies for real (not a parallel minimal decoder), adjusting the porting plan as needed rather
  than working around gaps. This surfaced three ADV hardware dependencies with no P4 equivalent: the SD card
  (`storage_service.h`), the DS3231 external RTC chip (`external_rtc.h`), and battery monitoring
  (`board_power.h`). Design: P4 is not a second implementation of ADV's storage/RTC-chip/battery hardware —
  it simply doesn't have SD, DS3231, or a pack gauge, permanently. **Amended again 2026-09-18 (I29):**
  "no storage" was overstated — ADV keeps `Station.txt` / `.adi` on **internal** flash, not on SD.
  Internal store migrates globally from FAT+TinyUSB-MSC to **LittleFS** (same crash-safer FS the sidekick
  web root already uses); P4 gets that LittleFS partition and Station persist. SD (ADV only) stays FAT.
  **Field-proven 2026-09-18 (I29 closed):** ADV Station + `.adi` across reboot; P4 LittleFS mount + Station
  Save survives reboot (after custom `poe_p4/partitions.csv` flash).
  RTC chip and battery reads still report "not present" on P4. The RTC
  *soft clock* itself (`rtc_now_ms()` and friends) has no board coupling — it's arithmetic plus the ESP's own
  built-in RTC peripheral (`settimeofday`/`gettimeofday`, present on every ESP32) — so it's shared code, ported
  as-is. P4 has no GPS wired yet, so it degrades to monotonic time the same way ADV already does with no GPS
  fix: a known, already-scoped gap (NTP over Ethernet is the natural fix, given no WiFi/GPS on this board),
  not something this pass solves. `decode_tx_state`, `callsign_hashtable`, `autoseq`, and `ft8_audio_pipeline`
  move from `main/` to `components/` per STYLE's module-placement rule, now that P4 genuinely build-forces
  them; `decode_monitor_results` and its helpers become a new shared component for the same reason.
  **Amended 2026-09-17 (same day):** this design built and link-verified clean on both targets (`idf.py
  build` for ADV and P4, `host_mock` 24/24, `tx_e2e` 7/7) — see `ROADMAP.md` B63. P4 gained real UAC RX audio
  capture (`qmx_transport.cpp`, a second USB host client alongside the existing CDC-ACM one, same FIFO
  tuning `stream_uac.cpp` already proved for this exact composite-device case) wired into
  `ft8_audio_pipeline_run()`. Not yet bench-confirmed against a live signal — see `TEST_PLAN.md`'s Pending
  table. TX (the tone-scheduling state machine) is unchanged, still `main.cpp`-only.
  **Sequencing amended 2026-09-18:** Phase 3 companion/porta (ROADMAP B67) is Now before P4 TX
  (B68), so station config and beacon arming use the real sidekick path rather than throwaway
  hardcodes.
* **Author / Lead:** Jeff Kalikstein, KB2SLO
* **Covers:** porting Micro-FT8's radio-control/decode/TX core to run on this specific board — USB host
  bring-up for the QMX, the headless (no display, no keyboard) operating model, and the board-specific facts
  that change B26/I27's original assumptions now that a real datasheet exists.
* **Does not cover:** the sidekick-to-browser protocol or web app ([RFC 0004](0004-headless-web-architecture.md),
  unchanged — the AtomS3 Lite companion carries over in the same role per [I27](../ROADMAP.md)), or decode
  tuning ([I26](../ROADMAP.md), a separate axis: what the decoder does with samples it already has, not
  whether this chip can capture them).
* **Ask:** confirm the phased scope in §7 before writing any P4-target code. §4's two-USB-port and no-WiFi
  findings are read from the vendor datasheet, not bench-confirmed on our own unit — Phase 0 exists to check
  them before anything else is built on top.

---

## 1. Why this RFC, and why now

[I27](../ROADMAP.md) ("Two-MCU headless Micro-FT8: P4 main + AtomS3 Lite companion") was stated publicly in
[Wei's Micro-FT8 Discussions](https://github.com/wcheng95/Micro-FT8/discussions/45) on 2026-09-08 and marked
in `ROADMAP.md` as a large product shift needing its own RFC before code, gated on
[B26](../ROADMAP.md) ("Stage 0: ESP32-P4 USB-host-QMX feasibility test"). B26 was never run on our own
hardware: the operator now has the board and has decided, on the strength of an unaffiliated project
already shipping the equivalent capability in production, not to spend a bench session re-proving it. That
decision closes B26 as answered by external evidence (recorded here and in the commit that drops the row)
and opens the gate on I27 — which is what this RFC exists to scope before any porting code is written, per
the Ideas-rule requirement for large product shifts.

## 2. What B26 was actually testing, restated

B26's own text is worth restating so this RFC doesn't quietly relax it: P4 "gets no free pass" from the
ADV's own proven USB-host silicon (RFC 0001 §4.6) because it is a different chip family, and the AtomS3 Lite
precedent (same RFC, same section) is exactly a case where a same-vendor, same-general-purpose board turned
out *not* to host a QMX despite looking like it should on paper. Nothing about accepting external evidence
here should be read as "P4-family chips always host QMX-class radios" — it's "this specific external project,
on its specific board, in production, does." §3 draws that line precisely.

## 3. What `qmx-panadapter` proves, and what it doesn't

`SteffenLav/qmx-panadapter` runs on the **M5Stack Tab5 (ESP32-P4)** — a different board from ours — as a USB
host for a QMX: UAC audio in, CAT control, on-device FT8/FT4 decode and TX, a browser-streamed panadapter UI,
ADIF logging with third-party upload, and live spotting. That is real, shipping evidence that:

- The ESP32-P4 silicon can USB-host a QMX-class radio (UAC + CDC-ACM) while decoding and transmitting FT8/FT4
  on-chip, with headroom to spare for a panadapter UI on top.
- This is not a one-off lab result; it is a maintained project with a version history, i.e. it survives past
  a first bring-up.

What it does **not** prove, because it is a different board:

- That the **M5Stack Unit PoE-P4 (U213)** wires its USB-C ports, VBUS sourcing, or power rails the same way
  the Tab5 does. RFC 0001 §4.6 is the cautionary precedent here — "AtomS3 Lite can't host" was a board-level
  finding about a specific SKU, not about ESP32-S3 in general, and the ADV's own Stamp-S3A was unaffected by
  it. The same board-specificity applies going the other direction: Tab5 hosting successfully doesn't
  guarantee U213 does, even though both carry the same SoC.
- Anything about running on PoE-only power (Tab5 is a battery/USB-powered tablet-style board; U213 has no
  battery and no WiFi — see §4).
- Anything about our own firmware's specific USB host stack usage (`usb_host_uac` / `usb_host_cdc_acm`,
  already vendored in this tree and already confirmed via `idf_component.yml` to list `esp32p4` in their
  `targets:` array — a manifest fact, not a bench fact).

So: feasibility-in-principle is accepted without our own bench proof, per the operator's decision. Board-level
integration facts about the U213 specifically are not yet established and are Phase 0 of §7, not assumed.

**Beyond evidence: treat it as a reference to study, not just a fact to cite.** `qmx-panadapter` has already
solved, on a real ESP32-P4 board, nearly everything Phase 0 through Phase 2 need to build fresh — USB host
bring-up on this SoC, hosting a QMX specifically (UAC audio + CAT), and running FT8/FT4 decode/TX alongside
it. That is worth reading closely before and during the port, not filed away as a closed feasibility
question. Concretely: its USB host init sequence, its QMX UAC/CAT handling, and any P4-specific quirks it
worked around are exactly the kind of thing that would otherwise cost us a bench session to rediscover.
**Same discipline as I12/I13 and the README's own note on this project ("Insight, not code — do not port or
copy from it without the operator's own read of the license and the code"): review it, understand it, and if
a pattern or fix is worth adopting, name the source file/commit explicitly rather than silently converging on
the same solution — no unattributed lift, license permitting.** This is a standing action for whoever works
Phase 0 onward, not a one-time desk check; add it as an explicit Phase 0 task in §7.

## 4. What the U213 datasheet says that changes I27's original framing

I27's text (written before this hardware was in hand) flagged two open questions this section can now
partly answer from the vendor spec (`docs.m5stack.com/en/unit/Unit_PoE-P4`, checked directly, same practice
RFC 0001 §4.6 used before trusting a vendor description) — **not yet bench-confirmed on our unit**:

**Two independent USB-C ports, not one shared port.** The datasheet lists a dedicated *"USB Type-C Host
(USB 2.0)"* port and a separate *"USB Type-C Download/OTG (USB 2.0)"* port. Cross-checked against
Espressif's own ESP32-P4 USB host docs: the chip has two independent USB-OTG peripherals (one High-Speed,
one Full-Speed), both host-capable — which is the "two peripherals, IDF allows one host at a time" limitation
B26 already named, now mapped onto two separate physical connectors rather than one connector time-shared
between roles. **If this holds on the bench**, it removes the constraint that forced B26's own test design
(and RFC 0001 §4.6's AtomS3 test, and the ADV's own G4/G5 note in `docs/README.md`) to choose between USB
host mode and a live USB-serial console — QMX hosting and log output could run simultaneously over the
board's own two ports, with no external CP2102/CH340 UART-TTL adapter needed. This is exactly the kind of
claim this project's own discipline says not to take from a datasheet alone: Phase 0 (§7) bench-confirms it
before anything downstream assumes it.

**No WiFi radio.** ESP32-P4 has no integrated 2.4 GHz radio; this board pairs it with an IP101GRI Ethernet
PHY and 802.3at PoE instead. I27 already anticipated this correctly — the AtomS3 Lite companion was always
the WiFi-and-browser half (RFC 0001 §5.1), unchanged here — but it's worth stating plainly now that it's a
confirmed hardware fact rather than an assumption: **this board cannot serve the phone UI on its own under
any circumstances**, on a summit or otherwise.

**PoE is not a target power path — operator correction 2026-09-16.** The board's PoE capability is
board-level trivia, not a feature this project intends to use. **Plan A is 5V over USB-C into the
Download/OTG (diagnostic) port** — the same port §4's two-port finding already earmarks for the live log
console, so that port does double duty as power-in and console, exactly the way the ADV's own single USB-C
already does both when not hosting. The dedicated Host port stays exclusively QMX's: a USB host normally
sources VBUS to the device it hosts, so the QMX draws its 5V from that port the same way it does from the
ADV today. See §7 Phase 4 for what this implies for the field power budget.

**Other confirmed facts worth carrying forward:** 16 MB flash, 32 MB octal PSRAM (a PSRAM-class host, which
is exactly what I26 named as the thing that would lift the `freq_osr`=2 ceiling ESP32-S3 can't reach — real
but out of scope for this RFC; a decode-tuning question, not a porting one), a Grove connector (`G53`/`G54`)
for the AtomS3 Lite link, no battery input of its own (5V in over USB-C, per Plan A above — no PoE injector
assumed), and no display/touch/keyboard silicon on the module at all (headless is the only mode this board
supports, not a design choice layered on top of a UI-capable board).

## 5. Goal: share code between ADV and PoE-P4, don't fork it

**Operator-set 2026-09-16, and worth stating as a goal rather than leaving it to fall out of Phase 0
incidentally.** As much of the port as possible should live in source shared by both builds; ADV-only and
PoE-P4-only files are fine where a board genuinely needs something the other doesn't (a UI driver on ADV, a
headless entry point on PoE-P4), but **the same logic must not exist twice with two boards each carrying
their own copy.** Concretely:

- **Bias toward one source file with `#ifdef`-guarded sections over two source files with duplicated logic.**
  Where the two boards' behavior genuinely diverges (UI present vs. absent, which USB-OTG peripheral role
  each port plays), guard the divergent lines in the shared file rather than forking the file.
- **A clean, board-only file is fine when the *whole file* is board-specific** — a UI driver ADV needs and
  PoE-P4 can't use at all, or a headless bring-up entry point PoE-P4 needs and ADV doesn't, are legitimately
  separate files. The test is whether the *file* is board-specific, not whether *any* line in it is — a file
  that is 90% shared logic with a 10% board-specific tail is a #ifdef candidate, not a fork candidate.
- **What this rules out:** copying a function (or a modified near-copy of one) into a second file so each
  board can edit "its own" version. That is exactly the failure mode STYLE and RFC 0002's extract campaign
  already spend effort preventing inside a single build target — this goal is the same discipline applied
  across the ADV/PoE-P4 boundary instead of within one binary.
- This is the resolution to the open question Phase 0 (§7) originally left unstated: `main.cpp`'s
  board-specific code (UI on ADV, none on PoE-P4) stays **one shared source tree with target guards**, not a
  shared-core-plus-two-app-layers split, except where a whole file is legitimately one board's alone.
- Radio control (`radio_control_qmx.cpp`), the decode/autoseq/TX pipeline, and the sidekick protocol client
  are the clearest shared-code candidates — none of that logic has any reason to differ by main board at all.
  USB host bring-up (which peripheral is host, which is console) and the UI layer are the clearest
  board-specific candidates.

## 6. Non-goals

- **Not either-or with the ADV build.** Per I27, the ADV remains the shipped, screen-and-keyboard product —
  but per operator direction 2026-09-16, this is not "a parallel path built separately and occasionally
  checked": **every build produces both an ADV and a PoE-P4 binary** (§7 Phase 0). Letting the P4 target
  bit-rot between occasional manual builds is exactly the failure mode this rules out.
- **Not a second sidekick.** The AtomS3 Lite companion is required hardware for this main board too (I27
  already assumed this), but the sidekick **firmware image must be the same binary** regardless of which main
  board it's paired with — no `#ifdef`/build-time branch on ADV-vs-P4 inside the sidekick. RFC 0004 already
  specifies the protocol against the application, not the board (its §1.3 "no dependency on the ADV's
  keyboard or screen" constraint), which is what makes this possible; §7 Phase 3 is where that gets verified
  rather than assumed.
- Not on-chip WiFi/BLE on the main board — unchanged from RFC 0001's non-goals; WiFi stays on the companion.
- Not decode-quality tuning (I26) — this RFC is about whether/how samples get captured and moved, not about
  `ftx_find_candidates` parameters.
- Not the sidekick protocol or web app (RFC 0004) — the companion's role is unchanged; only the main board
  under it is new.
- Not PoE. The board's PoE input exists in silicon; this project has no plan to use it (§4).
- Not a commitment to ship I27's full scope on a fixed timeline — Phase 0 through Phase 4 (§7) is the
  sequencing, not a promise of when each lands.

## 7. Phased plan (draft — confirm before starting Phase 0)

**Phase 0 — bench bring-up and dual-target build, no radio yet.** Confirm on the actual U213 unit, before
anything else depends on them: (a) the two-USB-C-port claim in §4 — enumerate a device on the Host port
while a serial console is live on the Download/OTG port, simultaneously, and confirm 5V-in over that same
diagnostic port (Plan A power, §4) doesn't conflict with its console role; (b) `idf.py set-target esp32p4`
builds cleanly against this project's pinned ESP-IDF v5.5.1; (c) `usb_host_uac` and `usb_host_cdc_acm`
(already vendored, already list `esp32p4` in `targets:`) actually link and run on P4, not just declare
support in their manifests. **Build-system requirement, operator-set 2026-09-16: every build produces both
the ADV (esp32s3) and PoE-P4 (esp32p4) binaries — this is not a target you pick.** `.github/workflows/ci.yml`
is currently esp32s3-only and needs a second matrix job, run alongside the existing one, not in place of it;
local `idf.py build` is single-target per `sdkconfig`, so the two boards need either separate build
directories (`idf.py -B build.adv`, `-B build.p4`) or an equivalent wrapper so a normal build session doesn't
silently produce only one. Deciding that mechanism is Phase 0's actual scope; the source-layout question it
might otherwise have left open is already answered by §5 — shared files with `#ifdef` guards, board-only
files where a whole file is genuinely one board's, no duplicated logic. **Also part of Phase 0, per §3:**
read `qmx-panadapter`'s source for its USB host bring-up and QMX UAC/CAT handling on P4 before writing our
own — cite anything adopted by source file/commit, per this project's licensing discipline, rather than
reconstructing it blind and converging on the same answer by coincidence.

**Phase 1 — USB host QMX bring-up.** Repeat B26's original test method (CAT frequency-set, verified on the
QMX's own screen) on this board specifically, now that Phase 0 has (hopefully) removed the need for an
external UART adapter. This is the bench proof the operator chose to skip at the SoC-family level in §3; it
still needs to happen at the board level before `radio_control_qmx.cpp` / `stream_uac.cpp` get ported, since
those files assume a working host stack under them, not prove one.

**Phase 2 — headless decode/TX core.** Port the decode/autoseq/TX pipeline without the UI stack (`M5Unified`,
`M5GFX`, `M5Cardputer` — none of which this board can use, since it has no display). This is likely the
bulk of the porting effort and the place `main.cpp`'s UI-vs-core coupling (RFC 0002's extract campaign) pays
off or doesn't.

**Phase 3 — companion link.** AtomS3 Lite over Grove UART, unchanged mechanism and role from the ADV
(I3/RFC 0001 §5.1) — this board's Grove pins (`G53`/`G54`) are the same physical pattern. **Verifies, rather
than assumes, the shared-binary requirement (§6): the same sidekick image already running against the ADV
must pair against the P4 board with zero rebuild.** If that fails, the failure is in the protocol or in
something the sidekick assumed about its main-board partner, not in a missing per-board sidekick variant —
there isn't going to be one. **Known blocker, found 2026-09-17 (see Status amendment):** before this can
land, the porta pump (`porta_tick()` + 3 sibling tick functions + `porta_start()`) needs to come out of
`gps_runtime_tick()`/`app_task_core0()` into something a headless board's own main loop can call too — it's
currently reachable only from ADV's `M5Cardputer`-driven loop, not because the pump itself needs a display.

**Phase 4 — field power validation.** Plan A per §4: one 5V/USB-C source into the Download/OTG port powers
the P4 board itself; the Host port sources QMX's 5V the way the ADV's USB host port already does; the
AtomS3 Lite draws from the Grove 5V/GND pair, same as the ADV supplies it today. The open question is
whether one external supply genuinely carries all three loads at once — I27's original "does one USB-C
battery feed QMX + P4 + Atom" question, now sharpened to a specific port and a specific plan rather than a
generic battery story. No PoE injector is assumed or tested here.

Each phase gets its own `ROADMAP.md` Backlog row with a Done-when when it's actually sequenced into Now —
this RFC sets the shape, not the schedule.
