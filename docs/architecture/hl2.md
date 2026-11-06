# The Hermes-Lite 2

The HL2 is the P4's second radio, beside the QMX, and **receives only** so far. What is still planned, transmit first, is epic [knpp](../roadmap/hl2-support.md). Where its pieces are described:

* the transport on the P4 (the PHY, discovery, the stream, the states the app shows, RAM): [main_board.md](main_board.md), **Ethernet**
* choosing it in Settings › Radio, live: [radio.md](radio.md), **How it works**
* its channel through the I/Q front end: [decode.md](decode.md), **I/Q front end**
* the radio chip and the slot bar's receive cues: [webapp.md](webapp.md)

This doc holds the rest: the codec's contract, the link, how receive is set up and why, and the facts about the radio that the code rests on.

## The codec

`components/hl2_proto` is the HL2's openHPSDR Protocol 1 wire format: discovery, start and stop, the receive registers and upstream receive packets with their status words. A pure codec (no sockets, no globals, nothing allocated), host-tested in `host_test_hl2_proto`. **Its contract is its header plus the golden frames in `host_mock/hl2_golden/`**, recorded from the operator's HL2 on gateware 74.2, including the downstream packet the radio streamed on ([README.md](../README.md), **No prose specs**). Written from the gateware's behaviour and the openHPSDR documents; no client's code was copied.

**It cannot key the radio.** Every downstream frame has MOX clear and zero transmit samples, and `hl2_encode_control_packet()` refuses every register but config, the receivers' NCOs and the LNA, and refuses register 0x00's PA-enable bit.

`tools/hl2_record.py` (recording the HL2 on a computer, **Scored against jt9** below) writes the same wire format in Python; `tools/test_hl2_record.py` holds it to the same golden frames.

## The link

* **A private link:** one Ethernet cable from the P4 to the HL2, nothing else on it. The P4 does not route and the sidekick has no Ethernet, so the home LAN cannot reach the radio. This matters for transmit: Protocol 1 has no authentication, so anything that can send the HL2 a packet can stop, start or key it, and anything that restarts it takes our stream ([system.md](system.md), **Who may key the transmitter**).
* **Static link-local addresses.** With no DHCP lease the HL2 takes 169.254.MAC[15:8].MAC[7:0]; each P4 takes the same from its own MAC (169.254.0.x and 169.254.255.x moved in by one, as RFC 3927 reserves them), with no conflict probe. **Why no DHCP server on a P4:** an HL2 that boots first falls back to link-local anyway, a server plugged into a home LAN by mistake would break it, and 74.2's DHCP client failed once on the bench for reasons not found. **The internet is out of scope:** the P4 sits beside the radio, and a VPN looks like a LAN.
* **On a LAN, through a switch: not run yet** (epic [knpp](../roadmap/hl2-support.md), **Open**).

## Receive

