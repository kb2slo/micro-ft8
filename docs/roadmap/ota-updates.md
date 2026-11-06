# OTA updates (epic ukk6)

After the first install, every firmware update, for every board, happens over the air from the web app. Rows: [wanw](#wanw), [v697](#v697), [hstv](#hstv), [yme5](#yme5) ([ROADMAP.md](../ROADMAP.md)).

## Goal

* **First install is from the public site:** the Web Serial flasher on the downloads page writes a full merged image over USB, and recovery uses the same path ([sidekick.md](../architecture/sidekick.md), **First install**; [main_board.md](../architecture/main_board.md), **Two USB-C ports**).
* **Every update after that is OTA,** for both boards. Noticing an update, the prompt, the download and the install all happen in the web app.

## Constraints

* **No TLS and no signing on the device.** Trust comes from the browser's HTTPS fetch of the manifest, as for the app bundle ([system.md](../architecture/system.md), **The inversion** and **Why not signing**).
* **An update never starts by itself, and the radio never restarts by itself afterwards.** The operator starts it, and taps **Resume operating** when it is done.
* **The main board has no network.** Its image reaches it over Port A.
* **The `continuous` channel only.** Choosing a channel is **Later**.

## Decided

* **The sidekick downloads images itself** (§ **Download**), not the phone: one path, not two (**Why not**).
* **Every update stops radio operation first,** a sidekick-only update included: its restart takes the phone UI away while the main board could keep transmitting unattended (§ **Update mode**).
* **The `FW_*` messages are frozen across `PORTA_PROTO_VERSION` bumps,** so any two builds can always carry an update between them.
* **When both boards need updating, the main board goes first,** while the sidekick still speaks its protocol; the frozen messages make that safe.
* **A main-board image counts as good** once the Port A handshake with the sidekick completes after its restart; otherwise the bootloader rolls back. The update is reported done only when the version the board then reports equals the manifest's.

## Design

Facts it rests on, checked 2026-11-06: the release host serves `/download/manifest.json` over plain HTTP as well as HTTPS, with no redirect; each build's version string is its git SHA (`PROJECT_VER`, `tools/git_version.cmake`); `porta_proto.h` reserves `PORTA_MSG_FW_BEGIN`/`FW_CHUNK`/`FW_END` (`0x10`–`0x12`), unused; the sidekick already has two 3 MB OTA slots and rollback (`sidekick/partitions.csv`, `esp_ota_mark_app_valid_cancel_rollback()` in `sidekick/main/main.c`); the P4 has one `factory` slot, no rollback, no build version, and sends no HELLO (`poe_p4/partitions.csv`, `poe_p4/CMakeLists.txt`).

### Manifest ([wanw](#wanw))

OTA writes an **app image**, not the merged `0x0` image. CI adds, per board, an `ota` entry: file, size, SHA-256, version (the SHA), and the `PORTA_PROTO_VERSION` it speaks. The merged images stay for first install.

### Download ([v697](#v697))

The web app fetches the manifest over HTTPS, so the browser authenticates it. It POSTs to the sidekick, behind the pairing token: which board, the image URL, size and SHA-256. The sidekick downloads the image over plain HTTP into its staging slot and refuses it unless the hash matches. Tampering in transit can only make the download fail.

* **The phone makes one short request,** then polls for progress, so a screen lock mid-update does not matter.
* **Updates need the sidekick on a network with internet** (home WiFi or the phone's hotspot). In AP mode they are refused with that reason.
* **Staging:** the sidekick has no PSRAM, so an image is staged in flash, in its **inactive OTA slot** (3 MB): its own next image or a main board's. The `web` partition is too small for a main-board image, so one image is staged at a time, and a sidekick update and a main-board update run one after the other.
* **Sidekick self-update:** download into the inactive slot, verify, set the boot partition, restart. The existing rollback covers an image that boots but cannot talk to the main board.

### Main board over Port A ([hstv](#hstv))

* **A build version for the P4** (`tools/git_version.cmake`, as the sidekick's), and **the main board sends HELLO** with its `PORTA_PROTO_VERSION` and build version, on the sidekick's cadence. The sidekick adds both to its config JSON.
* **Partitions:** `nvs`, `otadata`, `phy_init`, `ota_0` and `ota_1` (4 MB each), `storage` 3 MB, in 16 MB. A new table is written only over USB, so each board gets it once, from the merged image; `storage` is lost (pre-alpha). `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` on.
* **Transfer:** after the image is staged, verified, and the board is in update mode, the sidekick sends `FW_BEGIN` (size, SHA-256, version), `FW_CHUNK` (offset, up to ~240 bytes, each acknowledged), `FW_END`. The main board writes the chunks into its inactive slot (`esp_ota_begin/write/end`), checks the SHA-256 itself, and refuses the image unless the version embedded in it (`esp_ota_get_partition_description`) equals `FW_BEGIN`'s; then it sets the boot partition and restarts.
* **An interrupted transfer resumes** from the last acknowledged offset: a transfer takes minutes.
* **The receiver lives in `components/porta`,** so any main board shares it, and its chunk, offset and hash state machine is `host_mock`-tested.
* **Time:** unmeasured. Stop-and-wait, one ACK per chunk, at `PORTA_BAUD`; a window on `FW_CHUNK` is **Later**.

### Update mode ([hstv](#hstv))

A new Port A action puts the main board into update mode:

* it cancels any transmission, and the flow waits for the TX status the board already sends to say TX has stopped;
* it clears the autoseq queue, the CQ with it, and stops decoding, CAT and USB audio;
* while in it, the board refuses every transmit action, from any phone;
* it leaves only by restarting into the new image, or by an explicit resume action if the update fails or is cancelled. After a restart the radio comes up stopped, as after every restart today.

### Version pairing ([yme5](#yme5))

The web app compares each board's running version against the manifest and offers an update per board. Settings shows both boards' versions beside the sidekick's. It refuses a combination whose `PORTA_PROTO_VERSION`s differ: a board that can no longer talk to the sidekick can only be recovered over USB.

### The web app ([yme5](#yme5))

1. **Available.** When the app opens, and on its 60 s check (`checkAppStale`), it fetches the manifest. For each board with a newer version: a dot on the Settings gear and an **Updates** section in Settings listing each board's current and new version, size and rough time; and, **when no QSO is in progress**, one line where `#opQueueEmpty` reads "No active QSOs": "Update available", linking to Updates. It disappears while a QSO is active. No background notification: Web Push needs a secure origin ([webapp.md](../architecture/webapp.md), **A non-secure origin**).
2. **Not now.** Nothing stops; the notices stay. No "skip this version": the next build replaces it.
3. **Review.** **Update** opens a screen that says what will happen before anything does: which boards, in which order, roughly how long, and **what stopping the radio costs**, concretely ("Your CQ will end", "QSO with K1ABC will be abandoned", "2 queued replies will be cleared"), a QSO in progress shown as a warning. With no internet on the sidekick the button is disabled with the reason ("Connect the sidekick to WiFi with internet"). Two buttons: **Stop radio and update** (destructive) and **Cancel**.
4. **Progress.** A full-screen step list: stop radio → download → verify → transfer (percent and time left) → restart → confirm new version. The state lives on the sidekick, so locking or closing the phone does not matter, reopening the app shows where it is, and a second phone sees the same screen and cannot transmit. **Cancel** until the image is fully written; the old firmware is untouched until then.
5. **Result.** "Updated to d18f9b2", or a plain reason ("Verification failed — still on 7e61488", "New version did not start — rolled back to 7e61488"), with **Retry**.
6. **After.** The radio **stays stopped** until **Resume operating**, whether the update succeeded, failed or was cancelled.

While an update runs, the app suppresses its lost-contact banner and its device-reset banner; both would fire on a planned restart.

## Children

In order: each needs the one before it.

* <a id="wanw"></a>**wanw: CI publishes app-only OTA images** (**Manifest**). **Done-when:** `/download/manifest.json` carries a per-board `ota` entry (file, size, SHA-256, version, protocol version) and the files are served. **Lands in:** RELEASE_PROCESS.md (what a release publishes); TEST_PLAN's Automated table if CI gains a check of the manifest.
* <a id="v697"></a>**v697: the sidekick updates itself** (**Download**). **Done-when:** a sidekick updates from the web app; a forced bad image rolls back (field). **Lands in:** sidekick.md **Firmware updates** (its "not yet built" label goes); system.md (its label); TEST_PLAN Pending.
* <a id="hstv"></a>**hstv: main boards update over Port A, P4 first** (**Main board over Port A**, **Update mode**). **Done-when:** the receiver `host_mock`-tested; a P4 updates from the web app with transfer time measured, and a forced bad image rolls back (field). **Lands in:** main_board.md (partitions, rollback, update mode); sidekick.md **Port A** (the `FW_*` messages, the transfer time); RELEASE_PROCESS.md (the image carries a version); system.md (its label); TEST_PLAN Pending; README and site audits (the one-time USB reflash for the new partition table).
* <a id="yme5"></a>**yme5: the web app's update flow** (**Version pairing**, **The web app**). **Done-when:** **The web app**'s steps 1–6 walked with a QSO in progress, the phone locked mid-transfer, and a second phone open (field). **Lands in:** webapp.md **Screens**; TEST_PLAN Pending; site audit (Settings' Updates section, and Getting started if it says how to update).

### Later

* **Choosing a release channel:** `continuous`, or versioned `v*` releases ([RELEASE_PROCESS.md](../RELEASE_PROCESS.md)).
* **The app bundle relay** (`/update`) moving to the sidekick-downloads path.
* **A window on `FW_CHUNK`,** if [hstv](#hstv)'s measured transfer time is too long.

## Why not

* **The phone relays the image,** as it does the app bundle: it works in AP mode wherever the phone has signal, but the whole image sits in the browser as a Blob (Safari cannot stream a request body), and iOS suspends the page if the screen locks mid-upload.
* **IP over Port A** (PPP, NAPT on the sidekick, lwIP and HTTP on the main board): a redesign of Port A that costs RAM on the main board and runs slower than raw chunks on the same wire, and it would let a main board download for itself, the unattended-update trigger for reopening signing. Reopen if a main board needs the network for something continuous that cannot be handed down over Port A.
* **The P4 on its own Ethernet:** not a path this project uses ([main_board.md](../architecture/main_board.md)).
* **TLS on the sidekick:** not needed while the host serves plain HTTP. Reopen if the host ever forces HTTPS; measure the handshake's heap cost first, since the sidekick has no PSRAM.
