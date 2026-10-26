# RFC 0013: PSKReporter — who hears us, and reporting what we hear

* **Status:** Draft 2026-10-08. Decided in discussion on 2026-10-07: §8. The Ask: §7.
* **Author / Lead:** Jeff Kalikstein, KB2SLO
* **Covers:** showing which stations heard our transmissions, at home and on an activation, including one made where the phone has no signal ([ROADMAP.md](../ROADMAP.md) I37); sending our own decodes to PSKReporter as reception reports (f4kb).
* **Does not cover:** self-spotting to POTA or SOTA, a different risk class ([webapp.md](../architecture/webapp.md), **Tools**); other spot networks such as the Reverse Beacon Network.
* **Rows:** [dmyu](../ROADMAP.md) (this RFC), I37, f4kb.

---

## 1. Goal

1. **Who hears me (I37).** After a CQ, the operator sees the stations that heard it, with band, SNR and distance, on a Tools card and on the map. On a summit with no cell signal, they see the same list once they are back online.
2. **Report what we hear (f4kb).** With an opt-in setting on, our decodes appear on pskreporter.info with our call as the receiver, as WSJT-X's do.

**Constraints:**
* [system.md](../architecture/system.md)'s inversion: third parties are reached by the phone, never by the device, and the sidekick never speaks TLS.
* Nothing here may become something the station needs in order to operate.
* PSKReporter's own usage rules (§2).

## 2. What exists

Facts are from the working tree at `f4618e6` and from measurements made on 2026-10-07.

**In our code:**
* **A Worker route, `/pskreporter`,** wired at `webapp/worker.js:16` and implemented at `:126`. It runs one query per request for `senderCallsign`, the last hour, FT8 only (`:139`–`:141`), and returns only the `<receptionReport>` elements, as small JSON with CORS (`:214`).
  - It does not set `noactive`. That is the likely reason one query measured 1.25 MB, almost all of it a list of every active receiver on the network (`:113` comment).
  - **Nothing calls it.** The map's PSKReporter layer button is disabled ("Not built yet", `webapp/app/map.html:99`), and the site lists PSKReporter under Coming soon (`webapp/index.html:802`).
* **The app's origin is not a secure context** ([webapp.md](../architecture/webapp.md), **A non-secure origin**). `localStorage` and `fetch` to HTTPS work. The **Screen Wake Lock API needs a secure context, so the app cannot keep the screen on.** With no Service Worker, the app cannot be opened while the sidekick is unreachable.
* **The station call** is set in Settings ([webapp.md](../architecture/webapp.md), **The Station sheet**).
* **The phone keeps cellular while joined to the sidekick's AP** (iPhone, [system.md](../architecture/system.md), **Connectivity**). The sidekick has internet only on home WiFi or the phone's hotspot.

**PSKReporter offers three interfaces:**
* **A live MQTT feed** at `mqtt.pskreporter.info`, run by Tom M0LTE with Philip Gladstone's permission ([its page](https://www.mqtt.pskreporter.info/)).
  - Ports: 1883 plain TCP, 1884 TLS, 1885 WebSocket, 1886 WebSocket over TLS.
  - Topics: `pskr/filter/v2/{band}/{mode}/{tx_call}/{rx_call}/{tx_grid}/{rx_grid}/{tx_dxcc}/{rx_dxcc}`.
  - Each message is one spot as JSON: `sc`/`sl` sender call and grid, `rc`/`rl` receiver call and grid, `f` Hz, `rp` SNR, `b` band, `md` mode, and `t_tx`, the start of the transmission.
