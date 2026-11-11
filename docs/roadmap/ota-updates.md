# OTA updates (epic ukk6)

After the first install, every firmware update, for every board, happens over the air from the web app. Rows: [v697](#v697), [hstv](#hstv), [yme5](#yme5), [antu](#antu), [nufg](#nufg) ([ROADMAP.md](../ROADMAP.md)).

## Goal

* **First install is from the public site:** the Web Serial flasher on the downloads page writes a full merged image over USB, and recovery uses the same path ([sidekick.md](../architecture/sidekick.md), **First install**; [main_board.md](../architecture/main_board.md), **Two USB-C ports**).
* **Every update after that is OTA,** for both boards. Noticing an update, the prompt, the download and the install all happen in the web app.

## Constraints

* **No TLS and no signing on the device.** Trust comes from the browser's HTTPS fetch of the manifest, as for the app bundle ([system.md](../architecture/system.md), **The inversion** and **Why not signing**).
* **An update never starts by itself.** The operator starts it, and is asked to switch the radio off first and back on when it is done.
* **The main board has no network.** Its image reaches it over Port A.
* **The `continuous` channel only,** until [nufg](#nufg) adds versioned releases.

## Decided

* **The sidekick downloads images itself** (§ **Download**), not the phone: one path, not two (**Why not**).
* **The operator is asked to switch the radio off for a firmware update, and nothing checks.** One left connected may need switching off and on afterwards; that is all a check would prevent, and two attempts at one cost more than it is worth ([main_board.md](../architecture/main_board.md), **Firmware updates**). The main board cancels transmission before it restarts. An app-only update asks nothing.
* **The `FW_*` messages are frozen across `PORTA_PROTO_VERSION` bumps,** so any two builds can always carry an update between them.
* **When both boards need updating, the main board goes first,** while the sidekick still speaks its protocol; the frozen messages make that safe.
* **A main-board image counts as good** once the Port A handshake with the sidekick completes after its restart; otherwise the bootloader rolls back. The update is reported done only when the version the board then reports equals the manifest's.

## Design

Facts it rests on, checked 2026-11-10: the release host serves `/download/manifest.json` over plain HTTP as well as HTTPS, with no redirect; each build's version string is its git SHA (`PROJECT_VER`, `tools/git_version.cmake`), the P4's included, and the manifest's `ota` entry carries it ([RELEASE_PROCESS.md](../RELEASE_PROCESS.md), **Public Cloudflare mirror**); `porta_proto.h` reserves `PORTA_MSG_FW_BEGIN`/`FW_CHUNK`/`FW_END` (`0x10`–`0x12`), unused; the sidekick already has two 3 MB OTA slots and rollback (`sidekick/partitions.csv`, `esp_ota_mark_app_valid_cancel_rollback()` in `sidekick/main/main.c`); a P4 flashed before the two-slot partition table has one `factory` slot and answers `FW_BEGIN` with `NO_SLOT` until it is flashed over USB once.

### Built, awaiting the bench ([v697](#v697), [hstv](#hstv))

Both boards' updates are built and their field checks are owed ([TEST_PLAN.md](../TEST_PLAN.md), Pending); what is true of the code is in the architecture docs, not here:

* **The sidekick's side:** the request, the download, the checks, staging, and the transfer down Port A ([sidekick.md](../architecture/sidekick.md), **Firmware updates**).
* **The main board's side:** HELLO, the receiver, what `FW_END` checks, and the trial ([main_board.md](../architecture/main_board.md), **Firmware updates**).
* **The page's side so far** ([webapp.md](../architecture/webapp.md), **Settings › Updates**).

Still open for [hstv](#hstv):

* **Time:** measured, 74.4 s for 872 KB ([sidekick.md](../architecture/sidekick.md), **A main board's image**). A window on `FW_CHUNK` stays **Later**, with no case for it yet.
* **The QMX after the update:** left connected, it wedged on every update restart (2026-11-10: three logged, and a fourth where it did not come back), and the operator is told so. Owed: that a QMX switched off for the update and on afterwards connects with no further power-cycle.

### The web app ([yme5](#yme5))

Built: the plan, the pairing rule, Review, Progress and Result are in [webapp.md](../architecture/webapp.md), **Settings › Updates**. Where the app as it now is made the sketch here wrong, the build follows the app:

* **No "Update available" line on Operate.** The line it was to sit in is gone, and Operate has no banners (operator decision 2026-10-25). The Settings tab's dot is the notice.
* **No banners to suppress.** The lost-contact state is the radio bar's, which reads **Updating firmware** instead on a phone following one; there is no device-reset banner.
* **No time left** on the transfer step, only a percentage.

## Children

Only the next child is a row in the index ([ROADMAP.md](../ROADMAP.md), Backlog: [v697](#v697)); the others wait here, in order, until it ships.

In order: each needs the one before it.

* <a id="v697"></a>**v697: the sidekick updates itself** (**Built, awaiting the bench**). **Done-when:** a sidekick updates from the web app; a forced bad image rolls back (field). Built; open for the field check. **Lands in:** sidekick.md **Firmware updates**, webapp.md **Settings › Updates**, system.md and TEST_PLAN Pending, all done; this entry goes when the check passes.
* <a id="hstv"></a>**hstv: main boards update over Port A, P4 first** (**Built, awaiting the bench**). **Done-when:** the receiver `host_mock`-tested; a P4 updates from the web app with transfer time measured, and a forced bad image rolls back (field). Built; open for the field check and the transfer time. **Lands in:** main_board.md **Firmware updates** and its partitions, sidekick.md **Firmware updates** and **Port A**, RELEASE_PROCESS.md, system.md and TEST_PLAN Pending, all done; the measured transfer time goes into sidekick.md. No README or site note for the one-time USB reflash: only a board flashed before the two-slot table needs it, and pre-alpha carries no upgrade notes.
* <a id="yme5"></a>**yme5: the web app's update flow** (**The web app**). **Done-when:** **The web app**'s steps 1–6 walked with a QSO in progress, the phone locked mid-transfer, and a second phone open (field). Built; open for the field check. **Lands in:** webapp.md **Settings › Updates** and **The radio bar**, sidekick.md and TEST_PLAN Pending, all done; the site audit (Settings' Updates section, and Getting started's Updates line) is owed when the check passes.
* <a id="antu"></a>**antu: one Update for the whole station.** After [yme5](#yme5); epic [eu2t](release-1-0.md) needs it. The web app is a separate bundle only for a developer's convenience; an operator sees one station and one **Update**. The bundle's manifest carries its build ID; Settings › Updates lists **App** beside the boards when the published bundle differs from the running one; **Update** installs the app first, by the phone relay, then runs the firmware plan. An app-only update asks nothing of the operator; a firmware update asks for the radio off (**Decided**). On the sidekick's own network, where firmware cannot be fetched, the app alone is offered. **Install / update** leaves Settings; the page stays for first install and recovery. **Done-when:** the plan with an app step is host-tested; with all three behind, one **Update** brings app and both boards to the published build and Settings shows no other update control; with only the app behind, it updates with the radio running (field). **Lands in:** webapp.md **Settings › Updates** and **Build**; sidekick.md **Installing the app bundle**; TEST_PLAN Pending; site audit (Getting started's Updates line names the control).
* <a id="nufg"></a>**nufg: versioned releases, and a version an operator can read.** After [antu](#antu); epic [eu2t](release-1-0.md) needs it. A build from a `v*` tag reports that version (`1.0.0`), any other build its SHA as today; a board running a versioned release is offered only the next versioned release, a `continuous` build only `continuous` ([RELEASE_PROCESS.md](../RELEASE_PROCESS.md)). **Done-when:** the manifest carries both channels; a board flashed from a `v*` tag shows its version in Settings and is offered the next tag, not `continuous` (field). **Lands in:** RELEASE_PROCESS.md (an image carries its version; what each channel publishes); webapp.md (**Updates**); TEST_PLAN Pending.

### Later

* **The sidekick downloads the app bundle itself,** published with the firmware as one unit, so the app step of [antu](#antu) needs no phone and the station has one version (with [nufg](#nufg)).
* **A window on `FW_CHUNK`,** if [hstv](#hstv)'s measured transfer time is too long.

## Why not

* **The phone relays the image,** as it does the app bundle: it works in AP mode wherever the phone has signal, but the whole image sits in the browser as a Blob (Safari cannot stream a request body), and iOS suspends the page if the screen locks mid-upload.
* **IP over Port A** (PPP, NAPT on the sidekick, lwIP and HTTP on the main board): a redesign of Port A that costs RAM on the main board and runs slower than raw chunks on the same wire, and it would let a main board download for itself, the unattended-update trigger for reopening signing. Reopen if a main board needs the network for something continuous that cannot be handed down over Port A.
* **The P4 on its own Ethernet:** not a path this project uses ([main_board.md](../architecture/main_board.md)).
* **TLS on the sidekick:** not needed while the host serves plain HTTP. Reopen if the host ever forces HTTPS; measure the handshake's heap cost first, since the sidekick has no PSRAM.
