# HL2 four bands at once, 2026-11-05

The Hermes-Lite 2 recorded on a computer on 10, 15, 20 and 40 m at once, on four receivers, from 8:17 AM to 6:02 PM CST, then 40 m alone after dark with the LNA held at two gains in turn, and every slot scored with our decoder against `jt9` on the same samples: what row q9my (four bands at once, and the LNA at two gains) asked for, beside the one-band [2026-11-04 survey](../hl2-2026-11-04/README.md). The audio is not in git. It is the release [`corpus-2026-11-05-hl2`](https://github.com/kb2slo/micro-ft8/releases/tag/corpus-2026-11-05-hl2), once uploaded: one tar per stream and band holding that band-hour's WAVs and `manifest.jsonl` (paths `<stream>/<band>/`), each about 1 GB, so one band-hour can be fetched alone (a release asset must be under 2 GiB); one tar for the LNA A/B (`lna-ab/40m/`); and the run's log. The manifests here are copies, each band's streams concatenated in order (`manifest-<band>.jsonl`, and `manifest-lna-ab-40m.jsonl`), so every file's SHA-256 is in git. The tars are reproducible: `tar --format=gnu --sort=name --mtime=2026-11-06T00:00:00Z --owner=0 --group=0 --numeric-owner --mode=a+rX,u+w,go-w`, over the WAVs and manifests only.

| File | SHA-256 |
| --- | --- |
| `hl2-2026-11-05-now-10m.tar` | `24b1b3469709a66ac5e7c605536efcf5814b4da8deba5c8a298fa2c9c3c75a9e` |
| `hl2-2026-11-05-now-15m.tar` | `64842375b02d00a0e6e19cce9fb321f1b3f44ba0af87f58e41056b88be4a7fed` |
| `hl2-2026-11-05-now-20m.tar` | `8528e8852c74c4f294356846451b048f359beafebd3641ef8f56c7e24002118c` |
| `hl2-2026-11-05-now-40m.tar` | `9cbd624a1232131d1e68091647cb24b278331f15ec43e98872bf477a57285703` |
| `hl2-2026-11-05-1500z-10m.tar` | `e34e78636ba1fd0075ea60af1271482ba62e3313691b0981f32c3c6840cc916b` |
| `hl2-2026-11-05-1500z-15m.tar` | `e3b06172c1635787ea7a22c3da7c49323a067fd603f41547835d2319c6ca4af9` |
| `hl2-2026-11-05-1500z-20m.tar` | `e8a6d2ba7d3830f758bed948d71fd5d7df62f636ebb26a46806fcc97769ce15c` |
| `hl2-2026-11-05-1500z-40m.tar` | `92a2fa4ee805ef05df9363b7bb7d89d87c031b5a7137ed63fd5228da3c2f798d` |
| `hl2-2026-11-05-1600z-10m.tar` | `337655b2ee49ad91d10520bad678478ed9791a61890b4621c0300924636d2b65` |
| `hl2-2026-11-05-1600z-15m.tar` | `e509b847c14b7321dd028cc190e7d45b7a3f9fc01dd96db8d6472d29dbe7cb6d` |
| `hl2-2026-11-05-1600z-20m.tar` | `2732cfa5e83066e29191551dcae0159d26a3f26421e0d2bc0a135b40e97f2de3` |
| `hl2-2026-11-05-1600z-40m.tar` | `01fc1ac79c333f14e2e2cc793bd7b85a8c5a02703f35223bc4aa11661cde391a` |
| `hl2-2026-11-05-1700z-10m.tar` | `7bf07ff405ee5875be0fda555c25fbcd9c0fd1ce744752546ffd74d3291f70a4` |
| `hl2-2026-11-05-1700z-15m.tar` | `90f5ee50fa0aa59c131c57fbc1dcb445cdfe56f9c989e810db6c77d741798e45` |
| `hl2-2026-11-05-1700z-20m.tar` | `8ede6d0d61d11f763dfc82b205e867a660412dd594924525f3ce477be2e97ce8` |
| `hl2-2026-11-05-1700z-40m.tar` | `1c4d10107f9e9ce7eb1c73927870defe43225f09c7413877b3fe4b7731d1d1fb` |
| `hl2-2026-11-05-1800z-10m.tar` | `54a81ea4cfd18e7e7d1545cd24c0b6dd46c2b484cce81bc281d67bbdeb49bf55` |
| `hl2-2026-11-05-1800z-15m.tar` | `b583a866e9995ab7f11cf9c47d1d1f9feaa999fc04a94605fbc7ed2eb37cb925` |
| `hl2-2026-11-05-1800z-20m.tar` | `64b0385285ff2710d511b2fc9917c054c38f03c9571e9aaec19f4f892a085230` |
| `hl2-2026-11-05-1800z-40m.tar` | `7f227ce0259403c871a95180a2f4df1a8622a69cdce6030d7c4fdec9a28d8e74` |
| `hl2-2026-11-05-1900z-10m.tar` | `c676d9b7f14a54608bdb7c4b330a5fe67a7d4fb4912c7ae6505d342d4744cd79` |
| `hl2-2026-11-05-1900z-15m.tar` | `0e0434b39b3bbc57ed89bed2a90772582e5b37555999205bf82b94c7d5c6e3ed` |
| `hl2-2026-11-05-1900z-20m.tar` | `23ef49295d77c1e0e7e2220a8f004aad1c0fbc470778ac9e7cf58c121f09d2ff` |
| `hl2-2026-11-05-1900z-40m.tar` | `8a33e629a73266095138c1e87eea94549cfbf1746f7508146e9626b0510129a6` |
| `hl2-2026-11-05-2000z-10m.tar` | `86825c2c21a0058688f9e8c21912b974766d8fb935f3514db4e1e159fd093f43` |
| `hl2-2026-11-05-2000z-15m.tar` | `0793d88a1fd1236ca5d004c6a3333c94ab31031607ac0f4f62de2464626bc2e9` |
| `hl2-2026-11-05-2000z-20m.tar` | `14f1bc5c96ae95bdc2647337ecb37798b83308941e553e09c20c8fa793d5bcf9` |
| `hl2-2026-11-05-2000z-40m.tar` | `e2a711f8d36cda734fcff0200db4f2ee5c12aabb0f89aa69854f752585eabacb` |
| `hl2-2026-11-05-2100z-10m.tar` | `e8e9225f5a6ad5b6e3bdd349270122b8f2331b020c68654f259adaeb5a0d837b` |
| `hl2-2026-11-05-2100z-15m.tar` | `b01e55beb6f7974de3bb17d47c72a229bc5560be4c6df3ce911e8fdea41c31ca` |
| `hl2-2026-11-05-2100z-20m.tar` | `937636393c8f77312b79927e76b693abdb1ff129fafc4093e9b11b61d0eb356c` |
| `hl2-2026-11-05-2100z-40m.tar` | `d81971b126519790e9035d3d036f8a759741d890f8f56bcd5060a7e64d07ac43` |
| `hl2-2026-11-05-2200z-10m.tar` | `1c76e098905bc814d3b4c7e4f4aae78ee54911e685eba12b431869120095d71b` |
| `hl2-2026-11-05-2200z-15m.tar` | `0e962ebfc59febbc95522b715860ac52e54541932a2f09332664c9e29572091e` |
| `hl2-2026-11-05-2200z-20m.tar` | `5ccee9eb035ee1fe42bf9d229d29606af4cfe310b9a61184aa3b32ac1797376d` |
| `hl2-2026-11-05-2200z-40m.tar` | `41eb8657d4b67018775a0ee676aacee8394997d5226bbce53263183f100e460c` |
| `hl2-2026-11-05-2300z-10m.tar` | `ca8ef26032913fc1b2ff17328c83dfdf5dd9d739eb5d175604473fe969ce50aa` |
| `hl2-2026-11-05-2300z-15m.tar` | `e82a8321cee748093978a39c13e966884f53876dbcbf795e78d8363734705d73` |
| `hl2-2026-11-05-2300z-20m.tar` | `a95aa3a1cf9ef87bf84e6e30b9450ddcf23d090450afcc299fd393c8dfc8b530` |
| `hl2-2026-11-05-2300z-40m.tar` | `76cdc019810a76f7641d401ce90cdd2c350676ac93db3423aca09a1f31b05790` |
| `hl2-2026-11-05-lna-ab-40m.tar` | `dfe303b030b5f831d4016e7491c977fc4ff61c63ef7516e5dfbcd8c06cdeb3a5` |
| `hl2-2026-11-05-run.log` | `2728e368efdada1faf96ad5cc4f9f2aa84cbb0fc746af77e4e48e33e4f0026e5` |

The Experiments page on the site draws this day as [four bands at once](https://micro-ft8.kalikstein.com/experiments/four-band/) (`webapp/experiments/four-band/`), from one slot in eight decoded by `jt9` and these per-stream scores.

## The station and the run

* KB2SLO at grid EM10cg (Austin, Texas). HL2, gateware 74.2, with the N2ADR filter board; the same 40 m end-fed half-wave as the survey, good on 40, 20, 15 and 10 m and poor on the others; receive only.
* Its Ethernet straight into a Linux laptop's port with a link-local address; the laptop's clock NTP-synced.
* `tools/hl2_record.py`, one stream an hour (a plan of `28074000+21074000+14074000+7074000`, 240 slots at each hour from 1500z to 2300z, preceded by `now`, 169 slots from 14:17 UTC), 2,329 slots a band; then `--plan 7074000:240@00:30 --lna-ab 20,14`, 240 slots from 6:30 PM CST. The HL2 driven as the P4 drives it: each receiver at 48 kHz with its NCO on the band's FT8 dial.
* **Four receivers share one filter, one LNA and one ADC.** The N2ADR byte is the highest band's, so all four bands heard through the 10 m low-pass (`n2adr` 32 in every four-band manifest line); the A/B run, one receiver on 40 m, had 40 m's (4). The LNA, ADC clipping, temperature and packet counters are the radio's, the same in every band's manifest for a slot.
* **The LNA rule:** each stream starts at +20 dB and steps down 3 dB a second while more than 1% of a second's ADC status words read clipped, never back up within the stream ([hl2.md](../../../../docs/architecture/hl2.md), **Receive**). Every stream stepped down in its first seconds, before its first slot began, so no slot starts at +20 dB; the run log has each step.
* **The morning's two attempts are not in the corpus.** Each lost its stream 13.8 minutes in: the HL2 counts its receive sequence in 20 bits and sends it in a 32-bit field, so with four receivers it wrapped to 0 after 1,048,576 packets, which the recorder compared as 32 bits and read as a million packets late, sent no more control packets, and the HL2's watchdog stopped. Compared modulo 2^20 (#45, recorder and P4), the `now` stream ran through the wrap at about 14:31 UTC, the fix's bench proof, and no stream lost or delivered late a packet all day.
* **Not the P4's figures.** Packets lost and late here describe the laptop's receive, not the P4's network stack ([main_board.md](../../../../docs/architecture/main_board.md), **Ethernet**).

## How it was scored

`python3 tools/decode_bench/score_ab.py BAND_DIR --center-hz 1500 --iq-flag=--negate-q` on every stream's band directory, on the three survey-matched windows (below) and on the A/B with `--arm-field lna_arm_db`: each slot through `tools/iq_convert` at the HL2's channel (1,500 Hz above the NCO, Q negated), then `jt9 -8 -d 3` and `tools/decode_recall` at the firmware's settings and at t4/f2. The tables below were printed from those `score.txt` files and the manifests by `python3 tools/decode_bench/score_table.py RECORDING_DIR now 1500z 1600z 1700z 1800z 1900z 2000z 2100z 2200z 2300z`.

## Results

Each band's day, every slot (shares weighted by `jt9`'s decodes, from the hourly summaries):

| Band | Slots | `jt9` per slot | Ours per slot | Ours, share of `jt9` | t4/f2, share | Lowest hour for ours |
| --- | --- | --- | --- | --- | --- | --- |
| 10 m | 2,329 | 9.6 | 5.9 | 61.2% | 64.2% | `now` (8:17 AM): 53.5% of 18.4 |
| 15 m | 2,329 | 24.3 | 15.5 | 63.8% | 66.2% | `now` (8:17 AM): 57.0% of 23.1 |
| 20 m | 2,329 | 44.4 | 23.4 | 52.6% | 54.4% | `1900z` (1:01 PM): 49.4% of 48.6 |
| 40 m | 2,329 | 10.6 | 6.6 | 61.9% | 64.1% | `1900z` (1:01 PM): 55.4% of 1.5 |

Per stream and band. LNA, clipped status words (the stream's total, of about 9,470 a slot), temperature and lost / late packets are radio-wide, so the same in a stream's four rows:

| Stream (CST start) | Band | Slots | `jt9` per slot | Ours per slot (share of `jt9`) | t4/f2 | `jt9` SNR median | LNA | Clipped status words | HL2 °C | Lost / late packets |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `now` (8:17 AM) | 10 m | 169 | 18.4 | 9.8 (53.5%) | 10.4 (56.7%) | −12 dB | +11 dB | 95 | 29.7–30.2 | 0 / 0 |
| `now` (8:17 AM) | 15 m | 169 | 23.1 | 13.2 (57.0%) | 13.9 (59.7%) | −8 dB | +11 dB | 95 | 29.7–30.2 | 0 / 0 |
| `now` (8:17 AM) | 20 m | 169 | 33.7 | 18.7 (55.7%) | 19.6 (58.0%) | −3 dB | +11 dB | 95 | 29.7–30.2 | 0 / 0 |
| `now` (8:17 AM) | 40 m | 169 | 19.8 | 13.2 (66.4%) | 13.5 (68.2%) | −6 dB | +11 dB | 95 | 29.7–30.2 | 0 / 0 |
| `1500z` (9:00 AM) | 10 m | 240 | 25.6 | 14.0 (54.5%) | 14.7 (57.4%) | −11 dB | +11 → +8 dB | 34 | 29.7–30.1 | 0 / 0 |
| `1500z` (9:00 AM) | 15 m | 240 | 28.7 | 16.8 (58.4%) | 17.6 (61.2%) | −9 dB | +11 → +8 dB | 34 | 29.7–30.1 | 0 / 0 |
| `1500z` (9:00 AM) | 20 m | 240 | 38.2 | 21.9 (57.1%) | 22.6 (58.9%) | −1 dB | +11 → +8 dB | 34 | 29.7–30.1 | 0 / 0 |
| `1500z` (9:00 AM) | 40 m | 240 | 10.6 | 6.8 (63.6%) | 7.0 (66.0%) | −10 dB | +11 → +8 dB | 34 | 29.7–30.1 | 0 / 0 |
| `1600z` (10:00 AM) | 10 m | 240 | 19.0 | 12.0 (62.9%) | 12.4 (65.3%) | −10 dB | +17 → +8 dB | 101 | 29.3–30.1 | 0 / 0 |
| `1600z` (10:00 AM) | 15 m | 240 | 25.9 | 15.9 (61.5%) | 16.6 (64.0%) | −9 dB | +17 → +8 dB | 101 | 29.3–30.1 | 0 / 0 |
| `1600z` (10:00 AM) | 20 m | 240 | 40.5 | 23.1 (57.0%) | 23.9 (58.9%) | −2 dB | +17 → +8 dB | 101 | 29.3–30.1 | 0 / 0 |
| `1600z` (10:00 AM) | 40 m | 240 | 3.3 | 2.1 (63.4%) | 2.2 (66.2%) | −13 dB | +17 → +8 dB | 101 | 29.3–30.1 | 0 / 0 |
| `1700z` (11:00 AM) | 10 m | 240 | 9.1 | 6.1 (66.2%) | 6.3 (68.9%) | −11 dB | +11 → +8 dB | 608 | 29.1–29.8 | 0 / 0 |
| `1700z` (11:00 AM) | 15 m | 240 | 22.8 | 14.8 (64.7%) | 15.4 (67.6%) | −10 dB | +11 → +8 dB | 608 | 29.1–29.8 | 0 / 0 |
| `1700z` (11:00 AM) | 20 m | 240 | 40.1 | 22.5 (56.0%) | 23.2 (57.7%) | −5 dB | +11 → +8 dB | 608 | 29.1–29.8 | 0 / 0 |
| `1700z` (11:00 AM) | 40 m | 240 | 1.6 | 1.2 (74.7%) | 1.3 (78.7%) | −7 dB | +11 → +8 dB | 608 | 29.1–29.8 | 0 / 0 |
| `1800z` (12:01 PM) | 10 m | 240 | 3.6 | 2.4 (67.0%) | 2.6 (71.3%) | −14 dB | +8 dB | 19 | 28.9–29.6 | 0 / 0 |
| `1800z` (12:01 PM) | 15 m | 240 | 19.6 | 14.1 (71.9%) | 14.6 (74.3%) | −9 dB | +8 dB | 19 | 28.9–29.6 | 0 / 0 |
| `1800z` (12:01 PM) | 20 m | 240 | 35.4 | 19.8 (56.0%) | 20.6 (58.0%) | −6 dB | +8 dB | 19 | 28.9–29.6 | 0 / 0 |
| `1800z` (12:01 PM) | 40 m | 240 | 0.5 | 0.4 (71.7%) | 0.4 (72.4%) | −10 dB | +8 dB | 19 | 28.9–29.6 | 0 / 0 |
| `1900z` (1:01 PM) | 10 m | 240 | 2.3 | 1.7 (71.7%) | 1.8 (75.3%) | −13 dB | +8 dB | 0 | 28.9–29.6 | 0 / 0 |
| `1900z` (1:01 PM) | 15 m | 240 | 21.8 | 15.5 (70.9%) | 15.9 (72.7%) | −9 dB | +8 dB | 0 | 28.9–29.6 | 0 / 0 |
| `1900z` (1:01 PM) | 20 m | 240 | 48.6 | 24.0 (49.4%) | 24.9 (51.3%) | −4 dB | +8 dB | 0 | 28.9–29.6 | 0 / 0 |
| `1900z` (1:01 PM) | 40 m | 240 | 1.5 | 0.9 (55.4%) | 0.9 (56.5%) | −16 dB | +8 dB | 0 | 28.9–29.6 | 0 / 0 |
| `2000z` (2:01 PM) | 10 m | 240 | 5.6 | 3.9 (69.7%) | 4.1 (72.1%) | −13 dB | +8 dB | 2 | 28.7–29.3 | 0 / 0 |
| `2000z` (2:01 PM) | 15 m | 240 | 28.5 | 18.2 (63.9%) | 18.9 (66.2%) | −6 dB | +8 dB | 2 | 28.7–29.3 | 0 / 0 |
| `2000z` (2:01 PM) | 20 m | 240 | 54.3 | 27.0 (49.7%) | 27.8 (51.1%) | −2 dB | +8 dB | 2 | 28.7–29.3 | 0 / 0 |
| `2000z` (2:01 PM) | 40 m | 240 | 3.5 | 2.0 (57.3%) | 2.1 (60.1%) | −14 dB | +8 dB | 2 | 28.7–29.3 | 0 / 0 |
| `2100z` (3:01 PM) | 10 m | 240 | 6.6 | 4.7 (70.7%) | 4.9 (74.5%) | −13 dB | +8 dB | 14 | 28.6–29.1 | 0 / 0 |
| `2100z` (3:01 PM) | 15 m | 240 | 26.3 | 17.1 (64.8%) | 17.7 (66.7%) | −6 dB | +8 dB | 14 | 28.6–29.1 | 0 / 0 |
| `2100z` (3:01 PM) | 20 m | 240 | 50.9 | 25.4 (49.7%) | 26.4 (51.7%) | −2 dB | +8 dB | 14 | 28.6–29.1 | 0 / 0 |
| `2100z` (3:01 PM) | 40 m | 240 | 11.7 | 7.9 (67.2%) | 8.2 (69.7%) | −8 dB | +8 dB | 14 | 28.6–29.1 | 0 / 0 |
| `2200z` (4:02 PM) | 10 m | 240 | 5.8 | 3.8 (65.6%) | 4.0 (69.4%) | −13 dB | +8 dB | 169 | 28.6–29.4 | 0 / 0 |
| `2200z` (4:02 PM) | 15 m | 240 | 29.4 | 18.6 (62.9%) | 19.3 (65.3%) | −6 dB | +8 dB | 169 | 28.6–29.4 | 0 / 0 |
| `2200z` (4:02 PM) | 20 m | 240 | 50.6 | 25.8 (50.7%) | 26.6 (52.4%) | −2 dB | +8 dB | 169 | 28.6–29.4 | 0 / 0 |
| `2200z` (4:02 PM) | 40 m | 240 | 21.8 | 13.3 (61.1%) | 13.8 (63.4%) | −5 dB | +8 dB | 169 | 28.6–29.4 | 0 / 0 |
| `2300z` (5:02 PM) | 10 m | 240 | 2.4 | 1.6 (68.1%) | 1.7 (71.0%) | −13 dB | +8 dB | 80 | 28.6–29.4 | 0 / 0 |
| `2300z` (5:02 PM) | 15 m | 240 | 16.7 | 10.6 (62.6%) | 11.1 (65.4%) | −9 dB | +8 dB | 80 | 28.6–29.4 | 0 / 0 |
| `2300z` (5:02 PM) | 20 m | 240 | 48.8 | 24.4 (49.9%) | 25.4 (51.8%) | −2 dB | +8 dB | 80 | 28.6–29.4 | 0 / 0 |
| `2300z` (5:02 PM) | 40 m | 240 | 34.8 | 20.4 (58.3%) | 21.1 (60.2%) | −3 dB | +8 dB | 80 | 28.6–29.4 | 0 / 0 |

**Our share of `jt9` is the survey's.** It falls where a band is crowded: 20 m at 44 decodes a slot over the day found 52.6%, and 49–51% from 1 PM on at 49–54 a slot, the plateau the QMX and the survey showed (our decoder levels off near 25–27 a slot; [decode.md](../../../../docs/architecture/decode.md), **Subtraction**). Lower too in the morning on 10 and 15 m, 53–58% at 18–29 a slot, below the survey's 64–65% at 6–22. On quieter bands and hours it is mostly 60–75%. The gap is the decoder's, which rows [kkap](../../../../docs/ROADMAP.md#kkap) (subtraction from the audio) and [qauk](../../../../docs/ROADMAP.md#qauk) (AP decoding) work on.

**40 m was nearly dead at midday:** 0.5 to 3.5 `jt9` decodes a slot from 10 AM to 3 PM CST, against 11 to 35 at the ends of the day; 10 m fell from 26 a slot at 9 AM to 2–7 from noon. The bands, not the radio: the same hours on 15 and 20 m held 19–54.

## Against the one-band survey

The same clock window (Central time) as each band of the [2026-11-04 survey](../hl2-2026-11-04/README.md), 120 slots each, taken from this run's streams (2000z, 2100z and 2200z) and scored alone. 40 m has none: the four-band run ended at 6:02 PM, the survey's 40 m ran 6:00–6:30 PM.

| Band | Window (CST) | `jt9` per slot, one band → four | Ours, share of `jt9` | `jt9` SNR median | LNA, one band / four |
| --- | --- | --- | --- | --- | --- |
| 10 m | 2:21–2:51 PM | 5.7 → 6.0 | 64.4% → 69.8% | −14 → −12 dB | +17 → +14 / +8 dB |
| 15 m | 3:22–3:52 PM | 22.2 → 25.4 | 65.4% → 65.7% | −7 → −6 dB | +17 → +14 / +8 dB |
| 20 m | 4:22–4:52 PM | 50.0 → 51.7 | 48.4% → 49.9% | −2 → −2 dB | +14 → +11 / +8 dB |

**The shared 10 m low-pass and the LNA at +8 dB cost these bands nothing these counts can see.** Four bands at once decoded as many as one band at a time, 3 to 14% more, with our share of `jt9` the same or higher. These are different days, so the band moves the counts as much as the radio could; a cost over about 10% would show, a smaller one would not. On this antenna only: not a claim about another antenna, or about a quiet 10 m on a short one, where the low-pass passing all of HF and the LNA's lower gain could matter.

## The LNA at +20 and +14 dB, 40 m after dark

One receiver on 40 m, 6:30–7:30 PM CST, its own low-pass; the LNA held, not stepped, at +20 dB for two slots, then +14 dB for two, 120 slots an arm (`score_ab.py --arm-field lna_arm_db`, each turn paired with the next).

| LNA | Slots | `jt9` per slot | Ours per slot (share of `jt9`) | t4/f2 | `jt9` SNR median | Clipped status words (of the arm's) |
| --- | --- | --- | --- | --- | --- | --- |
| +20 dB | 120 | 48.8 | 24.1 (49.1%) | 25.1 (51.1%) | −3 dB | 42.0%, median 1,181 a slot of 2,857 |
| +14 dB | 120 | 49.0 | 24.0 (48.6%) | 25.1 (51.0%) | −3 dB | 23.1%, median 674 |

Paired, +20 minus +14 dB per turn (60 pairs): `jt9` −0.43 ± 1.06 decodes a slot, ours +0.13 ± 0.71 (one standard error). **6 dB of gain made no difference these counts can see** (two standard errors are about 4% of `jt9`'s count), though at +20 dB the ADC flagged a clip in 42% of its status words, 42 times the rule's 1% trigger. Lost / late packets 0 / 0; HL2 28.6–29.4 °C.

## What this settles

* **The shared filter's cost:** on 10, 15 and 20 m with this antenna, four receivers through the 10 m low-pass decoded as one receiver through each band's own filter did, within about 10%. Which four bands to listen to is then a choice of activity and antenna (row [vjur](../../../../docs/roadmap/ideas.md#vjur)).
* **The LNA's first second and its steps down cost nothing measured:** from +5 dB (the survey's 40 m) to +20 dB, decodes did not move on these bands, and the rule's 1% clip trigger is far more sensitive than decoding needs: 40 m decoded the same with 42% of status words flagged. So neither ignoring the start-up second nor stepping back up when a band quiets would gain anything this measures; both are declined (rows [ujnx](../../../../docs/roadmap/declined.md#ujnx) and [zdmr](../../../../docs/roadmap/declined.md#zdmr)).
* **Our decoder's share is unchanged by the radio**, so its gap to `jt9` is the decoder's own.

**The image check does not apply**, as in the survey: `score_ab.py`'s wrong-side decodes are real stations below the dial on a direct-sampling radio, not images; its lines in each `score.txt` are left out here.

**Timing:** each slot's first packet arrived within −1 to +5 ms of its UTC boundary (one at +22 ms), the HL2's sample clock within a few ppm of the laptop's.

**Which recorder ran.** `now` and `1500z` record `adv-last-241-gd7ac94a-dirty`: the sequence-wrap fix as it stood uncommitted; the streams from `1600z` and the A/B record `adv-last-243-gd449103`, that fix committed (#45 merged it as `40c9d30`). Each manifest's `recorder` is `git describe` of the checkout at the stream's start.
