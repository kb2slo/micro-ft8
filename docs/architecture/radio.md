# Radio control: backends behind one table of operations

The core (slot loop, TX scheduler, HUD) talks to the radio only through `components/radio_control`. It never asks which radio it is.

## How it works

* **One table of operations per radio**, `radio_control_ops_t` (`components/radio_control/include/radio_control_backend.h`): `ready`, `on_audio_start`, `sync_frequency_mode`, `begin_tx`, `set_tone_hz`, `end_tx`, `set_tune`, `set_time`, the TX power/SWR trio `reset_tx_power_swr` / `poll_tx_power_swr` / `get_tx_power_swr`, and `begin_cpfsk_tx`. A null pointer means "not supported", never a crash.
* **Capabilities** (`radio_control_capabilities_t`, `radio_control.h`) are what the core and HUD ask instead of the radio's name: `has_wattmeter`, `has_swr`, `can_set_time`, `audio_is_uac`, `can_md8_tune`, `can_ps0`.
* **One `.cpp` per radio**: `radio_control_qmx.cpp`, `radio_control_qdx.cpp`. QMX and QMX+ are one backend until their CAT differs.
* **CAT replies are read by one parser**, `cat_reply_parser` (`components/radio_control`, host-tested), fed from each board's CDC callback (`poe_p4/main/qmx_transport.cpp`, `main/audio/stream_uac.cpp`). It keeps the last `PC`, `SW` and `FA` reply, and drops the terminal display codes (`ESC[7m` ...) the radio appends to some replies, which would otherwise glue onto the next reply and hide its prefix. `FA` exists so a sweep can tell a frequency the radio refused (it answers `?;` and `FA;` still reads the old one) from one that took.
* **The product picker** is the table in `main/radio/radio_profile.h`: one row per radio, binding its name, audio source and backend.

## Rules

* **No `if (QMX)` in the core** — not in `main.cpp`, `poe_p4/main`, or the dispatcher in `radio_control.cpp`. Radio-specific behaviour is an operation or a capability.
* **QMX-only features go through a capability flag.** The SWR curve (I8) calls `MD8;` only where `can_md8_tune` is set; a CAT power-off (`PS0;`, I10) only where `can_ps0` is set.
* **Adding a radio** means filling in a new operations table and capabilities, and adding a row to `radio_profile.h` — no edit to the slot loop. This is the design's test; no second radio family has been added yet to prove it.
* **Why a table, not `class Radio`:** the variants are a closed set, and a C struct of function pointers is what the rest of this ESP-IDF tree already uses. [STYLE.md](../STYLE.md) (**Structure**) allows inheritance only for closed sets like this one, and the table is the lighter form of it.

## QMX SWR Tune over CAT

Measured on a QMX at firmware 1_04_003 with `tools/qmx_swr_probe.py` (2026-09-30); `tools/qmx_swr_sweep.py` and `tools/qmx_swr_plot.py` are the bench prototype of the sweep and its plot. **Not yet built as a sweep: row I8**; this is what the probe proved, and what the sweep must follow.

* **`MD8;` enters SWR Tune and keys the radio by itself** (`KD;`/`TQ;` read `1`; `MD;` then reads `3`). `SW;` returns the SWR in hundredths while keyed, and an empty `SW;` in receive. No `TX;` is needed, and a plain `TX;` does not measure: `KD;` stays `0` and `SW;` returns noisy, meaningless values.
* **The frequency is taken when SWR Tune is entered.** `FA` while keyed is refused (`?;`), and `MD8;` sent while already in SWR Tune keeps the old frequency. Per point: `RX;`, `MD6;`, `FA<hz>;`, `MD8;`, then read `SW;`. Afterwards `RX;`, `MD6;` and the original `FA`.
* **Timing:** setters return nothing, and the next query answers within a few ms, so the radio's own latency is small. With a 300 ms discard after `MD8;` the first read was already steady, and three reads 50 ms apart plus a median absorb the odd outlier (one in ~20 reads strays 0.1-0.3). A point costs about 0.7 s keyed: 15 points in 11 s.
* **The reading depends on the Tune %.** The same dummy load on 20 m read 1.14-1.15 at 50%, 1.19-1.22 at 25%, 1.28-1.32 at 10%; at 10% the noise hides the small rise across the band. So 25% is the floor, and a curve must be labelled with its Tune %. The real antenna at 25% read a smooth 1.37-1.43 across 20 m, lowest at 14.175-14.200 MHz.
* **`FA` is limited to the radio's configured bands.** A 6-17 MHz sweep in 200 kHz steps was refused (`?;`) everywhere except inside 40 m (7.000, 7.200) and 20 m (14.000, 14.200); 6.8, 7.4, 13.8 and 14.4 MHz were refused. A CAT sweep is therefore bounded by the band table, not by us. The QMX's own terminal sweep plot spans 6-17 MHz by some other path; unexplained.
* **Not available:** `SR;` (the SWR-protection lock) returns `?;`, `MD0;` is not a mode, and `PC;` is get-only. The CAT timeout is on (`QB1;`), 120 s (`QC120;`), which a sweep's polling keeps alive.
* **The radio appends terminal display codes** (`ESC[7m` and similar) after some replies; a parser must strip them and read a reply by pattern.

