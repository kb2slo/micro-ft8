# Sidekick architecture

The AtomS3 Lite sidekick is the station's WiFi and web server: it serves the web app, guards the control API with the pairing token, and relays between the browser and the main board over Port A. Why it is shaped this way (no TLS, the phone reaches the internet): [system.md](system.md).

## Hardware

**M5Stack AtomS3 Lite**, chosen over the NanoC6 it replaced (2026-09-05) for its dual cores: WiFi, lwIP and an HTTP server run beside the Port A UART work, where the C6 has one 160 MHz core.

* **ESP32-S3FN8**: 8 MB embedded flash, **no PSRAM** (confirmed with `esptool.py flash_id`, 2026-09-04). Anything large is staged in flash, not RAM.
* **Grove** (Port A): `GND / 5V / G2 / G1`. **USB-C** is device mode only — flashing and a console, never a USB host ([system.md](system.md), **Why not**).
* **User button**: GPIO41, active low, used to disclose the pairing token.
* **Over USB it is indistinguishable from the ADV**: both enumerate as Espressif's generic `303A:1001` with no board string, and both are the same chip. The MAC address is the only reliable way to tell them apart; a differing flash vendor is a production-batch artifact, not a model identifier.

Building, flashing, and backing up a unit's stock firmware: [`sidekick/README.md`](../../sidekick/README.md).

## Web root

**The sidekick serves the whole app, and the app is not in the firmware image.**