* **One receiver at 48 kHz, its NCO on the dial,** so the FT8 channel is 1,500 Hz above it, with no IF and no image to correct; **Q negated**, since this HL2 sends it the other way round from the usual convention (taken as sent, 40 m found 256 candidates a slot and decoded none; negated, 9 to 30).
* **The N2ADR's filters, re-chosen on every band change** (`hl2_n2adr_receive_filters()`; its relays click). The band's low-pass, which is in the receive path as well as transmit: on 40 m at night it alone stopped the ADC clipping. The receive high-pass stays in line except on 160 m: its relay K7 is driven through a NAND of b6 and the HL2's INTTR (`hardware/companions/n2adr/n2adr.net`, U5), so b6 bypasses it, and its corner (about 2.5 MHz at −3 dB, computed from the netlist) would cost 160 m 17 dB.
* **The LNA starts at +20 dB and steps down 3 dB a second while more than 1% of a second's ADC status words read clipped,** to −12 dB, never back up within a stream. With no filter chosen the ADC clipped all the time on 40 m. Every stream clips in its first second and steps down at once (in the 2026-11-04 band survey and every stream of 2026-11-05), and later bursts step it again. **The steps cost no decodes measured, and the 1% trigger is far more sensitive than decoding needs:** on 40 m after dark, the LNA held at +20 and +14 dB in alternate turns decoded 48.8 and 49.0 `jt9` decodes a slot (paired, −0.43 ± 1.06; ours +0.13 ± 0.71), though +20 dB flagged a clip in 42% of its status words, and from +5 to +20 dB no band's decodes moved ([2026-11-05 corpus](../../tools/decode_bench/corpus/hl2-2026-11-05/README.md)). So the rule stands as it is: ignoring the start-up second or stepping back up would gain nothing measured.
* **Several receivers share one filter, one LNA and one ADC,** and the N2ADR filter is the highest band's: 10, 15, 20 and 40 m at once all go through the 10 m low-pass, with the LNA settling at +8 dB. On the 40 m end-fed that cost 10, 15 and 20 m nothing measured against one band at a time through its own filter, on the same clock window a day apart: `jt9` 6.0, 25.4 and 51.7 decodes a slot against 5.7, 22.2 and 50.0, our share of `jt9` the same or higher (a cost over about 10% would show; different days, so the band moves the counts too). Not measured on another antenna, or on a quiet 10 m on a short one ([2026-11-05 corpus](../../tools/decode_bench/corpus/hl2-2026-11-05/README.md)).
* **A sequence gap is zero-filled** with that many packets' samples, so a slot's timeline and each block's date hold; **a late or duplicate packet is dropped**, since its place was already written. Blocks are dated by their audio, as for the QMX; the HL2's sample clock is not UTC.
* **The sample clock tracks UTC closely:** over each 30-minute band of the 2026-11-04 survey, each slot's first packet arrived within −1 to +2 ms of its boundary on a computer's NTP clock, within about 1 ppm (the QMX ran about 150 ppm fast).

## Gateware

The standard `hl2b5up_main`, **74.2** (`883a338`): four receivers and a transmitter. 74.3 changes only DHCP and address handling and exists only as a testing release in the repository (`74p3`, `f0247d4`), not on the Releases page. The receive-only variants (`cicrx` 10 receivers, `6rx` 6, `4000` 5) cannot transmit.

## Facts

