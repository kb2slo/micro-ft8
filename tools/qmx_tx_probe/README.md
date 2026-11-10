# QMX transmit probe

Keys a QMX over CAT from a computer, the way the main board does (`poe_p4/main/tx_scheduler.cpp`), and reads who
heard it from PSKReporter's live feed through the web app's own listener. It measured that a QMX transmits a `TA`
tone far from its dial ([radio.md](../../docs/architecture/radio.md), **TX timing on a QMX**), and reruns that
measurement on another firmware or band.

These are real transmissions: run them only as, or beside, the station's control operator, on a frequency your
licence allows. The script refuses anything outside 20 m and 40 m.

## Setup

The QMX on this computer's USB instead of the main board's, antenna connected. Python 3 with pyserial, Node 22 or
later (built-in `WebSocket`), and a C compiler for the tone printer, from the repository root:

```bash
cc -O2 -std=gnu11 -w -Icomponents/ft8_lib -Icomponents/ft8_lib/vendor -o tools/qmx_tx_probe/ftx_tones \
  tools/qmx_tx_probe/ftx_tones.c components/ft8_lib/vendor/ft8/{message,text,encode,constants,crc}.c -lm
```

(`ftx_tones.exe` on Windows; it uses the in-tree `ft8_lib`, the encoder the main board uses.)

## Run

Start the listener first: the feed keeps no history. Spots reach it up to about ten minutes after a transmission.

```bash
node tools/qmx_tx_probe/listen.js <CALL>
```

Then a control on the usual tone, and the transmission under test, each previewed with `--dry-run` first:

```bash
python3 tools/qmx_tx_probe/key_qmx.py --port <QMX serial port> --mode FT8 --base 1500 --count 3 --message "CQ <CALL> <GRID>"
python3 tools/qmx_tx_probe/key_qmx.py --port <QMX serial port> --mode FT4 --base 7500 --count 6 --message "CQ <CALL> <GRID>"
```

The dial stays at `--dial` (default 14.074 MHz, FT8's); `--base 7500` puts FT4 at 14.0815 MHz, FT4's 14.080 plus
1,500 Hz. Each spot prints the receiver's reported frequency: a tone the radio clamped would show near the dial plus
the clamp, not where it was aimed. If the control draws no spots, the keying is at fault, not the tone.

## Below the dial: split or retune

A `TA` under 10 Hz keys a QMX up, so a channel below the receive dial (40 m's FT4, 26.5 kHz below FT8's) cannot
go out as a far tone. `--tx-dial` moves the transmission there, `--via` chooses how:

```bash
python3 tools/qmx_tx_probe/key_qmx.py --port <QMX serial port> --mode FT8 --dial 7074000 --base 1500 --count 3 --message "CQ <CALL> <GRID>"
python3 tools/qmx_tx_probe/key_qmx.py --port <QMX serial port> --mode FT4 --dial 7074000 --tx-dial 7047500 --via split --base 1500 --count 6 --message "CQ <CALL> <GRID>"
python3 tools/qmx_tx_probe/key_qmx.py --port <QMX serial port> --mode FT4 --dial 7074000 --tx-dial 7047500 --via retune --base 1500 --count 6 --message "CQ <CALL> <GRID>"
```

* **split:** `SP1;` then `FB<tx-dial>;` a second before the slot, `SP;` read back, and after `RX;` `FB<dial>;` `SP0;`
  `FR0;` `FT0;`, with `SP;` `FR;` `FT;` read back. `FB` is written only while split is on and never read back:
  qmx-panadapter found a QMX refuses `FB` once split is off, and `SP0;` leaves the VFO mode at Split until `FR0;`; the
  CAT manual has `FB;` answer with an `FA`-prefixed reply.
* **retune:** `FA<tx-dial>;` a second before the slot, then `FA;` read back, and `FA<dial>;` after `RX;`.

Each line prints the read-back and how long the set took to read back; a read-back that does not match skips that
transmission, so nothing is keyed at the wrong place. The FT4 spots should read near 7.049000 MHz (7.0475 plus
1,500 Hz). What receive loses after a transmission is not visible from here: the main board measures that.

## What receive loses after a retune

Nothing need be keyed: both dial moves happen while the radio receives. `--no-key` moves the dial at the times an
FT4 transmission would (`FA` to `--tx-dial` a second before the slot, back where `RX;` would go), and
`--every 8` does it once a minute, at :00. Record the radio's I/Q stream alongside, set to IQ mode (`Q9 1;`),
with `tools/qmx_record.py`, which needs no serial port given `--dial-hz`, so the probe keeps CAT to itself:

```bash
python3 tools/qmx_record.py --device QMX --mode-tag iq --dial-hz 7074000 --minutes 15 --out DIR
python3 tools/qmx_tx_probe/key_qmx.py --port <QMX serial port> --mode FT4 --dial 7074000 --tx-dial 7047500 \
  --via retune --no-key --every 8 --count 12 --base 1500 --message "CQ <CALL> <GRID>" | tee probe.log
python3 tools/qmx_tx_probe/score_retune.py DIR probe.log
```

`score_retune.py` decodes each slot's FT8 and FT4 with `jt9` and pairs, within each minute, the slot after the
dial came back with one of the same parity that it did not touch (its docstring says which). The 2026-11-08 run
and why the retune was chosen: [several-channels.md](../../docs/roadmap/several-channels.md), **Decided**.
