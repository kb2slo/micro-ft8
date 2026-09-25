# RFC 0007: Flashing and updating Micro-FT8

* **Status:** Accepted 2026-09-25

## 1. Goal

Stated by the operator, 2026-09-25:

* **First install is done from the public site.** The Web Serial flasher on `downloads.html` writes a full merged image. Recovery uses the same path.
* **After that, every update is OTA**, for all boards. Noticing an update, the prompt, the download, and the install all happen in the sidekick webapp.
* **M5Launcher is dropped** as a supported install path.
* **The P4 comes first**, then the ADV. Row `za6n` may still drop the ADV altogether.

Decided by the operator, 2026-09-25: **the sidekick downloads images itself** (§3.2), and **the ADV stops carrying and flashing a sidekick image** (§3.10), which frees the flash its A/B layout needs.

## 2. What exists

| | Sidekick (AtomS3 Lite) | P4 (PoE-P4) | ADV (Cardputer) |
|---|---|---|---|
| Flash | 8 MB | 16 MB | 8 MB |
| App image (local build, 2026-09-25) | 955 KB | 725 KB | 2.36 MB, of which ~0.95 MB is the embedded sidekick image (`components/sidekick_flasher`) |
| Partitions | `ota_0`/`ota_1` 3 MB each, `web` 1.9 MB | one `factory` 4 MB, `storage` 3 MB | one `factory` 4.9 MB, `storage` 3 MB |
| Rollback | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`; marked valid after three Port A heartbeats (`sidekick/main/main.c:110`) | none | none |
| Network | WiFi | none (Ethernet PHY present but unused, [main_board.md](../architecture/main_board.md)) | none |
| Update today | USB-C, or field-flashed from the ADV ([sidekick.md](../architecture/sidekick.md) (**Installing the sidekick from the ADV**)) | USB-C | USB-C, M5Launcher, `tools/flash_keep_launcher.py` |

The release host already publishes, per board, a merged `0x0` image and its SHA-256 (`/download/manifest.json`, mirrored to R2 by `ci.yml`). It serves over **plain HTTP as well as HTTPS**: `curl -sI http://micro-ft8.kalikstein.com/download/manifest.json` returns 200 with no redirect (checked 2026-09-25). Each build's version string is the git SHA (`PROJECT_VER`).

Neither main board has a network stack. Any firmware for them reaches them over Port A (`components/porta`, `porta_proto`: 255-byte payloads at 115200 baud), and both boards already link that code.

## 3. Design

### 3.1 Release manifest

OTA writes an **app image**, not the merged `0x0` image. CI adds, per board, an `ota` entry: filename, size, SHA-256, version (the SHA), and the `PORTA_PROTO_VERSION` it speaks. The merged images stay for first install.

**The webapp offers the `continuous` channel only** (operator, 2026-09-25). Choosing a channel is a later phase (§5).

### 3.2 How an image reaches the sidekick: it downloads it

Decided 2026-09-25. The webapp fetches the manifest over HTTPS, so the phone's browser authenticates it. It then POSTs to the sidekick, behind the pairing token ([architecture/system.md](../architecture/system.md) (**Who may key the transmitter**)): which board, the image URL, size and SHA-256. The sidekick downloads the image over plain HTTP into its staging slot (§3.3) and refuses it unless the hash matches.