* **A query interface,** `retrieve.pskreporter.info/query` ([pskdev.html](https://pskreporter.info/pskdev.html)).
  - **No CORS header,** which is why the Worker route exists ([system.md](../architecture/system.md), **Third-party services**).
  - **How far back is unclear.** By default it returns "the last 100 reception records… no more than 6 hours", yet `flowStartSeconds` may reach back 24 hours. `rptlimit` is described only as "Limit the number of records returned".
  - **Usage rules:** "no more often than once every five minutes". It suggests a transmitting app query five minutes after a transmission, so that every receiver has reported. It asks frequent users to identify themselves with `appcontact`.
* **Ingest: IPFIX messages to `report.pskreporter.info:4739`.**
  - **UDP or TCP.** pskdev.html describes only UDP. **WSJT-X also sends over TCP** when its option is on (`use_tcpip` in `Network/PSKReporter.cpp`, WSJTX/wsjtx master, checked 2026-10-08), to the same host and port.
  - **Reachable from a Worker.** `report.pskreporter.info` resolves to 74.116.41.13, which is not a Cloudflare address, so a Worker's `connect()` can reach it.
  - **The test listener is not.** It is at `pskreporter.info:14739`, which now resolves to Cloudflare addresses, and a Worker cannot connect to those.
  - **Pacing:** at most one message every five minutes unless it is full, not synchronised to the clock, and each callsign at most once per five minutes.
  - **Required fields:** `informationSource` must be 1. `decodingSoftware` should name the program and its version.
  - **The clock check compares the message header's send time with its arrival.** More than a minute apart means the sender's clock is wrong, and every time in the message is shifted. Each report carries its own time (`flowStartSeconds`, when the call was decoded), so this check alone does not rule out sending reports late. **Whether hours-old reports are accepted is not documented** (Ask 1.3).

**Measured on the MQTT feed, 2026-10-07.** QoS 0, no retained messages, no persistent-session backfill, a ping needed inside about 90 s, and the lag from transmission to arrival are in [webapp.md](../architecture/webapp.md) (**Who hears me**), which depends on them. Also measured: **spots per transmission** on 20 m FT8 (642 transmissions) were median 1, 75th percentile 3, 90th 5, 99th 15, most 77, a slight undercount since the run stopped before the slowest arrived. **Port 1884's certificate** is a Let's Encrypt certificate for `mqtt.pskreporter.info`.

**Philip Gladstone's reply (2026-09-24):** his concern is load. When many people use a service, its queries can overwhelm the query backend, which he is working to speed up. He points people at the MQTT feed, which is built to scale. The reply neither allows nor rules out the capped gap-fill in §3.2. A follow-up email, sent 2026-10-08, asks him directly (Ask 1).

## 3. Design

### 3.1 Who hears me: live feed in the browser

**Built 2026-10-08** (S1's live half): [webapp.md](../architecture/webapp.md), **Who hears me**. It listens from a transmission until 15 minutes after the last one, and while Tools is on screen, as Ask 7 recommended.

### 3.2 Gaps: one history query each

**A gap is any time the app should have been listening and was not:** the page hidden, the phone locked, no network, or the socket dropped. Gaps are the normal case: spots arrive up to nine minutes after a transmission, and the app cannot keep the screen on (§2).

* **The app tracks gaps, and shows them** (built with S1's live half, [webapp.md](../architecture/webapp.md), **Who hears me**). Filling them, below, is not built.
* **It fills each gap once the gap ends.** It asks the Worker route for our call's reports covering the gap, waiting until five minutes after the last transmission inside it.
* **It also fills on load.** When the page loads, one query covers the history window, so opening the app after an activation shows it.
* **History and live spots merge into one list,** keyed on receiver call, band and slot.
* **The card says what each row came from.** A filled gap shows as "Not listening 14:10–14:25 · filled from PSKReporter's history". One that could not be filled shows the reason: offline, the 6-hour window passed, or PSKReporter's record limit was reached.

### 3.3 The Worker route

The route stays. I37's Done-when deletes it; this reverses that.

* **New query parameters:** `noactive=1` to drop the network-wide receiver list; a `since` parameter capped at PSKReporter's window (Ask 1.2); FT4 as well as FT8; `appcontact` (Ask 6); and `rptlimit` if Philip allows.
* **At most one upstream query per call per five minutes.** The Worker keeps each answer in the Cloudflare Cache API, keyed by call and window. Reloads, several phones, or someone hammering one call all get the cached answer.
* **A ceiling across all users, and an off switch.** The Worker counts upstream queries and stops at an hourly ceiling Philip chooses. A Worker variable turns gap-fill off at once if he asks. Either way, the card says history is unavailable.
* **The response adds** the window it covers, and whether it hit the record limit, so the card can say so.
* **The cost** is a few Worker requests per session, against 100,000 a day on the free plan.
* **One more point for Ask 1:** every operator's queries leave from Cloudflare's shared addresses, so PSKReporter sees what looks like one client.

### 3.4 The card and the layer

**Built 2026-10-08** (S1's live half and S2): [webapp.md](../architecture/webapp.md), **Who hears me** and **The map**. The map is the main view; the list is a Tools sub-page (§8).

### 3.5 After an activation, without the sidekick

The app cannot load once the sidekick is packed away (§2). A page on the release host, `webapp/heard.html` at `https://micro-ft8.kalikstein.com/heard.html`, would show the same history for a call using only the phone. It is a static page like `geo.html`, works on the way down from a summit, and as a secure origin it may keep the screen on. Its only data is the query window. Whether to build it is Ask 3.

### 3.6 Reporting what we hear (f4kb)

**The browser sends through the Worker, over TCP.** A browser cannot open a TCP or UDP connection, but a Worker can open TCP, and PSKReporter takes TCP (§2).

* **The browser batches.** It already receives every decode it shows. About every five minutes, randomised and not on the clock, it posts a small JSON batch to a new Worker route:
  - the receiver: our call, grid and app version
  - one entry per decoded call: call, grid when the message carried one, frequency (dial plus audio offset), SNR, mode, and time
* **The Worker builds the IPFIX message itself, then sends it.** It validates the batch, opens TCP to `report.pskreporter.info:4739` with `connect()`, writes the message and closes. Because the Worker builds every byte, only well-formed reports carrying our `decodingSoftware` ("Micro-FT8" plus the version) and `informationSource` 1 can leave through it.
* **Pacing.** The Worker sends at most one message per receiver call every five minutes. The browser sends each decoded call at most once per five minutes.
* **This keeps the inversion:** the phone reaches the third party, through our Worker. The Worker gains a new kind of route, one that speaks a protocol the browser cannot (§6).
* **Coverage is what the page saw.**
  - A phone that is locked, or a page that is closed, reports nothing for that time.
  - At home, a computer with the page left open should cover a whole session. Whether a background tab keeps receiving decodes is checked in S4.
  - If PSKReporter accepts hours-old reports (Ask 1.3), batches made on a summit with no signal wait in `localStorage` and go out once the page is back online. Otherwise they are dropped.
* **Cost:** one Worker request per operator every five minutes, 288 a day.
* **Opt-in:** a Settings switch, off by default. Turning it on publishes the station's call and grid as a receiver.
* **The contract is PSKReporter's format.** A host test holds the Worker's encoder to a golden message: pskdev.html's byte layout, or a capture of WSJT-X sending over TCP. The test listener cannot be reached from a Worker (§2).
* **If reporting while the phone is locked matters,** the sidekick can send the same reports over UDP (§4). That is a later slice and a second exception to the inversion.

## 4. Alternatives

* **The browser alone, with no history query.** Everything sent while the phone is locked or offline is lost, which is most of an activation. The app cannot keep the screen on. And a CQ's spots arrive up to nine minutes later.
* **A persistent MQTT session to fill gaps.** Measured not to work (§2).
* **Our own recorder:** a Cloudflare Durable Object holding one MQTT connection (TLS on 1884 through Workers' `connect()`), with one subscription per registered call and spots in its SQLite storage.
  - **Free-plan costs** ([pricing](https://developers.cloudflare.com/durable-objects/platform/pricing/)):
    - Duration is billed at 128 MB while loaded, and an open socket keeps it loaded. Always on, that is 10,800 GB-s a day of the 13,000 included, and it does not grow with the number of calls.
    - About one database row per spot, against 100,000 a day.
    - A wake-up every minute is 1,440 requests a day.
    - Over a limit, the operation fails; nothing is billed.
  - **What it gains:** no history window, no record limit, readable any time later, and no reliance on the query interface.
  - **What it costs:**
    - our first service that stores data
    - a public endpoint to cap against abuse
    - a recording that must be started before signal is lost
    - failures found on a summit
  - **Two behaviours need a prototype:**
    - An open socket keeps the object loaded for only 15 minutes; after that it can be evicted after 70–140 s idle ([lifecycle](https://developers.cloudflare.com/durable-objects/concepts/durable-object-lifecycle/)), so it needs a wake-up each minute.
    - How socket reads count against the 30 s CPU budget is not documented.
  - **This is the fallback** if Ask 1.1 is no or the 6-hour window proves too short (Ask 5).
* **The sidekick subscribes (I37).** It has no internet in AP mode, so it gets nothing on a summit, and it costs flash and DIRAM. It breaks the inversion for ground the browser already covers.
* **Polling the query interface alone.** This is the use Philip steered us away from, and it is slower than the feed.
* **f4kb from the sidekick, over UDP.** The sidekick sends only when the operator has opted in and it has internet: home WiFi, or the phone's hotspot. It sends nothing in AP mode.
  - **What it gains:** every decode the sidekick relays is reported, whether or not the phone is locked. UDP needs no TLS, so "the sidekick never speaks TLS" holds.
  - **What it costs:**
    - a firmware encoder, and a table of recently reported calls (flash and DIRAM to be measured; the S3 has no PSRAM, [sidekick.md](../architecture/sidekick.md), **Hardware**)
    - **a second named exception** to "third parties are reached by the phone, never by the device", beside the firmware fetch
  - **This is a later slice** (S5), worth it only if reporting while the phone is locked matters (Ask 4).
* **f4kb from a UDP relay host of our own.** No longer needed now that TCP is accepted, and it would be a server we run.
* **f4kb from the P4 over Ethernet.** It works at home only, and the P4's Ethernet is unused by decision ([main_board.md](../architecture/main_board.md)). [RFC 0012](0012-home-decode-server.md), a draft, proposes reversing that for the LAN.

## 5. Slices

Each slice states which verifiability category from [AGENTS.md](../../AGENTS.md) applies.

0. **S0: ask Philip, and tell M0LTE** (Ask 1). No code. S1's gap-fill waits on the answer; its live half does not.
1. **S1 (I37): the card, the live feed, gap-fill, and the route changes.**
   - Verification: host tests for the MQTT packet code and the gap and merge logic (category 1). Field: a CQ from the station, with the phone locked through it (category 2).
   - **Done-when:** a CQ shows its receiving stations with band, SNR and distance within minutes; a ten-minute lock is filled after unlocking; offline, the card says so.
2. **S2: the map layer.** Built 2026-10-08 with S1's live half. Category 1 for the listener (`test_pskr_live.js`); the operator looks at the drawing.
3. **S3: `heard.html` on the release host,** if Ask 3 is yes. Graduates in the next commit, once the Pages and Worker deploy is green ([README.md](README.md), **RFC lifecycle**).
4. **S4 (f4kb): batches from the browser, and the Worker's TCP sender, opt-in.** No firmware.
   - Verification: a host test of the Worker's encoder against the golden message, and of the browser's batching and pacing (category 1). Field: with the option on, a session's decodes appear on pskreporter.info with our call as the receiver (category 2). The Worker half graduates once its deploy is green.
5. **S5 (f4kb, optional): the sidekick's UDP sender** (§4), only if Ask 4 chooses it. This is a firmware slice.

## 6. What changes elsewhere on acceptance

* **[ROADMAP.md](../ROADMAP.md):**
  - I37 and f4kb are reduced to pointers at §3.1–3.5 and §3.6.
  - I37's Done-when stops deleting the route.
  - I37's "§12.2" citation, which points at no section, goes.
  - dmyu drops.
* **[system.md](../architecture/system.md), Third-party services:**
  - S1 changes nothing there: the query interface still has no CORS.
  - S4 adds a second reason for a Worker route: a protocol the browser cannot speak. The route builds the message itself rather than passing bytes through.
  - S5, if built, adds the sidekick's UDP reports as a named exception beside the firmware fetch.
* **[webapp.md](../architecture/webapp.md), Tools:** the card's tier and sources, with S1. The map section gains the layer, with S2.
* **The site:** `webapp/index.html`'s Coming soon line moves to what ships. The [README.md](../../README.md) fork notes are audited with S1.

## 7. Open questions — the Ask

1. **For Philip** (S0). If his 2026-09-24 reply already answers these, pasting it settles them.
   1. Is a history query after a gap acceptable? At most one per call per five minutes, cached by our Worker, from Cloudflare's shared addresses, carrying `appcontact`.
   2. How far back does a query reach, 6 hours or 24? Can `rptlimit` raise the default 100?
   3. For f4kb: may our Worker send reports over TCP for many receivers, from Cloudflare's shared addresses? Are reports hours old accepted, so a summit's decodes can go out on the way down?
2. **Accept the design:** the live feed in the browser, the history query for gaps, and the route kept?
3. **Build `heard.html`** (S3), so an activation can be looked at without the sidekick?
4. **f4kb: who sends.** Recommended: the browser through the Worker over TCP (S4), which keeps the inversion and reports what the page sees. Or add the sidekick over UDP (S5) as well: it also reports while the phone is locked, but needs firmware and a second exception to the inversion.
5. **Keep our own recorder (§4) as the fallback** if 1.1 is no or the window proves too short?
6. **`appcontact`:** which address?
7. **When the browser connects.** Either whenever the page is open with a call set (simplest; an idle connection costs a ping a minute), or from the first transmission of the session (fewer connections to M0LTE's broker). Recommended: from the first transmission, or when the card or layer opens. Built that way on 2026-10-08, pending your answer.

## 8. Decisions

* **2026-10-07:** subscribe to the station's own call only, never the whole feed (operator).
* **2026-10-07:** a persistent MQTT session is not a gap-fill: measured, §2.
* **2026-10-08:** build S1's live half now; the gap-fill waits on Philip's answer (operator).
* **2026-10-08:** the map is the main view of who hears us; the list moves off the Tools card to its own sub-page, built only when opened (operator).
* **2026-10-08:** the summary names the bands heard on and the farthest station; the list filters by band and sorts by time, distance or dB; every call card shows what PSKReporter has of that station hearing us (operator).
