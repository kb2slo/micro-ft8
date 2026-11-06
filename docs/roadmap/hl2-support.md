# HL2 support (epic knpp)

The Hermes-Lite 2 (HL2) as a second radio on the P4, beside the QMX: receive, then transmit, at home, then in the park. Rows: [abkw](#abkw), [kgyh](#kgyh) ([ROADMAP.md](../ROADMAP.md)). Receive on one band has shipped: how it works is [hl2.md](../architecture/hl2.md). Several HL2 bands at once is epic [u8mc](several-channels.md)'s several-HL2-bands slice.

## Goal

Operate FT8 from an iPhone anywhere in the house, asynchronously, on the home station (HL2, N2ADR filter board, antenna), and the same station in the park for POTA. The HL2 is the station's several-band radio: up to four receivers, each a source of channels that epic [u8mc](several-channels.md) decodes and operates.

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
* **A private link,** built for receive ([hl2.md](../architecture/hl2.md), **The link**), and what makes transmit safe to add: Safety 11.
## Design

### Backend

* **Radio control:** a new `radio_control_ops_t` table and capabilities ([radio.md](../architecture/radio.md), **Adding a radio**). The tone lead becomes its own property instead of following `begin_cpfsk_tx` (`radio_control.cpp:102`): the HL2 takes symbols itself but needs the PTT lead for its own T/R relay.
* **The web app** changes only where the radio does: its name, and the HL2's telemetry where the radio bar shows the QMX's.

### Receive

* **On transmit, the TX FIFO's recovery flag and count** (address 0's C3) join the receive counters the P4 already logs ([main_board.md](../architecture/main_board.md), **Ethernet**).
* **As a source of several channels** (epic [u8mc](several-channels.md), Later): four receivers, each on its own band; FT8 and FT4 on one band are two front-end instances on one receiver. The N2ADR receive filter serves every receiver, so it is set for the highest band listened to (as Quisk does), and ADC overload is counted per slot and reported as the source's reason when it rises.

### Transmit

* **One transmission at a time,** on the channel epic [u8mc](several-channels.md) chooses. It sets the HL2's TX NCO and the N2ADR transmit filter for that channel; no receiver moves.
* **The waveform:** GFSK, BT 2.0 for FT8 and 1.0 for FT4, first and last symbols ramped, as ft8_lib's `demo/gen_ft8.c` and WSJT-X make it. Complex baseband at 48 kHz, offset by the audio offset from a TX NCO at the dial, written ahead into the HL2's transmit buffer. The first tone at the slot start plus 0.5 s, as on the QMX ([radio.md](../architecture/radio.md), **TX timing on a QMX**). ft8_lib's encoder is proved against `ft8code` (row [I40](../ROADMAP.md#I40)) before anything transmits.
* **PTT before RF:** PTT rides every transmit sample, so the P4 keys with silent samples for the Min TX delay before the first tone, giving the HL2's T/R relay time to switch.
* **The filter before every key-down:** the N2ADR transmit LPF for the band, then the receive byte again afterwards.

### Safety