* **No TLS on the device.** Trust comes from the same HTTPS fetch [architecture/sidekick.md](../architecture/sidekick.md) (**Installing the app bundle**) relies on; someone tampering with the traffic can only make the download fail.
* **The phone only makes a short request**, then polls for progress, so a screen lock mid-update does not matter.
* **Updates need the sidekick on a network with internet** (home WiFi or the phone's hotspot). In AP mode it has none, and updating is refused with that reason. Updating is a desk activity, and the hotspot also works in the field.

The phone-relay alternative is in §4.

The web-page bundle relay (`/update`, [architecture/sidekick.md](../architecture/sidekick.md) (**Installing the app bundle**) steps 1–3) is out of scope here (§5).

### 3.3 Staging on the sidekick

The AtomS3 Lite has no PSRAM, so a whole image never fits in RAM; it is staged in flash. The sidekick's **inactive OTA slot** (3 MB) holds either its own next image or a main-board image. The `web` partition (1.9 MB) is too small for the ADV's. So only one image is staged at a time, and a sidekick update and a main-board update run one after the other.

### 3.4 Sidekick self-update

Download into the inactive slot, verify SHA-256, set the boot partition, restart. The existing rollback covers an image that boots but cannot talk to the host.

### 3.5 Main-board update over Port A

After the image is staged and verified, and the board is in update mode (below), the sidekick sends it down Port A using the message types already reserved in `porta_proto.h` (`0x10`–`0x12`, defined but unused):

* `FW_BEGIN` (size, SHA-256, version)
* `FW_CHUNK` (offset, up to ~240 bytes), acknowledged
* `FW_END`

The main board writes the chunks into its inactive slot (`esp_ota_begin/write/end`), checks the SHA-256 itself, and refuses the image unless the version embedded in it (`esp_ota_get_partition_description`) equals `FW_BEGIN`'s. Then it sets the boot partition and restarts.

**When the new image counts as good** (operator, 2026-09-25): the board marks it valid once the Port A handshake with the sidekick completes; otherwise the bootloader rolls back. The update is reported as done only when the version the board then reports (§3.6) equals the manifest's. The receiver lives in `components/porta`, so both boards share it, and the chunk/offset/hash state machine is `host_mock`-testable. Needs `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` on both main boards.

**An interrupted transfer resumes** from the last acknowledged offset, as the reserved messages' comment already requires: a transfer takes minutes on this link.

**These messages are frozen across `PORTA_PROTO_VERSION` bumps**, so that any two builds can always carry an update between them.

**Update mode.** Every update stops radio operation first, including a sidekick-only update: its restart takes the phone UI away while the main board could keep transmitting unattended. A new Port A action puts the main board into update mode:

* it cancels any transmission, and the flow waits for the TX status the board already sends to report that TX has stopped;
* it turns the beacon off, clears the autoseq queue, and stops decoding, CAT and USB audio;
* while in update mode it refuses every transmit action, from any phone and from the ADV's own keys. The ADV shows "Updating — do not unplug"; the P4 has no screen;
* it leaves update mode only by restarting into the new image, or by an explicit resume action if the update fails or is cancelled. After a restart the radio comes up stopped, as after every restart today (operator, 2026-09-25); nothing new is needed for that.

**Time:** an estimate, not measured. At today's 115200 baud with acknowledged chunks, roughly 1–2 min for the P4 and 3–5 min for the ADV (1.4 MB once §3.10 lands, so nearer 2). §3.11 raises the rate first.

### 3.6 Version pairing

**Each main board reports its version** (required by §3.5's done check). Today only the sidekick sends HELLO (`sidekick/main/main.c:77`); the main board only receives it (`components/porta/porta.cpp:164`), and the webapp shows just the sidekick's version (`sk_fw_version`). The main board sends HELLO too, with its `PORTA_PROTO_VERSION` and build version, on the same cadence. **Found 2026-09-25:** the P4 build has no build version yet (`poe_p4/` uses neither `git_version.cmake` nor `gen_build_identity.cmake`), so it gains one first; the sidekick adds both to its config JSON, and Settings shows them beside the sidekick's.

The webapp compares each board's running version against the manifest and offers an update per board. It refuses a combination whose `PORTA_PROTO_VERSION`s differ, because a board that can no longer talk to the sidekick can only be recovered over USB. When both boards need updating, **the main board goes first**, while the sidekick still speaks its protocol; the frozen OTA messages are what make this safe.

### 3.7 Partition tables

* **P4:** `nvs`, `otadata`, `phy_init`, `ota_0` and `ota_1` (4 MB each), `storage` 3 MB. This fits in 16 MB with room to spare.
* **ADV:** fits once the embedded sidekick image goes (§3.10), bringing the app to ~1.4 MB. For example: two 2.25 MB slots from `0x20000`, then `storage` 3 MB, ending at `0x7A0000` of 8 MB. With the image still in, it would not fit: two 2.5 MB slots plus 3 MB storage exceeds the ~7.9 MB available.

A new partition table can only be written over USB, so each board gets it once, from the Web Serial flasher, as part of the merged image. Existing data on `storage` is lost (pre-alpha).

### 3.8 First install and recovery

The public page's Web Serial flasher, or `esptool.py` with the merged image. Web Serial exists only in desktop Chrome and Edge; no iOS browser has it. Recovery from a board that will not boot is the same path.

### 3.9 Webapp UX

Decided with the operator, 2026-09-25. An update never starts by itself, and the radio never restarts by itself afterwards.

1. **Available.** When the webapp opens, and on its existing 60 s check, it fetches the manifest. For each board with a newer version:
   * a dot on the Settings gear, and an **Updates** section in Settings listing each board's current and new version, size and rough time;
   * **when no QSO is in progress**, one line in the Operate queue area, where `#opQueueEmpty` shows "No active QSOs" today: "Update available" and a link to the Updates section. It disappears while a QSO is active, so it never competes with operating.

   There are no background notifications: Web Push needs an HTTPS origin, and the webapp is served from `http://micro-ft8.local`.
2. **Not now.** Nothing stops; the notices stay. There is no "skip this version": the next build replaces it.
3. **Review.** Tapping **Update** opens a screen that says what will happen before anything does: which boards, in which order, roughly how long, and **what stopping the radio will cost**, listed concretely: "Beacon will turn off", "QSO with K1ABC will be abandoned", "2 queued replies will be cleared". If a QSO is in progress, that line is shown as a warning. If the sidekick has no internet, the button is disabled with the reason ("Connect the sidekick to WiFi with internet"). Two buttons: **Stop radio and update** (styled as destructive) and **Cancel**. One button, not a separate stop step: the screen has already said what is lost.
4. **Progress.** A full-screen step list: stop radio → download → verify → transfer (percentage and time left) → restart → confirm new version. The state lives on the sidekick, so locking or closing the phone does not matter, reopening the app shows where it is up to, and a second phone sees the same screen and cannot transmit. **Cancel** is offered until the image is fully written to the board; the old firmware is untouched until then.
5. **Result.** "Updated to d18f9b2", or a plain reason ("Verification failed — still on 7e61488", "New version did not start — rolled back to 7e61488"), with **Retry**.
6. **After.** The radio **stays stopped** until the operator taps **Resume operating**, whether the update succeeded, failed or was cancelled. Nothing transmits on its own after a restart.

While an update is running, the webapp suppresses its lost-contact banner (30 s without a poll) and its device-reset banner (a link counter dropping). Both would fire on a planned restart.

### 3.10 The ADV stops flashing sidekicks

Decided 2026-09-25. Once the sidekick updates itself (§3.4), the ADV no longer needs to carry and flash a sidekick image. The code goes, not just the image:

* `components/sidekick_flasher`, including `target_firmware/`, and its `esp-serial-flasher` dependency (`main/idf_component.yml`).
* The ADV's probe and flash paths in `main/main.cpp`, and the flasher references in `main/usb_c_presence.h`.
* `porta_board_embedded_sidekick_version()` (both boards' `porta_board.cpp`, `porta_board.h`, the `-u` link flag in `main/CMakeLists.txt`) and the HELLO version-match check in `components/porta/porta.cpp` that uses it. Version pairing moves to the webapp (§3.6).
* `tools/stage_sidekick_firmware.sh`, and the CI steps that stage the sidekick into the ADV build.
* **Kept, and moved:** CI builds the sidekick's public merged image from that same staging folder (`ci.yml`, `tools/merge_sidekick_bin.sh`). That step moves into the sidekick's own build job, so `/download/` still publishes it.
* [sidekick.md](../architecture/sidekick.md) (**Installing the sidekick from the ADV**)'s field-flash design, and the docs and README text that describe it.

**What this gives up:** installing or recovering a sidekick in the field from the ADV. Both now need a computer (Web Serial or `esptool.py`), which is where the P4 already is.

### 3.11 Port A speed

Folded in by the operator, 2026-09-25: a faster link has been discussed before but never tuned, and updates are what make it worth doing. The rate is defined twice today (`kBaud` in `components/porta/porta.cpp:51`, `PORTA_BAUD` in `sidekick/main/main.c:37`); it moves to one constant in `porta_proto.h` that both ends use. Everything on Port A benefits, not only updates.

**The rate is chosen by a directed stress test**, not by watching the link-health counters during normal use (operator, 2026-09-25). Normal traffic is small and bursty, and would pass rates that fail under a firmware transfer.

* **Traffic:** back-to-back 255-byte frames, in both directions at once. Payloads cycle through patterns that stress the framing and the UART: the sync byte `0xC6` repeated, all zeros, all ones, alternating bits, and pseudo-random bytes.
* **Checked independently of the CRC:** each payload carries a sequence number and is generated from a known seed. The receiver regenerates it and compares every byte, so a dropped frame, a duplicate, or corruption the CRC happens to pass is counted separately from CRC errors.
* **Load on the main board:** decoding running and USB audio streaming from a real radio in RX, because interrupt load from USB is what makes a UART drop bytes. A second pass adds TX (Tune into a dummy load).
* **Rate sweep without reflashing:** a test-only message switches both ends to a candidate rate. Both revert to 115200 on their own if no test traffic arrives within a few seconds, so a rate that fails outright cannot strand the link.
* **Pass:** zero CRC errors, drops, duplicates and mismatched bytes over at least 10× the largest image (about 14 MB), on each main board, with the operator's own Grove cable. The chosen rate is the highest that passes, and it must pass again under the TX load.
* **Started from the webapp** (a Settings diagnostic). Unlike update mode (§3.5), RX and decoding keep running, because they are the load; every transmit action is refused except the test's own Tune pass, and the beacon and autoseq queue are stopped first, with the same review-screen warning as an update (§3.9). The result, per rate and per counter, is shown on screen and also written to the log, so the pass is recorded rather than read off a screen. It stays in the product as a diagnostic for a suspect cable.

## 4. Rejected alternatives

* **The phone relays the image**, as the app bundle does ([architecture/sidekick.md](../architecture/sidekick.md), **Installing the app bundle**): the page fetches it over HTTPS and POSTs the bytes to the sidekick. It works in AP mode wherever the phone has cell signal, but the whole image sits in the browser as a Blob (Safari cannot stream a request body), and iOS suspends the page if the screen locks mid-upload. Rejected 2026-09-25 in favour of §3.2; one path, not two.

* **IP over Port A** (PPP on the UART, sidekick routing with NAPT, lwIP and HTTP on the main boards). It redesigns Port A, costs RAM on both main boards, and runs slower on the same wire than raw chunks. It would also let a main board download for itself, which is the unattended-update trigger for reopening signing ([architecture/system.md](../architecture/system.md), **Why not signing**). Reopen if a main board needs the network for something continuous that cannot be handed down over Port A.
* **P4 on its own Ethernet.** [main_board.md](../architecture/main_board.md) records that PoE and Ethernet are not a path this project uses.
* **Signing.** Unchanged from [architecture/system.md](../architecture/system.md) (**Why not signing**): the manifest's hash reaches the device over the owner's HTTPS-authenticated browser, and updates are started by the operator. Its reopen trigger stands.
* **TLS on the sidekick.** Not needed while the host serves plain HTTP. Reopen if the host ever forces HTTPS; measure the handshake's heap cost first, since the board has no PSRAM.
* **M5Launcher.** It owns the ADV's factory slot, which an A/B layout needs. Dropped by the operator, 2026-09-25.

## 5. Later phases

Not part of this design; each gets its own row or RFC when wanted.

* **Choosing a release channel:** `continuous`, or versioned `v*` releases ([RELEASE_PROCESS.md](../RELEASE_PROCESS.md)).
* **The web-page bundle relay** (`/update`) moving to the sidekick-downloads path of §3.2.

## 6. Slices

Each is a `ROADMAP.md` Backlog row.

* **S0** (`v3hh`) — Port A speed (§3.11): the stress test, its message types (frame codec and payload checker `host_mock`-tested), then the rate change. Field check: the stress test passes at the chosen rate on both main boards, under RX and TX load.
* **S1** (`wanw`) — CI publishes app-only images and the manifest's `ota` entries (§3.1).
* **S2** (`v697`) — Sidekick self-update (§3.2, §3.4). Field check: update from the webapp, then a forced bad image rolls back.
* **S3** (`hstv`) — Main-board HELLO (§3.6), update mode, the `FW_*` messages with resume, and the shared receiver, with `host_mock` tests; P4 partition table and rollback enabled (§3.5, §3.7). Field check: P4 updated from the webapp, transfer time measured, a forced bad image rolls back.
* **S4** (`yme5`) — Webapp update UX and version pairing (§3.6, §3.9). Field check: walk §3.9's steps 1–6 with a QSO in progress, with the phone locked mid-transfer, and with a second phone open.
* **S5** (`u8zt`) — Remove the ADV's sidekick flashing (§3.10), after S2 has shipped so the sidekick can still be updated without a computer. Proof: `idf.py build` for the ADV, with the image size drop checked; the sidekick's public merged image is still published; README audit, because a documented workflow goes.
* **S6** (`gujg`) — ADV partition table and rollback, reusing S3's receiver (§3.7). Field check as S3, on the ADV.
* (`ve47`) Remove M5Launcher support: `tools/flash_keep_launcher.py` and the docs that describe it.
