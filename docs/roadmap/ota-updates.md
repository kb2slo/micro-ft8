# OTA updates (epic ukk6)

After the first install, every firmware update, for every board, happens over the air from the web app. Rows: [v697](#v697), [hstv](#hstv), [yme5](#yme5), [antu](#antu), [nufg](#nufg) ([ROADMAP.md](../ROADMAP.md)).

## Goal

* **First install is from the public site:** the Web Serial flasher on the downloads page writes a full merged image over USB, and recovery uses the same path ([sidekick.md](../architecture/sidekick.md), **First install**; [main_board.md](../architecture/main_board.md), **Two USB-C ports**).
* **Every update after that is OTA,** for both boards. Noticing an update, the prompt, the download and the install all happen in the web app.

## Constraints

* **No TLS and no signing on the device.** Trust comes from the browser's HTTPS fetch of the manifest, as for the app bundle ([system.md](../architecture/system.md), **The inversion** and **Why not signing**).
* **An update starts only by an operator's action in the app,** never remotely and never by itself (maintainer, 2026-11-12). The operator is asked to switch the radio off first and back on when it is done.
* **One update, as the operator sees it** (maintainer, 2026-11-12). The app, the main board and the sidekick are parts of it, named in its progress and in a failure, and nowhere else offered or described apart.
* **The main board has no network.** Its image reaches it over Port A.
* **The `continuous` channel only,** until [nufg](#nufg) adds versioned releases.

## Decided

* **The sidekick downloads images itself** (§ **Download**), not the phone: one path, not two (**Why not**).
* **The operator is asked to switch the radio off for a firmware update, and nothing checks.** One left connected may need switching off and on afterwards; that is all a check would prevent, and two attempts at one cost more than it is worth ([main_board.md](../architecture/main_board.md), **Firmware updates**).
* **Nothing transmits during any step of an update** (maintainer, 2026-11-12, at the bench: a CQ went out through a 79 s transfer). From the tap to the end the main board holds transmit off and ends its queue; it goes on receiving, and the operator starts again afterwards. This replaces "nothing is stopped" (2026-11-10).
* **The boards update together** (maintainer, 2026-11-12). **Update** is offered only when a main board is linked and the sidekick has internet, and takes every part that differs to the published build; nothing is offered for part of a station, and a sidekick with no main board is not updated over the air at all (USB is the way out). Two boards cannot be made atomic, since each installs and restarts by itself: the main board goes first, so its failure changes nothing, and if the sidekick's then fails, the next **Update** finishes it. The app enforces this; the sidekick still takes a one-board request, which is how `install_firmware.py` sends a bench build.
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
* **No banners to suppress.** The lost-contact state is the radio bar's, and the progress sheet covers it on a phone following an update; there is no device-reset banner.
* **No time left** on the transfer step, only a percentage.

## Children

Only the next child is a row in the index ([ROADMAP.md](../ROADMAP.md), Now: [antu](#antu)); the others wait here, in order, until it ships. The first three are built and wait only on their field checks ([TEST_PLAN.md](../TEST_PLAN.md), Pending).

In order: each needs the one before it.

* <a id="v697"></a>**v697: the sidekick updates itself** (**Built, awaiting the bench**). **Done-when:** a sidekick updates from the web app; a forced bad image rolls back (field). Built; open for the field check. **Lands in:** sidekick.md **Firmware updates**, webapp.md **Settings › Updates**, system.md and TEST_PLAN Pending, all done; this entry goes when the check passes.
* <a id="hstv"></a>**hstv: main boards update over Port A, P4 first** (**Built, awaiting the bench**). **Done-when:** the receiver `host_mock`-tested; a P4 updates from the web app with transfer time measured, and a forced bad image rolls back (field). Built; open for the field check and the transfer time. **Lands in:** main_board.md **Firmware updates** and its partitions, sidekick.md **Firmware updates** and **Port A**, RELEASE_PROCESS.md, system.md and TEST_PLAN Pending, all done; the measured transfer time goes into sidekick.md. No README or site note for the one-time USB reflash: only a board flashed before the two-slot table needs it, and pre-alpha carries no upgrade notes.
* <a id="yme5"></a>**yme5: the web app's update flow** (**The web app**). **Done-when:** **The web app**'s steps 1–6 walked with a QSO in progress, the phone locked mid-transfer, and a second phone open (field). Built; open for the field check. **Lands in:** webapp.md **Settings › Updates** and **The radio bar**, sidekick.md and TEST_PLAN Pending, all done; the site audit (Settings' Updates section, and Getting started's Updates line) is owed when the check passes.
* <a id="antu"></a>**antu: one Update for the whole station.** After [yme5](#yme5); epic [eu2t](release-1-0.md) needs it. The web app is a separate bundle only for a developer's convenience; an operator sees one station and one **Update** (**Decided**). **To build, in the page and the build script:** the Updates list is one line (the published build, about how long), with the app, the main board and the sidekick named only as progress steps and in a failure; the app is installed first, by the phone relay, then the firmware plan runs; the tap takes the hold on transmit before the app's step (`POST /api/fw/hold`, [sidekick.md](../architecture/sidekick.md), **Firmware updates**) and gives it back if no firmware follows; the review says once that transmitting stops until the update is done, in a dialog shared with the band and frequency change, whose sentence (`retuneEnds()`: "ends your QSO with …", "stops the transmission on the air") it reuses, adding a running CQ, and which replaces that change's native confirm; it does not promise the station keeps receiving, since decoding stops during a transfer ([main_board.md](../architecture/main_board.md), **Firmware updates**); with no main board linked or the sidekick on its own access point, Software update is unavailable and says why; **Install / update** leaves Settings, the sidekick's `/update` page staying for first install and recovery. **Found on a first build (2026-11-12, scrapped: it predated the rules above), not to be rediscovered:** (1) the build ID cannot go in the bundle's `manifest.json`: the sidekick's parser refuses a field it does not know and every installer posts the manifest as fetched, so a new field stops every sidekick already flashed from installing an app; the build script writes a `build.json` beside the manifest instead (the build ID stamped into `app.html`, and the bundle's size), never one of its assets. (2) The page that started the update follows it to the result and reloads on **Close**; a page reloaded mid-update is no longer the one that started it and loses the result and **Retry**. (3) A sidekick whose firmware is too old for the bundle refuses it at `POST /api/bundle/begin` ("api_min … requires firmware API"); the plan then goes on to the firmware and offers the app again after, or the operator has no way forward. (4) One version for the whole station cannot be shown yet: firmware is published only when a push touches it and the app on every push, so their builds differ on an up-to-date station until [nufg](#nufg) publishes them as one. **Done-when:** the plan with an app step, the review's sentence and the relay are host-tested; with all three behind, one **Update** ends a running CQ, brings app and both boards to the published build, and Settings shows no other update control; a band change asks in the same dialog (field). **Lands in:** webapp.md **Settings › Updates** and **Build**; sidekick.md **Installing the app bundle**; TEST_PLAN Pending; site audit (Getting started's Updates line names the control).
* <a id="nufg"></a>**nufg: versioned releases, and a version an operator can read.** After [antu](#antu); epic [eu2t](release-1-0.md) needs it. A build from a `v*` tag reports that version (`1.0.0`), any other build its SHA as today; a board running a versioned release is offered only the next versioned release, a `continuous` build only `continuous` ([RELEASE_PROCESS.md](../RELEASE_PROCESS.md)). **Done-when:** the manifest carries both channels; a board flashed from a `v*` tag shows its version in Settings and is offered the next tag, not `continuous` (field). **Lands in:** RELEASE_PROCESS.md (an image carries its version; what each channel publishes); webapp.md (**Updates**); TEST_PLAN Pending.

### Later

* **The sidekick downloads the app bundle itself,** published with the firmware as one unit, so the app step of [antu](#antu) needs no phone and the station has one version (with [nufg](#nufg)).
* **A window on `FW_CHUNK`,** if [hstv](#hstv)'s measured transfer time is too long.

## Why not

* **The phone relays the image,** as it does the app bundle: it works in AP mode wherever the phone has signal, but the whole image sits in the browser as a Blob (Safari cannot stream a request body), and iOS suspends the page if the screen locks mid-upload.
* **IP over Port A** (PPP, NAPT on the sidekick, lwIP and HTTP on the main board): a redesign of Port A that costs RAM on the main board and runs slower than raw chunks on the same wire, and it would let a main board download for itself, the unattended-update trigger for reopening signing. Reopen if a main board needs the network for something continuous that cannot be handed down over Port A.
* **The P4 on its own Ethernet:** not a path this project uses ([main_board.md](../architecture/main_board.md)).
* **TLS on the sidekick:** not needed while the host serves plain HTTP. Reopen if the host ever forces HTTPS; measure the handshake's heap cost first, since the sidekick has no PSRAM.