* **Served locally, not loaded from the internet.** An HTTPS page cannot reach `http://micro-ft8.local` (browsers block active mixed content, with no exception for private addresses), and an app fetched from the internet each session does not exist on a summit. Served from the sidekick, the app and the API are the same origin.
* **Not inside the firmware image**, because the app ships on its own cadence and firmware flash is the wrong vehicle for a weekly UI.
* **The `web` partition is the only HTTP origin**: LittleFS, 1.9 MB, above `ota_1` (`sidekick/partitions.csv`). What is on the filesystem is what the device serves.
* **The firmware carries only a bootstrap**, `sidekick/main/web/` (provisioning, `/update`, `pairing.js`, error pages), embedded by a CMake glob as a **hydrate seed**, not as a second live origin. On boot, if the filesystem is empty, unmountable, or its seed digest differs from the embed, the firmware copies the seed onto it; after that, HTTP reads the filesystem only. An installed bundle may replace any file, seed pages included.
* **Recovery** from present-but-broken files is a forced re-hydrate: token-guarded `POST /api/web/rehydrate` (`web_fs.c`), which restores the seed pages. USB-C reflash is the floor.
* **Consequence, accepted:** a sidekick that has never had internet has only the bootstrap. It can join WiFi and install a bundle, nothing more. System constraint 2 makes that acceptable.
* **Budget:** `web_fs.c` refuses a file over 512 KB, so large assets (the map's basemap files) ship as separate files, never inlined into `app.html`. The partition is sized for the app plus its bundled data; outgrowing it means a new partition table, which costs a USB reflash of every device.

## Installing the app bundle

The phone relays the bundle: `/update` fetches it from the release host over HTTPS (that fetch is what authenticates it), then writes it to the sidekick in three token-guarded steps, so a truncated upload cannot promote and an incompatible bundle is refused before any bytes land (`web_bundle.c`):

1. `POST /api/bundle/begin` — body is the manifest. Refused if its `api_min` exceeds the device's `WEB_API_VERSION`. Wipes the staging area.
2. `PUT /api/bundle/file?path=<relpath>` — one per asset. `Content-Length` must equal the manifest's size, and the body's SHA-256 must equal its digest.
3. `POST /api/bundle/commit` — every asset must be staged; re-hashes from disk, then renames each onto the live tree. A failed promote leaves staging for a retry; a forced re-hydrate recovers the seed pages.

**The manifest** (`manifest.json` in the built bundle, written from the build's output by `webapp/tools/gen_manifest.py` and never committed ([webapp.md](webapp.md), **Build**), parsed by `web_manifest.c`, host-tested by `host_test_web_manifest`) is constrained JSON: `api_min` and an `assets` list of `path`/`sha256`/`size`; no unknown fields, lowercase hex digests, and relative paths only (no leading `/`, no `..`, no dotfiles). The digests catch a corrupted upload over flaky WiFi, not an adversary. **Version pairing:** `api_min` stops a newer app installing onto firmware whose API it outranks, saying so rather than failing later.

### What the release host must provide

The design depends on two properties of the host, not on which host it is:

1. **HTTPS with a publicly trusted certificate** — the only thing authenticating the bundle.
2. **An `Access-Control-Allow-Origin` that lets the plain-HTTP page read the response.** This is a capability, not a security control: the browser is the courier, and it must be able to read the bytes it re-POSTs.

Today's host is a Cloudflare Worker with Static Assets (`webapp/wrangler.jsonc`) at `https://micro-ft8.kalikstein.com`; the app lives under `/app/`. It sends the header only because `assets.run_worker_first` routes every request, including real asset hits, through `worker.js`. Without it, Cloudflare serves assets directly and the header is missing — which once broke `/update` with a generic fetch failure. **Measure any new host before adopting it** (the `curl` in [system.md](system.md), **Third-party services**); GitHub Releases, for one, sends no CORS header and cannot carry the bundle.

Rules that follow:

* **The bundle fetch stays a simple request**: `GET`, no custom headers, so no CORS preflight. Versions belong in the manifest or the URL, never a header.
* **Manifest paths are relative** to the manifest, so the host can move.
* **The host's base URL lives in NVS**, with a build-time default (`https://micro-ft8.kalikstein.com/app`), set with a token-guarded write (`bundle_host.c`, `/api/bundle-host`): a host move is a config change, not a reflash, and a settable host is the most attractive thing on the device for a LAN attacker to repoint.

## Firmware updates

**Decided 2026-09-25, not yet built: rows v697, hstv** ([RFC 0007](../rfcs/0007-flash-and-update.md)). The sidekick downloads firmware images itself, from the release host over plain HTTP, and refuses any image whose SHA-256 differs from the one the web app read over HTTPS and handed it behind the pairing token (the trust argument: [system.md](system.md), **The inversion**). It has no PSRAM, so an image is staged in flash, in its spare OTA slot: either its own next image, or a main board's, which it then sends down Port A. Unlike the app bundle, the image never passes through the browser. Updates need the sidekick on a network with internet, so they are refused in AP mode. Its own rollback already exists: a new image is marked valid only after three Port A heartbeats (`sidekick/main/main.c`).

## Why the sidekick has no TLS

"TLS on the sidekick" is two questions with different answers.

**As a client — the sidekick calling out over HTTPS: not needed, and not free.**

* **Nothing needs it.** The phone reaches every internet service ([system.md](system.md), **The inversion**). The one thing the sidekick will fetch, firmware, is checked against a SHA-256 the browser read over HTTPS, which proves integrity as well as TLS would; TLS would add only confidentiality, for a public file.
* **Its costs, weighed honestly.** Some are weaker than they first looked:
  * **Flash:** ~116 KB, measured on this target. Affordable: the image is under 1 MB in a 3 MB slot.
  * **CA bundle:** it must track root-certificate rotation. Weak, since the device updates itself; only a device offline for years falls behind.
  * **Clock:** checking certificate dates needs the right time. Weak: ESP-IDF's mbedTLS skips the date check by default.
  * **RAM:** a handshake needs tens of KB of heap on a board with no PSRAM that is already running WiFi and the HTTP server. **Unmeasured**, and the one cost that could decide it outright.
* **The cost that holds is architectural.** Once the device can speak HTTPS, the easy path for every new online feature becomes "have the sidekick call it". That erodes the inversion, and those features would then fail in AP mode, where the sidekick has no uplink, while the phone often still has cellular.

**As a server — the browser reaching the sidekick over HTTPS: it would fix our one real security limit, but it cannot be done properly.**

* **What it would fix.** The pairing token and all control traffic cross the LAN in clear ([system.md](system.md), **Who may key the transmitter**). HTTPS would close that, and would also make the app a secure context again: geolocation, a service worker, an offline cache ([webapp.md](webapp.md), **A non-secure origin**).
* **Why it can't be done properly.** Browsers trust a certificate only for a name a public CA can verify, and `micro-ft8.local` resolves on one LAN.
  * A **self-signed** certificate means a full-page warning to click through on every new phone and browser.
  * A **private CA** means installing a root certificate on every phone.
  * The scheme that works — a public domain name that resolves to the device's LAN address, with a CA-issued certificate — puts the private key on the device and needs renewal every few months. Renewal needs internet and a correct clock, exactly what a summit in AP mode lacks.
* **So the limit is accepted and scoped**: a nuisance control, on WPA2/WPA3 networks.

**Reopen TLS as a client** if the release host stops serving plain HTTP, or a feature needs the sidekick itself to reach an HTTPS-only service with no phone present. Measure the handshake's heap first. **Reopen TLS as a server** if a way appears to give a LAN device a browser-trusted certificate without renewal or a clock, or if the station will be operated on networks its owner does not trust.

## Installing the sidekick from the ADV

**To be removed: row u8zt**, once the sidekick updates itself ([RFC 0007](../rfcs/0007-flash-and-update.md) §3.10). Today the ADV carries a copy of the sidekick image (`components/sidekick_flasher`, staged by `tools/stage_sidekick_firmware.sh`) and, when a sidekick is plugged into the ADV's USB-C, can write it through the sidekick's ROM bootloader after the operator confirms — the same protocol `esptool.py` uses, via `esp-serial-flasher`. Before offering, it reads the installed build's `esp_app_desc_t` straight off the sidekick's flash, so it can tell "not ours" from "ours, current" from "ours, outdated" without the app running. Only USB-C can do this: the ROM listens only on UART0's default pins, never on Grove ([`sidekick/README.md`](../../sidekick/README.md), B42).

## Port A: the link to the main board

Everything the app needs crosses Port A: decodes at slot cadence, TX control, config reads and writes, and the log. The message set is defined in `components/porta_proto/include/porta_proto.h`, which is the source of truth (host-tested by `host_test_porta_proto`); the sidekick side is `host_link.c`, the main-board side `components/porta`.

* **Framed messages over a byte pipe**, with framing that assumes no stream, so the transport could change without touching the app or the API.
* **Board-agnostic**: specified against the application, so the ADV and the P4 present the same surface, and **one sidekick image pairs with either main board** — no per-board build or `#if` in the sidekick. There is one sidekick build, and it serves both.
* **Versioned** (`PORTA_PROTO_VERSION`, carried in HELLO), for the same reason the bundle manifest pairs app and API. A mismatch is logged, not refused, so both ends must be flashed together when it changes. v2 (2026-09-28) added each DECODE's slot index and length and the dial frequency at the middle of its signal, which the main board takes from `components/dial_history` (every successful tune through `radio_control_sync_frequency_mode()` is noted there); the sidekick relays them in `/api/events` as `si`, `sl` and `fq`. v4 (2026-09-30) added the SWR sweep ([radio.md](radio.md), **The SWR sweep on the main board**): the `SWEEP_START` action, which `POST /api/swr/start` (token required, body `lo-hi-step,lo-hi-step` in kHz, at most 12 ranges) sends, and the `SWEEP` event, served in `/api/events` as a row with `"d":6` and `k` (0 point, 1 band skipped, 2 done, 3 aborted), `r` (the range), `hz`, `swr` (hundredths, -1 none) and `why` (0 cancelled, 1 CAT lost, 2 stalled). Stopping is the existing `/api/tx/cancel`. The sidekick only relays: the host validates the ranges and its own state and answers with `{"ok":false,"why":...}`. The event ring holds 64 rows, so a page that polls about once a second keeps up with a sweep (about 1.3 points a second) but one that sleeps through it does not; the page marks what it never received as missing ([webapp.md](webapp.md), **The SWR sweep**).
* **One build identity for both sides.** The sidekick and the ADV take their version string from the same `tools/git_version.cmake` (exact git SHA, plus a dirty flag), so "same commit" is an exact string match. CI pins `MICROFT8_GIT_SHA` for both jobs, because a pull request builds a synthesized merge commit and two jobs left to derive it separately could disagree.
* **Asymmetric traffic**: commands are rare and want low latency; decodes arrive every 15 s and tolerate hundreds of milliseconds. A full slot of decodes is about 3 KB, so sustained load is a couple of hundred bytes per second.
* **The browser polls** `GET /api/events?since=<seq>`; the sidekick keeps a ring of recent events so a client can catch up after a short gap. Config is `GET /api/config` (open) and `PUT /api/config` (token-guarded).
* **The sidekick answers for itself without the main board.** Every `GET /api/config` reply carries its own facts (`sk_*`: firmware version, WiFi mode, network, IP, uptime, its link counters), built at reply time (`send_config_reply()` in `host_link.c`), including the `{"ok":false,...}` reply when the main board does not answer. The web app shows them only from a live reply, never from its browser cache.

## Tasks and cores

Nothing of ours is pinned to a core: FreeRTOS places our tasks, and ESP-IDF pins some of its own to CPU0 by configuration (`sidekick/sdkconfig`). As of 2026-09-25:

| Task | Created by | Priority | Core | Stack | Job |
|---|---|---|---|---|---|
| main loop | `app_main` (`main.c`) | 1 | CPU0 | 3.5 KB | Once a second: Port A HELLO, marking a new image valid, the pairing-button disclosure |
| `porta_rx` | `host_link.c` | 5 | any | 4 KB | Reads Port A, parses frames, fills the event ring and the reply state |
| `httpd` | `wifi_prov.c` (`httpd_start`) | 5 | any | 8 KB | Every HTTP handler; control requests wait on `porta_rx` for the main board's reply |
| `scan_fresh` | `wifi_prov.c` | 3 | any | 3 KB | AP mode only: keeps the WiFi scan list fresh for provisioning |
| WiFi driver, mDNS, `esp_timer` | ESP-IDF | IDF defaults (mDNS 1) | CPU0 | — | |
| lwIP `tcpip` | ESP-IDF | 18 | any | — | |

* **Known defect, row hvz8:** the reply state `porta_rx` writes and HTTP handlers read is not fully under `s_lock`, so a handler can read the previous reply's result.
* **Not yet built: row wgjx** — pin the latency-sensitive work (main loop, `porta_rx`) to one core and the bursty work (HTTP, DNS, scanning) to the other. After hvz8, since pinning would make that race certain.