Read from the [HL2 wiki](https://github.com/softerhardware/Hermes-Lite2/wiki), the gateware (`softerhardware/Hermes-Lite2` at `41d75b8` unless named), the openHPSDR documents and other clients. They inform the code; the wire contract is the codec's header and golden frames.

| Fact | Source |
|---|---|
| Discovery is `EF FE 02` plus 60 zero bytes, broadcast to UDP 1024; reply byte `0x0A` is the Board ID, `0x06` for an HL2. | Wiki [Protocol](https://github.com/softerhardware/Hermes-Lite2/wiki/Protocol); gateware `ethernet/network.v:428` |
| **Port 1025 is a command side channel, not a second IQ client.** IQ goes to one host: whichever sent to port 1024 last while the radio was stopped. | `dsopenhpsdr1.v:317`–`:325`; `network.v:717`–`:728` |
| Frames: 1032-byte UDP payloads (`EF FE 01`, endpoint, sequence: 32 bits on the wire, counted in 20 by the HL2's receive stream, so it wraps to 0 every 1,048,576 packets, 45.6 min of one receiver) holding two 512-byte frames, each `7F 7F 7F` + five control bytes. Receive is 24-bit I/Q per receiver plus a 16-bit mic word, ⌊504 / (6N + 2)⌋ samples per frame for N receivers; transmit is 16-bit L/R then 16-bit I/Q, 63 per frame. No audio codec: L/R are zeros. | openHPSDR USB_protocol V1.60; `software/ft8/rx4000.py:115`; `usopenhpsdr1.v:89` |
| **Two dead-man layers.** PTT is a bit on every TX sample: when samples stop, a hang (register 0x17 [12:8]) then off. A hang of 31 is software PTT with no auto-unkey ([issue #178](https://github.com/softerhardware/Hermes-Lite2/issues/178)). The watchdog clears `run` and PTT after 4,096 ticks with no downstream packet; Start bit 7 or register 0x39 disables it. | `radio.v:935`–`:1047`; `dsopenhpsdr1.v:128`, `:399`, `:409` |
| Telemetry: temperature, forward and reverse power, PA current (RADDR 0x01–0x02; discovery reply 0x1C–0x23); no supply voltage. °C = (3.26 · code / 4096 − 0.5) / 0.01; A = (3.26 · code / 4096) / 50 / 0.04 ÷ (1000 / 1270). Watts from a per-hardware table (Quisk's `HL2FilterE3`: code ≈ 3,383 is 5.0 W). Which word is forward depends on the N2ADR's sense transformer. | Wiki Protocol; [Quisk](https://github.com/jimahlstrom/quisk) `hermes/quisk_widgets.py:132`, `quisk_conf_defaults.py:500`; [hl2_tcp](https://github.com/hotpaw2/hl2_tcp) `hl2_tcp.c:1001` |
| **N2ADR filters:** one byte over I2C to 0x20 (an MCP23008), from register 0x00 [23:17]; GP0 unused, GP1 to GP6 bands 1 to 6: b0 160 m, b1 80, b2 60/40, b3 30/20, b4 17/15, b5 12/10 low-pass; b6 the receive high-pass, bypassed when set. No byte written is no transmit filter. | Wiki Protocol, "Filter Selection"; `hardware/companions/n2adr/n2adr.net` |
| LNA: register 0x0a bit 6 set gives −12 to +48 dB; clear, the Hermes-style −12 to +20 dB. ADC overload is address 0's C1[0]. | Wiki Protocol, "LNA Gain" |
| **Byte 27 of the discovery reply is `resp_control`:** external CW key, PTT, EXTTR, internal T/R, `tx_on`, `cw_on` and a 2-bit clip count. Address 0's upstream C3 is the TX FIFO's `dsiq_status`, a recovery flag and the FIFO's count [6:0]. | `control.v:899`, `fifos.v:116` at `f0247d4` |
| **Protocol 1 has no authentication:** any host can stop the radio, and the next to send while it is stopped gets the stream. | `ethernet/ip_recv.v`, `udp_recv.v`, `network.v:686` |
| **`radio-utils-emu` is not a faithful HL2:** two receivers, Hermes-style telemetry, up to 32 clients, no watchdog or PTT hang. A smoke-test peer only. | [docs.rs](https://docs.rs/crate/radio-utils-emu/latest) |

**The operator's unit** (gateware 74.2, `BOARD` 5, 4 receivers, no static IP stored). Its discovery reply, MAC zeroed, is the golden frame `host_mock/hl2_golden/discovery_reply_2026-10-30.hex`; the byte map (offset = 9 + 0x32 − the case label, `rtl/usopenhpsdr1.v:250`–`:306`): 2 status (`02` stopped, `03` running); 3–8 MAC; 9 `VERSION_MAJOR`; 10 `0x06`; 13–16 the EEPROM static IP; 19 receivers; 20 bandscope bits and `BOARD[5:0]`; 21 `VERSION_MINOR`; 27 `resp_control`; 28–29 temperature; 30–31 `fwdpwr`; 32–33 `revpwr`; 38 `tx_buffer_latency`; 40 (low five bits) `ptt_hang_time`.

* **Transmit settings at power-on:** `tx_buffer_latency` 10 ms and `ptt_hang_time` 4 ms, 74.2's defaults in source (`usopenhpsdr1.v:108`–`:109` at `883a338`); the wiki's 20 ms and 12 ms are the wiki's.
* **The watchdog cleared `run` about 43 s** after our last downstream packet (an Ethernet pull, one sample, ±1 s); until then the HL2 answered discovery as running. After its link returns it answers nothing, ARP included, for about 16 s: 46 s from the pull to streaming again.
* **One receiver at 48 kHz is about 383 packets/s;** 0 gaps in 404,000 on the P4 (2026-11-03).
* **Discovery while running:** the source does not gate discovery on `run`, but a broadcast while the HL2 streamed got no reply, with SparkSDR and again with no other client (epic, **Open**).

## Scored against jt9

`tools/hl2_record.py` records the HL2 on a computer as one I/Q WAV per slot per receiver, driven as the P4 drives it, and `tools/decode_bench/score_ab.py` scores the slots with our decoder and with `jt9` on the same samples (`--center-hz 1500 --iq-flag=--negate-q`). In the 2026-11-04 band survey our decoder found 62–75% of `jt9`'s decodes on every band up to about 22 a slot, the share it finds on the QMX's I/Q (68% on 15 m), and 48% on crowded 20 m, the plateau the QMX showed too, so the HL2's receive chain costs it nothing measured; per band, and how it was run: [the corpus's README](../../tools/decode_bench/corpus/hl2-2026-11-04/README.md). On four receivers at once through a day (2026-11-05) it found 52.6% on 20 m, 61–64% on 10, 15 and 40 m, the same plateau where a band is crowded: [that corpus's README](../../tools/decode_bench/corpus/hl2-2026-11-05/README.md).

## A capture beside the P4

The P4 decodes the HL2 while a computer between them captures the same packets, so the P4's decodes and the host's are compared on identical slots, and the host can decode them under any setting: `tools/hl2_pcap.py` cuts the capture into `hl2_record.py`'s slot WAVs and manifest by the capture's own timestamps, and `tools/decode_bench/capture_ab.py` scores them (2026-11-06: [the corpus's README](../../tools/decode_bench/corpus/hl2-capture-2026-11-06/README.md)). Receive only: nothing on the computer talks to the radio.

* **The link stays private:** the computer bridges its two Ethernet ports (Linux: an `nmcli` bridge with no address on it, `ipv4.method disabled`, `ipv6.method disabled`), the HL2 on one and the P4 on the other, and captures on the bridge, `tcpdump -i br-hl2 -w FILE udp src port 1024`, about 1.5 GB an hour. A switch's mirror port does the same. The computer's own network moves to Wi-Fi meanwhile; a profile that claims a port, such as one for `hl2_record.py`, has to be off or it takes the port from the bridge.
* **The P4's clock is not UTC:** its decode sync holds it about 0.5 s behind, where signals start ([time.md](time.md), **Where it settles**). Slots cut on UTC end their decode window half a second before the signals do (the host's `dt` read +0.4 to +0.8 where the P4's read −0.7 to 0.0), so `--start-offset-s 0.7` cuts them where the P4 does.
* **Scaled before 16 bits:** the HL2's channel comes out of the front end with the band noise under one LSB of a 16-bit WAV on a quiet band, and the host then loses the weak decodes the P4, decoding floats, keeps (40 at −13 to −20 dB in 48 slots on 12 m). `capture_ab.py` scales each slot to −24 dBFS first.
* **The host matches the P4:** at the P4's budget, 178 messages both, 3 the P4 alone, 4 the host alone over 47 slots of 12 m; 1,874, 78 and 78 over 83 slots of 20 m, the rest of the alignment: the P4's offset moves with each sync step.

## Why not

* **A host main board on a home computer:** it runs `jt9` and four bands with room to spare, but needs a third main board (shared station logic, an OS seam for decode, two API servers, pairing on the host), and it stays home.
* **Two clients on the HL2, or SparkSDR beside us as a live reference:** Protocol 1 sends IQ to one host, and port 1025 carries none. Score recorded slots against `jt9` instead.
* **WSJT-X, SparkSDR or RigPi as the station:** they lose our autoseq, log and phone UI. RigPi (MIT, read at `hlnurse/rigpi-sdr` `7c0c913`) has no HL2 client, does receive-only FT8 on one band, and lets anything on the LAN drive the radio.
* **Receive-only gateware for more receivers:** it cannot transmit.
* **A DHCP server on the P4:** under **The link**, above.
