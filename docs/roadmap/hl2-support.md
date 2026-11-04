# HL2 support (epic knpp)

The Hermes-Lite 2 (HL2) as a second radio on the P4, beside the QMX: receive, then transmit, at home, then in the park. Rows: [mg9c](#mg9c), [abkw](#abkw), [kgyh](#kgyh) ([ROADMAP.md](../ROADMAP.md)). Several HL2 bands at once is [RFC 0016](../rfcs/0016-decode-scheduler.md) S6, after [mg9c](#mg9c).

## Goal

Operate FT8 from an iPhone anywhere in the house, asynchronously, on the home station (HL2, N2ADR filter board, antenna), and the same station in the park for POTA. The HL2 is the station's several-band radio: up to four receivers, each a source of channels that RFC 0016 decodes and operates.

## Constraints

* **One codebase, one main board.** The P4 stays the only main board ([main_board.md](../architecture/main_board.md)); the HL2 is a second `radio_control_ops_t` backend, and the sidekick and web app serve the phone as they do today.
* **The QMX station is unchanged.** A P4 running the QMX decodes and transmits exactly as today; the recall floors and `tx_e2e` goldens hold.
* **Offline in the park** ([system.md](../architecture/system.md), constraint 1): everything runs over the sidekick's AP.
* **Each QSO on the air is a WSJT-X QSO** ([AGENTS.md](../../AGENTS.md), **WSJT-X is the on-air standard**).
* **Transmit ships off,** behind **Safety** below.
* **Our own code.** Written from the protocol documents and the gateware's behaviour; nothing copied from Quisk (GPL) or another client. Their formulas are facts we cite.
* **Out of scope:** an amplifier (the station's, not this backend's); SOTA (on a summit the QMX station wins on weight and cabling); a host main board on a computer.

## Decided

* **Feature parity with the QMX station is the goal.** Every operator-facing feature works on the HL2 as it does on the QMX, or this file says why not (**Parity**). A feature that differs only because the HL2 has no front panel is not a gap.
* **One image, both radios, chosen live** in Settings › Radio (built: [radio.md](../architecture/radio.md), **How it works**). Both stacks stay installed once started (Ethernet's DMA buffers are about 15 KB of heap) and a change only moves which one is active: no teardown, no restart, and no reset that can wedge a QMX left plugged in. A change is refused while anything is keyed, so the radio that keys is the one that un-keys. Until the HL2 has a transmit backend, choosing it leaves the station's controls on a no-radio backend, so a QMX still plugged in cannot key. A reply transmits on the radio that heard it, never another; one radio is active at a time.
* **A private link:** one Ethernet cable from the P4 to the HL2, nothing else on it. The P4 does not route and the sidekick has no Ethernet, so the home LAN cannot reach the radio.
* **Static link-local addresses:** each P4 takes 169.254.MAC[15:8].MAC[7:0] from its own MAC, as the HL2 derives its own (169.254.0.x and 169.254.255.x moved in by one), with no conflict probe; if it equals the HL2's, the P4 does not stream and says why. Discovery takes whatever address answers (built: [main_board.md](../architecture/main_board.md)). **No DHCP server on a P4:** an HL2 that boots first falls back to link-local anyway, a server plugged into a home LAN by mistake would break it, and 74.2's DHCP client failed once for reasons not found; revisit only if link-local fails on the bench. **The internet is out of scope:** the P4 sits beside the radio, and a VPN looks like a LAN (**Open**).
* **Gateware 74.2,** the standard `hl2b5up_main` (four receivers and a transmitter). 74.3 changes only DHCP and address handling, and exists only as a testing release in the repository (`74p3`, `f0247d4`), not on the Releases page.
* **ft8_lib only** decodes; recorded HL2 slots are scored offline against `jt9` with `tools/decode_bench/compare_decoders.py`, which keeps the gap (about 30% of `jt9`'s decodes) measured.

## Design

### Backend

* **The codec,** `components/hl2_proto` (name provisional): discovery, frames, control registers, 24-bit receive and 16-bit transmit packing, sequence numbers, telemetry. Host-tested in `host_mock`; its contract is its header plus golden frames recorded from the operator's HL2, starting with the discovery reply under **Facts**.
* **Ethernet on the P4:** the EMAC, the IP101 PHY and lwIP, one UDP socket. One receiver is 381 packets/s in, four are 1,263; transmit adds 381 out. The EMAC's DMA buffers stay internal. Linking Ethernet, `esp_netif`, lwIP and a socket adds about 2.7 KB of static DRAM and 2.1 KB of internal-RAM code (measured on `2f2caab` by addition, not built), inside today's budget; Ethernet's DMA buffers take about 15 KB of heap when the stack starts. Using the PHY reverses main_board.md's 2026-09-16 "not used".
* **Radio control:** a new `radio_control_ops_t` table and capabilities ([radio.md](../architecture/radio.md), **Adding a radio**). The tone lead becomes its own property instead of following `begin_cpfsk_tx` (`radio_control.cpp:102`): the HL2 takes symbols itself but needs the PTT lead for its own T/R relay.
* **The web app** changes only where the radio does: its name, and the HL2's telemetry where the radio bar shows the QMX's.

### Receive

* **Built (row [mg9c](#mg9c)):** one receiver through its own front-end instance, the NCO on the dial so the channel is 1,500 Hz, Q negated ([decode.md](../architecture/decode.md), **The HL2's channel**); a half-second ring in PSRAM; the N2ADR's low-pass for the band and its receive high-pass, and the LNA stepped down while the ADC clips ([main_board.md](../architecture/main_board.md)). The front end costs 1.16 s of core 0 a slot, as on the QMX.
* **Sequence gaps are zero-filled** with the known number of samples (built), so the timeline and each block's date hold, and the slot is marked unclean. Blocks are dated by their audio, as today; the HL2's sample clock is not UTC.
* **Per-slot counters** on SLOT and BDRY: sequence gaps, samples zero-filled, ring drops, ADC overload, and on transmit the TX FIFO's recovery flag and count (address 0's C3).
* **As a source of several channels** (RFC 0016 S6): four receivers, each on its own band; FT8 and FT4 on one band are two front-end instances on one receiver. The N2ADR receive filter serves every receiver, so it is set for the highest band listened to (as Quisk does), and ADC overload is counted per slot and reported as the source's reason when it rises.

### Transmit

* **One transmission at a time,** on the channel RFC 0016 chooses. It sets the HL2's TX NCO and the N2ADR transmit filter for that channel; no receiver moves.
* **The waveform:** GFSK, BT 2.0 for FT8 and 1.0 for FT4, first and last symbols ramped, as ft8_lib's `demo/gen_ft8.c` and WSJT-X make it. Complex baseband at 48 kHz, offset by the audio offset from a TX NCO at the dial, written ahead into the HL2's transmit buffer. The first tone at the slot start plus 0.5 s, as on the QMX ([radio.md](../architecture/radio.md), **TX timing on a QMX**). ft8_lib's encoder is proved against `ft8code` (row [I40](../ROADMAP.md#I40)) before anything transmits.
* **PTT before RF:** PTT rides every transmit sample, so the P4 keys with silent samples for the Min TX delay before the first tone, giving the HL2's T/R relay time to switch.
* **The filter before every key-down:** the N2ADR transmit LPF for the band, then the receive byte again afterwards.

### Safety

1. **Transmit is off by default,** enabled on this hardware only after the bench procedure (8) has passed and is recorded in [TEST_PLAN.md](../TEST_PLAN.md).
2. **SWR protection in the client.** Γ = √(P_rev / P_fwd), forward the larger reading. Evaluated only above a minimum forward power (Quisk uses 0.05 W); K consecutive readings over the limit trip it; it latches transmit off until the operator resets it in the web app. Power from a calibration table, Quisk's `HL2FilterE3` until this unit is calibrated.
3. **A drive cap:** register 0x09 [31:28] has 16 steps; a compile-time maximum clamps every setting.
4. **Never sent while transmit is enabled:** a PTT hang of 31, or either watchdog-disable bit. The codec refuses to encode them, host-tested.
5. **Stream loss** relies on the gateware's two dead-man layers (**Facts**), and the bench proves them: pull the cable and reset the P4 mid-transmission into a dummy load; record how long RF takes to stop, and the watchdog's period.
6. **A runaway transmitter** is bounded by row [u4dw](../ROADMAP.md#u4dw)'s cap.
7. **A filter write that fails stops the transmission.**
8. **The bench procedure:** a dummy load through an attenuator; occupied bandwidth against WSJT-X's; harmonics and spurs through each band's LPF; RF start and stop against the slot boundary.
9. **A frequency outside the allowed band is refused,** by RFC 0016's rule for every backend: before key-down the backend checks the TX NCO plus the audio offset, and the signal's bandwidth, against the band table.
10. **A gap in transmit samples ends the transmission.** The P4 never fills a gap with samples carrying PTT: that would hold the transmitter keyed and defeat the gateware's first dead-man layer. If the writer falls behind by more than a set time, or the TX FIFO's recovery flag (address 0's C3) reports it ran dry, the transmission stops, PTT drops, and the slot is counted and marked unclean. Host-tested against a stalled writer. No browser heartbeat: transmission is autonomous by design, so the station-side bound is item 6.
11. **Who may key it:** the pairing token guards our API, not the radio. Protocol 1 has no authentication, so anything that can send the HL2 a packet can stop, start or key it, and anything that restarts it takes our stream (seen as stream loss). Hence the private link; the phone's path stays through the sidekick, behind the token ([system.md](../architecture/system.md), **Who may key the transmitter**).

## Parity

Every feature the QMX station has, against what the HL2 needs for it. **Fits**: the design above covers it. **Work**: a known build with a named challenge. **Open**: feasibility not yet known. **None**: nothing to build, and why.

| Feature (QMX today) | On the HL2 | |
|---|---|---|
| Decode FT8 from I/Q through the front end | One receiver through the same front end (**Receive**). | Fits ([mg9c](#mg9c)) |
| FT4 (row [xph7](../ROADMAP.md#xph7), both radios) | Same pipeline; on transmit, BT 1.0. | Fits |
| Transmit: the QMX makes the tones from `TA` | The P4 makes the waveform and streams it, 381 packets/s on core 0, with no gap allowed (Safety 10). Core 0's headroom while transmitting is unmeasured. | Work ([abkw](#abkw)) |
| Band change: `FA`, and the QMX's own filters | NCO plus the N2ADR byte, both ours, re-chosen on every band change; its relays click. The receive filter is shared once several receivers run (**Receive**). | Fits (built) |
| Power and SWR while transmitting (`PC;SW;`, slot bar) | Forward and reverse telemetry through a calibration table. Which word is forward on this unit, and how often telemetry refreshes while keyed, are unread; until calibrated, the watts are Quisk's table, not this unit's. | Work ([abkw](#abkw)) |
| SWR protection: the QMX's own latch, which the P4 reads (`SR;`, `amp_swr_guard`) | The HL2 has no latch of its own that we found, so the client latch (Safety 2) is the only one. Its reaction time is set by the telemetry rate and must be measured on the bench, not assumed. | Work ([abkw](#abkw)) |
| Tune: SWR Tune (`MD8;`) at the QMX's Tune %, SWR shown, ended by a trip | A carrier from the P4 at a Tune drive level of ours (the HL2 has no Tune %), SWR from telemetry, ended by the client latch: row [bfkb](../ROADMAP.md#bfkb)'s stop comes built in. A continuous carrier needs row [u4dw](../ROADMAP.md#u4dw)'s cap and the stream-loss dead-man. At low drive the forward power may be under the latch's minimum, so the latch may not see a bad antenna. | Work (later slice) |
| SWR sweep: per point `FA`, `MD8;`, `SW;`; the QMX refuses frequencies outside its configured bands | Per point: NCO, the band's LPF, a short carrier at sweep drive, telemetry, unkey; or stay keyed across a band and move the NCO, if the HL2 retunes while keyed. Unknown: telemetry settling time after keying or retuning, and T/R relay wear from keying per point. `radio_swr_io_t` is QMX-shaped (`MD8`, `SW`) and becomes "carrier at f, read SWR". The band table (Safety 9) replaces the QMX's own refusal of out-of-band points. Row [vu8t](../ROADMAP.md#vu8t)'s Tune-% label becomes our sweep drive. | Open (later slice) |
| Prime an RF-sensing amplifier on a band change | Out of scope with the amplifier. If it returns, it is a 500 ms Tune. | None |
| Radio bar: **CAT** and **Audio** ✓/✕ | The HL2's chip states (no link, silent, busy, found, streaming, lost; **Receive only**) and the slot bar's receive cues ([webapp.md](../architecture/webapp.md), **The slot bar's receive cues**). Getting started quotes **CAT** and **Audio**, so the site audit applies when this ships. | Fits ([mg9c](#mg9c), built) |
| **Radio firmware** in the drawer (`VN;`), and the warning before 1.04.010 | The gateware version from the discovery reply is logged; not yet in the drawer, and no warning off 74.2. | Work (Later) |
| Read-back and pushed state (`IF;`, `AI2;`): **Put it back** / **Use it** when the radio moved | No front panel, so the HL2 is only where we set it. Its "radio moved" is another client holding the stream (Protocol 1), shown as **busy**; a lost stream is **Stream lost**. | None; the fault is built |
| Setting the radio's clock (`can_set_time`) | The HL2 has no clock. | None |
| The IQ-mode probe at link-up (`Q9`) | The HL2 is always I/Q. | None |
| Bands offered from the radio (row [vcng](../ROADMAP.md#vcng): `OM;`, `VN;`, its band table) | The HL2 covers 160–10 m. Which bands may transmit follows the N2ADR filters, and whether the board can be detected (the MCP23008 read-back, Open) is unknown; if not, it is a setting. Listen-only bands per radio: row [mtd6](../ROADMAP.md#mtd6). | Open |
| Per-radio settings (row [mtd6](../ROADMAP.md#mtd6)) | Identity is the MAC from discovery. | Fits |
| Receive gain: none to set on a QMX | The LNA starts at +20 dB and steps down 3 dB a second while the ADC clips, never back up within a stream; on 80 m at night it settled at +11 dB. Whether it should ever step back up is open. | Fits (built) |
| Frequency accuracy: the QMX's own reference | The HL2's reference sets every NCO. An error of a few ppm is tens of Hz on 10 m, within FT8's search but visible in `df`; unmeasured. A correction is per radio (row [mtd6](../ROADMAP.md#mtd6)) if one is needed. | Open (Later) |
| Listen on the phone (row [fc6w](../ROADMAP.md#fc6w)): the QMX demodulates | The front end already makes upper-sideband audio, so listening on the FT8 window is nearly free; other modes need demodulators of ours. | Work ([fc6w](../ROADMAP.md#fc6w)) |
| Field power from one bank | 12 V for the HL2; its receive and transmit current unmeasured. | Work ([kgyh](#kgyh)) |
| TX watchdog (row [u4dw](../ROADMAP.md#u4dw)), AP decoding (row [qauk](../ROADMAP.md#qauk)), autoseq, the log | Independent of the radio; the log names the HL2. | Fits |
| The simulator: a fake QMX behind `sim_station` | The codec is held by golden frames from this unit (built). A fake HL2 behind `sim_station`, so receive, transmit and safety run in `host_mock` end to end, is not built. `radio-utils-emu` stays a smoke test, not a model. | Work (Later) |
| Heat: the QMX's own business | The HL2 reports its PA temperature. FT8 keys about half the time, so a temperature cutoff becomes a safety item, its limit to be set from the bench. | Work ([abkw](#abkw)) |

**Lands in:** each row with the slice named in its last column. When the epic ends, every row still None or not at parity becomes a **Differences from the QMX** list in `hl2.md`, one line each with its reason (no Tune %, no clock, no front panel), so a reader does not file them as bugs.

## Children

* <a id="mg9c"></a>**mg9c: receive, one band** (Now). Built; its field steps are owed in TEST_PLAN. The codec; the radio chosen live; Ethernet with a link-local address; discovery; one receiver through the front end as one channel; golden frames; telemetry; `radio-utils-emu` as a CI smoke test. Codec host tests (category 1); the rest field (category 2). **Done-when:** an iPhone on home WiFi shows the HL2's decodes on Operate through the sidekick; golden frames committed with a codec test; the watchdog's period and the first-contact readings (**Facts**) recorded in the HL2's architecture doc; recorded HL2 slots scored against `jt9` (`tools/hl2_record.py`; the band survey of 2026-11-04); the QMX build within its RAM budget, its recall floors unchanged, and the QMX station still decodes and transmits (field). **Lands in:** a new `architecture/hl2.md`, created by this child (codec, link, discovery, receive, the facts); [radio.md](../architecture/radio.md) (the backend, the live choice); [decode.md](../architecture/decode.md) (the source, the SLOT and BDRY counters, the `jt9` scores); [webapp.md](../architecture/webapp.md) (the radio's name and what the radio bar shows for it); [main_board.md](../architecture/main_board.md) (the PHY now used), [memory.md](../architecture/memory.md) (its static and heap cost), [system.md](../architecture/system.md) (the private link); README, the HL2 as a supported radio.
* <a id="abkw"></a>**abkw: transmit, barefoot.** After [mg9c](#mg9c) and row [I40](../ROADMAP.md#I40). Waveform, PTT lead, filter, drive cap, SWR latch, stream loss, the band table, transmit off by default. The safety policy host-tested (category 1); everything RF on the bench (category 2). **Done-when:** which telemetry word is forward on this unit, read while transmitting; the bench procedure (Safety, 8) passes into a dummy load and is recorded in TEST_PLAN; then a QSO on the air at 5 W (field). **Lands in:** `hl2.md` (transmit, and a **Safety** section holding each item as built with its bench result and host test; items 6 and 9 land with row [u4dw](../ROADMAP.md#u4dw) and RFC 0016); radio.md (the tone lead, TX timing beside the QMX's); system.md (who may key it); webapp.md (power and SWR for the HL2); TEST_PLAN (the bench procedure's result); the site, what is verified on the air.
* <a id="kgyh"></a>**kgyh: in the park.** After [abkw](#abkw). AP mode with the HL2, field power measured, listen-only bands (row [mtd6](../ROADMAP.md#mtd6) holds them per radio). **Done-when:** a POTA activation on the HL2 station, with the power draw recorded (field). **Lands in:** main_board.md (**Field power** with the HL2); `hl2.md` (its current draw).

**Later** (slices not yet rows; grooming makes each one a row when it is next):

* **A combined bench run:** the P4 decodes the HL2 while a laptop bridged into the cable (or on a managed switch's mirror port) captures the same packets, so the P4's decodes and `jt9`'s are compared on identical slots, beside the P4's own network and CPU counters. No firmware change: the pcap becomes slot WAVs through `tools/hl2_record.py`'s parser and `qmx_record.py`'s SlotWriter. A laptop on the link breaks the private link while it is there, so receive only. Lands in `hl2.md` and the corpus's README.
* **Tune on the HL2,** after [abkw](#abkw) (**Parity**). Lands in radio.md (Tune beside **QMX SWR Tune over CAT**) and webapp.md, if the drive becomes a setting.
* **The SWR sweep on the HL2,** after Tune, once the settling and keyed-retune questions are answered. Lands in radio.md (**The SWR sweep on the main board**, with `radio_swr_io_t` generalised) and webapp.md (the drive in place of the Tune % label).
* **The gateware version in the drawer,** with a warning off 74.2 (**Parity**). Lands in webapp.md.
* **Frequency accuracy** measured, and a per-radio correction if it is needed (**Parity**). Lands in `hl2.md`.
* **A fake HL2 in the simulator** (**Parity**). Lands in the simulator's own docs and TEST_PLAN's Automated table.
* **Which bands transmit:** the N2ADR filters detected, or set (**Parity**, bands offered). Lands in `hl2.md`, and webapp.md where Settings offers the bands.

## Open

* **Discovery while running:** the source does not gate discovery on `run`, but a broadcast while the HL2 streamed got no reply, with SparkSDR and again with no other client.
* **LAN operation:** the HL2 on a home LAN, the P4 taking a DHCP lease first and link-local after. Not yet run on the bench: the P4 and the HL2 through a switch rather than the direct cable. Transmit only on a private link, or on a LAN too? Protocol 1 has no authentication (Safety 11), so anything on the LAN could key the radio.
* **A QMX and an HL2 at once:** channels from each (RFC 0016), all decoding, a reply on any of them transmitted by the radio whose channel heard it. Needs a queue entry carrying its radio as well as its band, and one backend per radio in place of one selected backend.
* **Whether the LNA steps back up** within a stream when the band quiets.
* **Whether the MCP23008 reads back** over the HL2's I2C read, for Safety 7 ([abkw](#abkw)).
* **Bench instruments** for Safety 8: dummy load, attenuator, a second receiver or a spectrum analyser ([abkw](#abkw)).
* **The HL2's enclosure and I/O board** (before [kgyh](#kgyh)).
* **How the operator marks a band listen-only** for the antenna in use ([kgyh](#kgyh), with [mtd6](../ROADMAP.md#mtd6)).
* **Telemetry while keyed:** how often forward and reverse power refresh, which sets the client latch's reaction time ([abkw](#abkw)).
* **Retuning while keyed,** and telemetry settling after a key or retune (the SWR sweep).
* **Tune's drive level** against the latch's minimum forward power (Tune).
* **A temperature limit** for the PA ([abkw](#abkw)).

**Lands in:** each answer, as a measured fact with its source, in `hl2.md` with the child that settles it; an answer that changes the design edits **Design** here first.

## Facts

Read from the [HL2 wiki](https://github.com/softerhardware/Hermes-Lite2/wiki), the gateware (`softerhardware/Hermes-Lite2` at `41d75b8` unless named), the openHPSDR documents and other clients. They inform the design; the wire contract is the codec's header and golden frames ([README.md](../README.md), **No prose specs**).

| Fact | Source |
|---|---|
| Discovery is `EF FE 02` plus 60 zero bytes, broadcast to UDP 1024; reply byte `0x0A` is the Board ID, `0x06` for an HL2. | Wiki [Protocol](https://github.com/softerhardware/Hermes-Lite2/wiki/Protocol); gateware `ethernet/network.v:428` |
| **Port 1025 is a command side channel, not a second IQ client.** IQ goes to one host: whichever sent to port 1024 last while the radio was stopped. | `dsopenhpsdr1.v:317`–`:325`; `network.v:717`–`:728` |
| Frames: 1032-byte UDP payloads (`EF FE 01`, endpoint, 32-bit sequence) holding two 512-byte frames, each `7F 7F 7F` + five control bytes. Receive is 24-bit I/Q per receiver plus a 16-bit mic word, ⌊504 / (6N + 2)⌋ samples per frame for N receivers; transmit is 16-bit L/R then 16-bit I/Q, 63 per frame. No audio codec: L/R are zeros. | openHPSDR USB_protocol V1.60; `software/ft8/rx4000.py:115` |
| `hl2b5up_main`: 4 receivers and a transmitter; rates 48, 96, 192 or 384 kHz. Receive-only variants (`cicrx` 10, `6rx` 6, `4000` 5) cannot transmit. | `gateware/variants/*/hermeslite.v:99` |
| **Two dead-man layers.** PTT is a bit on every TX sample: when samples stop, a 12 ms hang (register 0x17 [12:8]) then off. A hang of 31 is software PTT with no auto-unkey ([issue #178](https://github.com/softerhardware/Hermes-Lite2/issues/178)). The watchdog clears `run` and PTT after 4,096 ticks with no downstream packet; Start bit 7 or register 0x39 disables it. Its period in seconds: not found. | `radio.v:935`–`:1047`; `dsopenhpsdr1.v:128`, `:399`, `:409` |
| Telemetry: temperature, forward and reverse power, PA current (RADDR 0x01–0x02; discovery reply 0x1C–0x23); no supply voltage. °C = (3.26 · code / 4096 − 0.5) / 0.01; A = (3.26 · code / 4096) / 50 / 0.04 ÷ (1000 / 1270). Watts from a per-hardware table (Quisk's `HL2FilterE3`: code ≈ 3,383 is 5.0 W). Which word is forward depends on the N2ADR's sense transformer: Quisk takes the larger. | Wiki Protocol; [Quisk](https://github.com/jimahlstrom/quisk) `hermes/quisk_widgets.py:132`, `quisk_conf_defaults.py:500`; [hl2_tcp](https://github.com/hotpaw2/hl2_tcp) `hl2_tcp.c:1001` |
| **N2ADR filters:** one byte over I2C to 0x20 (an MCP23008), from register 0x00 [23:17]; GP0 unused, GP1 to GP6 bands 1 to 6: b0 160 m, b1 80, b2 60/40, b3 30/20, b4 17/15, b5 12/10 low-pass. The low-passes are in the receive path too (on 40 m at night the 60/40 m low-pass alone stopped the ADC clipping). The receive high-pass (series 1,500, 680, 1,500 pF, shunt 1.8 µH twice; about 2.5 MHz at −3 dB, 17 dB down at 1.84 MHz, computed from the netlist) is **in line unless b6 is set**: relay K7 is driven through a NAND of b6 and the HL2's INTTR, so set b6 only on 160 m. No byte written is no transmit filter. | Wiki Protocol, "Filter Selection"; `hardware/companions/n2adr/n2adr.net` (U5, K7); bench |
| LNA: register 0x0a bit 6 set gives −12 to +48 dB; clear, the Hermes-style −12 to +20 dB. ADC overload is RADDR 0x00 [24]. | Wiki Protocol, "LNA Gain" |
| **Byte 27 of the discovery reply is `resp_control`:** external CW key, PTT, EXTTR, internal T/R, `tx_on`, `cw_on` and a 2-bit clip count. Address 0's upstream C3 is the TX FIFO's `dsiq_status`, a recovery flag and the FIFO's count [6:0]. | `control.v:899`, `fifos.v:116` at `f0247d4` |
| **Protocol 1 has no authentication:** any host can stop the radio, and the next to send while it is stopped gets the stream. | `ethernet/ip_recv.v`, `udp_recv.v`, `network.v:686` |
| **`radio-utils-emu` is not a faithful HL2:** two receivers, Hermes-style telemetry, up to 32 clients, no watchdog or PTT hang. A smoke-test peer only. | [docs.rs](https://docs.rs/crate/radio-utils-emu/latest) |

**First contact, the operator's HL2** (laptop probe and SparkSDR 2.0.992, receive only). The discovery reply, MAC zeroed; [mg9c](#mg9c) commits it as a golden frame:

```
ef fe 02 00 00 00 00 00 00 4a 06 00 00 00 00 00
00 00 00 04 45 02 00 00 00 00 00 03 03 d3 00 0a
00 0b 00 00 00 00 0a 00 04 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00
```

* **Byte map** (offset = 9 + 0x32 − the case label, `rtl/usopenhpsdr1.v:250`–`:306`): 2 status (`02` stopped, `03` running); 3–8 MAC; 9 `VERSION_MAJOR`; 10 `0x06`; 13–16 the EEPROM static IP; 19 receivers; 20 bandscope bits and `BOARD[5:0]`; 21 `VERSION_MINOR`; 28–29 temperature; 30–31 `fwdpwr`; 32–33 `revpwr`; 38 `tx_buffer_latency`; 40 (low five bits) `ptt_hang_time`.
* **This unit:** gateware 74.2, `BOARD` 5, 4 receivers, no static IP. Telemetry agrees with the formula (27.9 °C against SparkSDR's 27.8).
* **Transmit settings at power-on:** `tx_buffer_latency` 10 ms and `ptt_hang_time` 4 ms, 74.2's defaults in source (`usopenhpsdr1.v:108`–`:109` at `883a338`); the wiki's 20 ms and 12 ms are the wiki's.
* **Addressing:** with no DHCP lease it takes 169.254.MAC[15:8].MAC[7:0] (`network.v:182`–`:191`), and answers a broadcast to 169.254.255.255 from a link-local address on a direct cable.
* **On the P4 (bench, 2026-11-03, 40 m and 20 m in the evening):** taken as sent, the HL2's Q decoded nothing (256 candidates, no decodes); **negated**, 9 to 30 a slot. One receiver at 48 kHz streams about 383 packets/s; 0 gaps in 404,000. FT8 decoded on 160, 80, 60 and 40 m at WSJT-X's default dials.
* **The watchdog cleared `run` about 43 s** after our last downstream packet (an Ethernet pull; one sample, ±1 s); until then it answered discovery as running. After its link returns the HL2 answers nothing, ARP included, for about 16 s: 46 s from the pull to streaming again.

**Lands in:** `hl2.md` with [mg9c](#mg9c): the table, the byte map and this unit's first-contact readings, each with its source, and the discrepancies as [mg9c](#mg9c) resolves them. A fact still unread when [mg9c](#mg9c) ships stays here, in **Open**.

## Why not

* **A host main board on a home computer:** it runs `jt9` and four bands with room to spare, but needs a third main board (shared station logic, an OS seam for decode, two API servers, pairing on the host), and it stays home.
* **A second P4 for more bands:** worth designing only once one P4's capacity is measured; RFC 0016 keeps it as a direction.
* **Two clients on the HL2, or SparkSDR beside us as a live reference:** Protocol 1 sends IQ to one host, and port 1025 carries none. Score recorded slots against `jt9` instead.
* **WSJT-X, SparkSDR, or RigPi as the station:** they lose our autoseq, log and phone UI. RigPi (MIT, read at `hlnurse/rigpi-sdr` `7c0c913`) has no HL2 client, does receive-only FT8 on one band, and lets anything on the LAN drive the radio; its HackRF transmit guard (band list, one transmission, a time cap) informs Safety 9, and its zero-IQ fill of missing audio is what Safety 10 forbids.
* **A complex FFT in place of real audio:** it replaces the front of the monitor stage, refits the SNR constant and moves the recall floors.
* **Receive-only gateware for more bands:** it cannot transmit.

**Lands in:** with [mg9c](#mg9c), a **Why not** list in `hl2.md`; the complex FFT goes to [decode.md](../architecture/decode.md), **I/Q front end**, and RigPi's transmit lessons with [abkw](#abkw), under Safety.