1. **Transmit is off by default,** enabled on this hardware only after the bench procedure (8) has passed and is recorded in [TEST_PLAN.md](../TEST_PLAN.md).
2. **SWR protection in the client.** Γ = √(P_rev / P_fwd), forward the larger reading. Evaluated only above a minimum forward power (Quisk uses 0.05 W); K consecutive readings over the limit trip it; it latches transmit off until the operator resets it in the web app. Power from a calibration table, Quisk's `HL2FilterE3` until this unit is calibrated.
3. **A drive cap:** register 0x09 [31:28] has 16 steps; a compile-time maximum clamps every setting.
4. **Never sent while transmit is enabled:** a PTT hang of 31, or either watchdog-disable bit. The codec refuses to encode them, host-tested.
5. **Stream loss** relies on the gateware's two dead-man layers ([hl2.md](../architecture/hl2.md), **Facts**), and the bench proves them: pull the cable and reset the P4 mid-transmission into a dummy load; record how long RF takes to stop, and the watchdog's period.
6. **A runaway transmitter** is bounded by row [u4dw](../ROADMAP.md#u4dw)'s cap.
7. **A filter write that fails stops the transmission.**
8. **The bench procedure:** a dummy load through an attenuator; occupied bandwidth against WSJT-X's; harmonics and spurs through each band's LPF; RF start and stop against the slot boundary.
9. **A frequency outside the allowed band is refused,** by epic [u8mc](several-channels.md)'s rule for every backend: before key-down the backend checks the TX NCO plus the audio offset, and the signal's bandwidth, against the band table.
10. **A gap in transmit samples ends the transmission.** The P4 never fills a gap with samples carrying PTT: that would hold the transmitter keyed and defeat the gateware's first dead-man layer. If the writer falls behind by more than a set time, or the TX FIFO's recovery flag (address 0's C3) reports it ran dry, the transmission stops, PTT drops, and the slot is counted and marked unclean. Host-tested against a stalled writer. No browser heartbeat: transmission is autonomous by design, so the station-side bound is item 6.
11. **Who may key it:** the pairing token guards our API, not the radio. Protocol 1 has no authentication, so anything that can send the HL2 a packet can stop, start or key it, and anything that restarts it takes our stream (seen as stream loss). Hence the private link; the phone's path stays through the sidekick, behind the token ([system.md](../architecture/system.md), **Who may key the transmitter**).

## Parity

Every feature the QMX station has, against what the HL2 needs for it. **Fits**: the design above covers it. **Work**: a known build with a named challenge. **Open**: feasibility not yet known. **None**: nothing to build, and why.

| Feature (QMX today) | On the HL2 | |
|---|---|---|
| Decode FT8 from I/Q through the front end | One receiver through the same front end (**Receive**). | Fits (built) |
| FT4 (rows [an8x](several-channels.md#an8x) and [grqg](several-channels.md#grqg), both radios) | Same pipeline; on transmit, BT 1.0. | Fits |
| Transmit: the QMX makes the tones from `TA` | The P4 makes the waveform and streams it, 381 packets/s on core 0, with no gap allowed (Safety 10). Core 0's headroom while transmitting is unmeasured. | Work ([abkw](#abkw)) |
| Band change: `FA`, and the QMX's own filters | NCO plus the N2ADR byte, both ours, re-chosen on every band change; its relays click. The receive filter is shared once several receivers run (**Receive**). | Fits (built) |
| Power and SWR while transmitting (`PC;SW;`, slot bar) | Forward and reverse telemetry through a calibration table. Which word is forward on this unit, and how often telemetry refreshes while keyed, are unread; until calibrated, the watts are Quisk's table, not this unit's. | Work ([abkw](#abkw)) |
| SWR protection: the QMX's own latch, which the P4 reads (`SR;`, `amp_swr_guard`) | The HL2 has no latch of its own that we found, so the client latch (Safety 2) is the only one. Its reaction time is set by the telemetry rate and must be measured on the bench, not assumed. | Work ([abkw](#abkw)) |
| Tune: SWR Tune (`MD8;`) at the QMX's Tune %, SWR shown, ended by a trip | A carrier from the P4 at a Tune drive level of ours (the HL2 has no Tune %), SWR from telemetry, ended by the client latch: row [bfkb](../ROADMAP.md#bfkb)'s stop comes built in. A continuous carrier needs row [u4dw](../ROADMAP.md#u4dw)'s cap and the stream-loss dead-man. At low drive the forward power may be under the latch's minimum, so the latch may not see a bad antenna. | Work (later slice) |
| SWR sweep: per point `FA`, `MD8;`, `SW;`; the QMX refuses frequencies outside its configured bands | Per point: NCO, the band's LPF, a short carrier at sweep drive, telemetry, unkey; or stay keyed across a band and move the NCO, if the HL2 retunes while keyed. Unknown: telemetry settling time after keying or retuning, and T/R relay wear from keying per point. `radio_swr_io_t` is QMX-shaped (`MD8`, `SW`) and becomes "carrier at f, read SWR". The band table (Safety 9) replaces the QMX's own refusal of out-of-band points. Row [vu8t](../ROADMAP.md#vu8t)'s Tune-% label becomes our sweep drive. | Open (later slice) |
| Prime an RF-sensing amplifier on a band change | Out of scope with the amplifier. If it returns, it is a 500 ms Tune. | None |
| Radio bar: **CAT** and **Audio** ✓/✕ | The HL2's chip states (no link, silent, busy, found, streaming, lost; **Receive only**) and the slot bar's receive cues ([webapp.md](../architecture/webapp.md), **The slot bar's receive cues**). Getting started quotes **CAT** and **Audio**, so the site audit applies when this ships. | Fits (built) |
| **Radio firmware** in the drawer (`VN;`), and the warning before 1.04.010 | The gateware version from the discovery reply is logged; not yet in the drawer, and no warning off 74.2. | Work (Later) |
| Read-back and pushed state (`IF;`, `AI2;`): **Put it back** / **Use it** when the radio moved | No front panel, so the HL2 is only where we set it. Its "radio moved" is another client holding the stream (Protocol 1), shown as **busy**; a lost stream is **Stream lost**. | None; the fault is built |
| Setting the radio's clock (`can_set_time`) | The HL2 has no clock. | None |
| The IQ-mode probe at link-up (`Q9`) | The HL2 is always I/Q. | None |
| Bands offered from the radio (row [vcng](../ROADMAP.md#vcng): `OM;`, `VN;`, its band table) | The HL2 covers 160–10 m. Which bands may transmit follows the N2ADR filters, and whether the board can be detected (the MCP23008 read-back, Open) is unknown; if not, it is a setting. Listen-only bands per radio: row [mtd6](../ROADMAP.md#mtd6). | Open |
| Per-radio settings (row [mtd6](../ROADMAP.md#mtd6)) | Identity is the MAC from discovery. | Fits |
| Receive gain: none to set on a QMX | The LNA starts at +20 dB and steps down 3 dB a second while the ADC clips, never back up within a stream; on 80 m at night it settled at +11 dB. Stepping back up would gain nothing measured ([hl2.md](../architecture/hl2.md), **Receive**). | Fits (built) |
| Frequency accuracy: the QMX's own reference | The HL2's reference sets every NCO. An error of a few ppm is tens of Hz on 10 m, within FT8's search but visible in `df`; unmeasured. A correction is per radio (row [mtd6](../ROADMAP.md#mtd6)) if one is needed. | Open (Later) |
| Listen on the phone (row [fc6w](../ROADMAP.md#fc6w)): the QMX demodulates | The front end already makes upper-sideband audio, so listening on the FT8 window is nearly free; other modes need demodulators of ours. | Work ([fc6w](../ROADMAP.md#fc6w)) |
| Field power from one bank | 12 V for the HL2; its receive and transmit current unmeasured. | Work ([kgyh](#kgyh)) |
| TX watchdog (row [u4dw](../ROADMAP.md#u4dw)), AP decoding (row [qauk](../ROADMAP.md#qauk)), autoseq, the log | Independent of the radio; the log names the HL2. | Fits |
| The simulator: a fake QMX behind `sim_station` | The codec is held by golden frames from this unit (built). A fake HL2 behind `sim_station`, so receive, transmit and safety run in `host_mock` end to end, is not built. `radio-utils-emu` stays a smoke test, not a model. | Work (Later) |
| Heat: the QMX's own business | The HL2 reports its PA temperature. FT8 keys about half the time, so a temperature cutoff becomes a safety item, its limit to be set from the bench. | Work ([abkw](#abkw)) |

**Lands in:** each row with the slice named in its last column. When the epic ends, every row still None or not at parity becomes a **Differences from the QMX** list in `hl2.md`, one line each with its reason (no Tune %, no clock, no front panel), so a reader does not file them as bugs.

## Children

* <a id="abkw"></a>**abkw: transmit, barefoot.** After row [I40](../ROADMAP.md#I40). Waveform, PTT lead, filter, drive cap, SWR latch, stream loss, the band table, transmit off by default. The safety policy host-tested (category 1); everything RF on the bench (category 2). **Done-when:** which telemetry word is forward on this unit, read while transmitting; the bench procedure (Safety, 8) passes into a dummy load and is recorded in TEST_PLAN; then a QSO on the air at 5 W (field). **Lands in:** `hl2.md` (transmit, and a **Safety** section holding each item as built with its bench result and host test; items 6 and 9 land with row [u4dw](../ROADMAP.md#u4dw) and epic [u8mc](several-channels.md)); radio.md (the tone lead, TX timing beside the QMX's); system.md (who may key it); webapp.md (power and SWR for the HL2); TEST_PLAN (the bench procedure's result); the site, what is verified on the air.
* <a id="kgyh"></a>**kgyh: in the park.** After [abkw](#abkw). AP mode with the HL2, field power measured, listen-only bands (row [mtd6](../ROADMAP.md#mtd6) holds them per radio). **Done-when:** a POTA activation on the HL2 station, with the power draw recorded (field). **Lands in:** main_board.md (**Field power** with the HL2); `hl2.md` (its current draw).

**Later** (slices not yet rows; grooming makes each one a row when it is next):

* **The HL2 against the QMX on a split antenna:** one antenna through a passive 2-way splitter into both radios (the same loss into each), recorded on the same slots with `tools/hl2_record.py` and `tools/qmx_record.py` side by side. The same station's SNR on both, slot by slot, is their relative sensitivity with the band taken out; decodes only one radio makes, and losses beside a strong signal, show the rest. Receive only: neither radio may transmit into the splitter. A scorer that pairs stations across the two, on top of `tools/decode_bench/score_snr.py`. Different days on one radio each cannot answer this: the band changes more between days than the radios differ. Lands in `hl2.md` and the corpus's README.
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
* **A QMX and an HL2 at once:** channels from each (epic [u8mc](several-channels.md)), all decoding, a reply on any of them transmitted by the radio whose channel heard it. Needs a queue entry carrying its radio as well as its band, and one backend per radio in place of one selected backend.
* **The receive high-pass's loss on 80 m,** where it is in line: about 2.5 MHz at −3 dB is computed from the netlist, not measured. A signal generator, or b6 set and clear on alternate slots on 80 m.
* **The receive high-pass during transmit:** its relay is a NAND of b6 and INTTR, whose polarity is not read, so what the receive path sees while keyed is unknown ([abkw](#abkw)).
* **Whether the MCP23008 reads back** over the HL2's I2C read, for Safety 7 ([abkw](#abkw)).
* **Bench instruments** for Safety 8: dummy load, attenuator, a second receiver or a spectrum analyser ([abkw](#abkw)).
* **The HL2's enclosure and I/O board** (before [kgyh](#kgyh)).
* **How the operator marks a band listen-only** for the antenna in use ([kgyh](#kgyh), with [mtd6](../ROADMAP.md#mtd6)).
* **Telemetry while keyed:** how often forward and reverse power refresh, which sets the client latch's reaction time ([abkw](#abkw)).
* **Retuning while keyed,** and telemetry settling after a key or retune (the SWR sweep).
* **Tune's drive level** against the latch's minimum forward power (Tune).
* **A temperature limit** for the PA ([abkw](#abkw)).

**Lands in:** each answer, as a measured fact with its source, in `hl2.md` with the child that settles it; an answer that changes the design edits **Design** here first.

## Why not

* **A second P4 for more bands:** worth designing only once one P4's capacity is measured; epic [u8mc](several-channels.md) keeps it as a direction.
* **RigPi's transmit guard and its zero-IQ fill** (`hlnurse/rigpi-sdr` `7c0c913`, MIT): its HackRF guard (a band list, one transmission, a time cap) informs Safety 9; its filling of missing audio with zero IQ is what Safety 10 forbids on an HL2, where samples carry PTT.

The rest of the epic's why-nots moved to [hl2.md](../architecture/hl2.md), **Why not**, when receive shipped.
