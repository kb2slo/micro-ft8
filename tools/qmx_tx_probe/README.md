# QMX transmit probe

Keys a QMX over CAT from a computer, the way the main board does (`poe_p4/main/tx_scheduler.cpp`), and reads who
heard it from PSKReporter's live feed through the web app's own listener. It measured that a QMX transmits a `TA`
tone far from its dial ([radio.md](../../docs/architecture/radio.md), **TX timing on a QMX**), and reruns that
measurement on another firmware or band.

These are real transmissions: run them only as, or beside, the station's control operator, on a frequency your
licence allows. The script refuses anything outside 20 m.

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
