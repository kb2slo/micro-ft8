# I/Q front end, offline

Runs `components/iq_frontend`, the receive front end the firmware will use (row y9bt), over a
recording, and makes synthetic I/Q to test it with. How the front end works and what it measured:
[docs/architecture/decode.md](../../docs/architecture/decode.md) (**I/Q front end**).

```bash
make -C tools/iq_convert

# A QMX IQ-mode recording (48 kHz stereo, 16- or 24-bit) to 12 kHz audio. Two channels from one
# recording: FT8 and FT4 on 20 m are 6 kHz apart, so 13,500 and 19,500 Hz.
tools/iq_convert/iq_convert --center-hz 13500 --center-hz 19500 -o out.wav qmx_iq.wav

# The same radio's ordinary (demodulated) audio, through the same filters, for the A/B.
tools/iq_convert/iq_convert --real -o audio_12k.wav qmx_audio.wav

# Synthetic I/Q from a 12 kHz corpus WAV, with a mixer imbalance and another slot in the mirror.
tools/iq_convert/iq_convert --synth --gain-db 0.5 --phase-deg 2 --mirror-wav other.wav -o iq.wav slot.wav
```

The output is 16-bit mono, which `tools/decode_recall` and `jt9` read. Each output prints an `OUT`
line with the NCO frequency it applied (`nco_hz`, the trim included) and any clipped samples;
`--synth` prints a `SYNTH` line with the image level its imbalance gives, measured through the
front end. `iq_convert` with no arguments lists every option.

**What a first QMX recording has to settle**, each a flag: which channel carries I (`--swap-iq`),
Q's sign (`--negate-q`), and the unit's IF error (`--if-trim-hz`). With either of the first two
wrong the band comes out of the far side of the LO and nothing decodes; a trim error moves every
decoded frequency by that many Hz.
