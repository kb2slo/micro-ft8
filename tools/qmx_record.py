#!/usr/bin/env python3
"""Record a QMX's USB audio as one WAV per 15 s FT8 slot, for row y9bt's offline A/B.

Stage 2 of row y9bt (docs/ROADMAP.md) records the QMX in IQ mode and in its normal audio
mode, then scores both against `jt9`. This script is the recorder. By default it records
whatever the QMX sends and never switches its mode: set IQ or audio mode on the radio by
hand and say which with --mode-tag.

With --alternate N (needs --port) it switches the mode itself, `Q9 1;` or `Q9 0;`, every N
slots, starting with --mode-tag. The switch goes out 0.5 s before a slot boundary, in the
tail of a slot that FT8 does not use, so no slot starts mid-switch. N=2 gives each mode an
even and an odd slot in turn, so both hear the same stations under the same conditions.
Bench-verified 2026-11-02 on QMX 1_04_010: the switch applies at once and the USB audio
stream keeps every frame across it.

Runs on any computer with the QMX's USB-C plugged straight in: it enumerates as an audio
device and a serial port. Needs `sounddevice` (PortAudio) and `pyserial`:

  python3 -m pip install sounddevice pyserial

The workstation clock must be synced to well under a second (NTP, or your OS's time
sync): files are cut on UTC slot boundaries, and `jt9`'s DT shows any offset. Check the
sync before each hour.

  python3 tools/qmx_record.py --list-devices

One IQ hour, then one audio hour (the serial port is e.g. COM5, /dev/ttyACM0 or
/dev/cu.usbmodem1234; --out is anywhere outside the repository):

  python3 tools/qmx_record.py --device QMX --port PORT --mode-tag iq    --minutes 60 --out DIR
  python3 tools/qmx_record.py --device QMX --port PORT --mode-tag audio --minutes 60 --out DIR

Or both in one run, two slots of each in turn:

  python3 tools/qmx_record.py --device QMX --port PORT --mode-tag iq --alternate 2 --minutes 30 --out DIR

On Linux, ModemManager probes the QMX's serial ports when it connects, and CAT then gives no
reply until the QMX restarts. Tell it to ignore the QMX (0483:a34c, which a QDX shares) once:

  sudo tee /etc/udev/rules.d/99-qmx-no-modemmanager.rules <<'EOF'
  ATTRS{idVendor}=="0483", ATTRS{idProduct}=="a34c", ENV{ID_MM_DEVICE_IGNORE}="1"
  EOF
  sudo udevadm control --reload-rules

then power-cycle the QMX. macOS and Windows have no ModemManager.

Without --port, pass the dial frequency with --dial-hz; it is recorded as given.

48,000 Hz, 2 channels, 24-bit. The stream starts on the next 15 s UTC boundary and is cut
every 720,000 frames into `YYMMDD_HHMMSS.wav` (WSJT-X's naming, the slot's nominal UTC
start). The cut is by frame count on one continuous stream, so no frame is dropped or
duplicated at a cut; over an hour the radio's sample clock and UTC drift apart, which
each slot's `first_frame_arrived_utc` in the manifest shows. A file is written as
`.wav.part` and renamed when its slot is complete.

Ctrl-C once: finish the current slot, then stop. Twice: stop now, and delete the
unfinished `.wav.part`.

`jt9` reads 12 kHz mono: tools/iq_convert (its README) turns these files into that, the
IQ ones through the front end and the audio ones with --real.

`manifest.jsonl` in --out gets one line per finished slot. With --port, the IQ-mode
state is read with `Q9;` at each slot start (documented by SteffenLav/qmx-panadapter and
bench-verified here; the raw reply is stored) and the dial with `FA;`.

Host test, no audio device needed:

  python3 tools/test_qmx_record.py
"""
import argparse
import datetime
import hashlib
import json
import math
import os
import queue
import re
import subprocess
import sys
import threading
import time

SAMPLE_RATE = 48000
CHANNELS = 2
SAMPLE_WIDTH = 3  # bytes: 24-bit
FRAME_BYTES = CHANNELS * SAMPLE_WIDTH
SLOT_S = 15
FRAMES_PER_SLOT = SAMPLE_RATE * SLOT_S  # 720,000
LEAD_S = 1.0  # open the stream at least this long before the first boundary
MANIFEST = "manifest.jsonl"


def next_boundary(t):
    """The first 15 s UTC boundary at or after epoch seconds t."""
    return math.ceil(t / SLOT_S) * SLOT_S


def utc_iso(t):
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).isoformat(timespec="milliseconds")


def slot_name(t):
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime("%y%m%d_%H%M%S.wav")


def q9_mode(reply):
    """'iq', 'audio', or None when the Q9; reply is not one the reference project documents."""
    m = re.fullmatch(r"Q9\s*([01]);", reply or "")
    return {"1": "iq", "0": "audio"}[m.group(1)] if m else None


SWITCH_LEAD_S = 0.5  # --alternate: send Q9 this long before the boundary


def mode_for(index, first_tag, alternate):
    """The mode slot `index` is recorded in: first_tag, or turns of `alternate` slots each."""
    if not alternate:
        return first_tag
    other = "audio" if first_tag == "iq" else "iq"
    return first_tag if (index // alternate) % 2 == 0 else other


def switch_plan(start, slots, first_tag, alternate):
    """[(epoch seconds, mode)] for each switch: the first before slot 0, then one per turn."""
    if not alternate:
        return []
    n = slots if slots is not None else 10**9
    return [(start + i * SLOT_S - SWITCH_LEAD_S, mode_for(i, first_tag, alternate))
            for i in range(0, n, alternate)]


def recorder_version():
    here = os.path.dirname(os.path.abspath(__file__))
    try:
        out = subprocess.run(["git", "-C", here, "describe", "--always", "--dirty"],
                             capture_output=True, text=True, timeout=10)
        return out.stdout.strip() or "unknown"
    except (OSError, subprocess.SubprocessError):
        return "unknown"


class Cat:
    """CAT over the QMX's serial port, as tools/qmx_swr_probe.py talks to it. Reads only,
    except set_iq(), which --alternate uses. Both threads share it, hence the lock."""

    def __init__(self, port):
        import serial  # pyserial; imported here so the host test needs neither dependency
        self.ser = serial.Serial(port, 115200, timeout=0.05)
        self.lock = threading.Lock()
        time.sleep(0.3)

    def set_iq(self, mode):
        """Q9 1; for 'iq', Q9 0; for 'audio'. The QMX sends no reply to a set."""
        self.query("Q9 1;" if mode == "iq" else "Q9 0;", wait=0.1)

    def query(self, cmd, wait=0.5):
        """Send one command; return the reply up to the last ';', or '' if none came."""
        with self.lock:
            return self._query(cmd, wait)

    def _query(self, cmd, wait):
        self.ser.reset_input_buffer()
        t0 = time.monotonic()
        self.ser.write(cmd.encode())
        buf = b""
        while time.monotonic() - t0 < wait:
            buf += self.ser.read(self.ser.in_waiting or 1)
            if buf.endswith(b";"):
                break
        # The radio sometimes appends terminal display codes (ESC[7m ...) after a reply.
        return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", buf.decode(errors="replace")).strip()

    def read_q9(self):
        return self.query("Q9;")

    def read_dial_hz(self):
        m = re.fullmatch(r"FA(\d{11});", self.query("FA;"))
        return int(m.group(1)) if m else None

    def close(self):
        self.ser.close()


class SlotWriter:
    """Cuts one continuous stream of 24-bit stereo frames into one WAV per slot.

    Not thread-safe: one thread calls feed(), request_stop() and abort().
    """

    def __init__(self, out_dir, first_start, slots, mode_tag, cat=None, dial_hz=None,
                 version="unknown", log=print, alternate=None):
        self.out_dir = out_dir
        self.first_start = first_start
        self.slots = slots  # None: until stopped
        self.mode_tag = mode_tag  # with alternate, the first turn's mode
        self.alternate = alternate
        self.cat = cat
        self.dial_hz = dial_hz
        self.version = version
        self.log = log
        self.index = 0  # the slot being written, or the next one
        self.cur = None
        self.stop_requested = False
        self.finished = False
        self.aborted = False
        self.written = []  # manifest records of finished slots

    def feed(self, data, overflowed, arrived):
        """One block from the stream. An overflow counts against the slot its first frame lands in."""
        if self.finished:
            return
        if len(data) % FRAME_BYTES:
            raise ValueError(f"block of {len(data)} bytes is not whole frames")
        mv = memoryview(data)
        first = True
        while mv:
            if self.cur is None:
                self._open(arrived)
            if first and overflowed:
                self.cur["overflows"] += 1
            first = False
            n = min(len(mv) // FRAME_BYTES, FRAMES_PER_SLOT - self.cur["frames"])
            self.cur["wav"].writeframesraw(mv[:n * FRAME_BYTES])
            self.cur["frames"] += n
            mv = mv[n * FRAME_BYTES:]
            if self.cur["frames"] == FRAMES_PER_SLOT:
                self._close()
                if self.finished:
                    return

    def request_stop(self):
        """Finish the slot being written, then stop; with none open, stop now."""
        self.stop_requested = True
        if self.cur is None:
            self.finished = True

    def abort(self):
        """Stop now; the unfinished slot's .part file is deleted and gets no manifest line."""
        if self.cur is not None:
            self.cur["wav"].close()
            self.cur["fh"].close()
            os.remove(self.cur["part"])
            self.log(f"aborted: {os.path.basename(self.cur['final'])} not kept "
                     f"({self.cur['frames']} of {FRAMES_PER_SLOT} frames)")
            self.cur = None
        self.aborted = True
        self.finished = True

    def _open(self, arrived):
        import wave
        nominal = self.first_start + self.index * SLOT_S
        final = os.path.join(self.out_dir, slot_name(nominal))
        if os.path.exists(final):
            raise FileExistsError(f"{final} already exists; not overwriting it")
        part = final + ".part"
        fh = open(part, "xb")
        wav = wave.open(fh, "wb")
        wav.setnchannels(CHANNELS)
        wav.setsampwidth(SAMPLE_WIDTH)
        wav.setframerate(SAMPLE_RATE)
        q9 = dial = None
        dial_source = "given" if self.dial_hz is not None else None
        if self.cat is not None:
            q9 = self.cat.read_q9()
            dial = self.cat.read_dial_hz()
            if dial is not None:
                dial_source = "cat"
            else:
                self.log(f"warning: no FA; reply at {slot_name(nominal)}; "
                         + ("recording --dial-hz as given" if self.dial_hz is not None else "dial unknown"))
            want = mode_for(self.index, self.mode_tag, self.alternate)
            if q9_mode(q9) is not None and q9_mode(q9) != want:
                self.log(f"warning: Q9; answered {q9!r} ({q9_mode(q9)}) but this slot is {want}")
        self.cur = {
            "mode_tag": mode_for(self.index, self.mode_tag, self.alternate),
            "fh": fh, "wav": wav, "part": part, "final": final, "frames": 0, "overflows": 0,
            "nominal": nominal, "arrived": arrived, "q9": q9,
            "dial_hz": dial if dial is not None else self.dial_hz, "dial_source": dial_source,
        }

    def _close(self):
        c = self.cur
        self.cur = None
        c["wav"].close()  # patches the header's frame count
        c["fh"].close()
        h = hashlib.sha256()
        with open(c["part"], "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        os.replace(c["part"], c["final"])
        rec = {
            "file": os.path.basename(c["final"]),
            "nominal_utc_start": utc_iso(c["nominal"]),
            "first_frame_arrived_utc": utc_iso(c["arrived"]),
            "mode_tag": c["mode_tag"],
            "q9_reply": c["q9"],
            "dial_hz": c["dial_hz"],
            "dial_source": c["dial_source"],
            "sample_rate": SAMPLE_RATE,
            "channels": CHANNELS,
            "sample_width_bytes": SAMPLE_WIDTH,
            "frames": c["frames"],
            "overflows": c["overflows"],
            "sha256": h.hexdigest(),
            "recorder": self.version,
        }
        with open(os.path.join(self.out_dir, MANIFEST), "a", encoding="utf-8") as f:
            f.write(json.dumps(rec) + "\n")
            f.flush()
            os.fsync(f.fileno())
        self.written.append(rec)
        late = c["arrived"] - c["nominal"]
        self.log(f"{rec['file']}  frames {rec['frames']}  overflows {rec['overflows']}  "
                 f"dial {rec['dial_hz']} ({rec['dial_source']})  Q9 {rec['q9_reply']!r}  "
                 f"first block {late * 1000:+.0f} ms")
        self.index += 1
        if self.stop_requested or (self.slots is not None and self.index >= self.slots):
            self.finished = True


class Recorder:
    """The stream's callback only queues; a writer thread runs the SlotWriter."""

    def __init__(self, open_stream, writer_args, clock=time.time, wait=None):
        self.open_stream = open_stream  # callable(callback) -> object with start(), stop(), close()
        self.writer_args = writer_args
        self.clock = clock
        self.wait = wait  # callable(done_event); default waits on it. Ctrl-C arrives here.
        self.q = queue.Queue()
        self.done = threading.Event()
        self.error = None
        self.start_utc = None
        self.writer = None

    def callback(self, indata, frames, time_info, status):
        arrived = self.clock()
        data = bytes(indata)
        if arrived < self.start_utc:
            return
        # The block that crosses the boundary: keep only the frames from it on, estimating
        # capture times from the arrival time (its last frame) back at the sample rate.
        n = len(data) // FRAME_BYTES
        block_start = arrived - n / SAMPLE_RATE
        if block_start < self.start_utc and not self._started:
            skip = min(n, round((self.start_utc - block_start) * SAMPLE_RATE))
            data = data[skip * FRAME_BYTES:]
        self._started = True
        if data:
            self.q.put(("data", data, bool(status and status.input_overflow), arrived))

    def _write_loop(self):
        w = self.writer
        try:
            while not w.finished:
                item = self.q.get()
                if item[0] == "data":
                    w.feed(item[1], item[2], item[3])
                elif item[0] == "stop":
                    w.request_stop()
                elif item[0] == "abort":
                    w.abort()
        except Exception as e:  # noqa: BLE001 - reported by run()
            self.error = e
            try:
                w.abort()
            except Exception:  # noqa: BLE001
                pass
        finally:
            self.done.set()

    def _switch_loop(self, plan, cat):
        """--alternate: send each planned Q9 at its time, until recording is done."""
        for when, mode in plan:
            if self.done.wait(max(0.0, when - self.clock())):
                return
            cat.set_iq(mode)

    def run(self):
        """Record until the slot count is reached or Ctrl-C. Returns the manifest records."""
        self._started = False
        self.start_utc = next_boundary(self.clock() + LEAD_S)
        self.writer = SlotWriter(first_start=self.start_utc, **self.writer_args)
        log = self.writer.log
        w = self.writer
        log(f"starting at {utc_iso(self.start_utc)} (mode tag {w.mode_tag}"
            + (f", alternating every {w.alternate} slots)" if w.alternate else ")"))
        stream = self.open_stream(self.callback)
        thread = threading.Thread(target=self._write_loop, name="qmx-record-writer", daemon=True)
        thread.start()
        if w.alternate:
            plan = switch_plan(self.start_utc, w.slots, w.mode_tag, w.alternate)
            threading.Thread(target=self._switch_loop, args=(plan, w.cat), name="qmx-record-switch",
                             daemon=True).start()
        stream.start()
        interrupts = 0
        try:
            while not self.done.is_set():
                try:
                    (self.wait or (lambda ev: ev.wait(0.5)))(self.done)
                except KeyboardInterrupt:
                    interrupts += 1
                    if interrupts == 1:
                        log("Ctrl-C: finishing this slot, then stopping (Ctrl-C again to stop now)")
                        self.q.put(("stop",))
                    else:
                        self.q.put(("abort",))
        finally:
            stream.stop()
            stream.close()
            if not self.done.is_set():
                self.q.put(("abort",))
            thread.join()
        if self.error is not None:
            raise self.error
        return self.writer.written


def list_devices():
    import sounddevice as sd
    for i, d in enumerate(sd.query_devices()):
        if d["max_input_channels"] > 0:
            host = sd.query_hostapis(d["hostapi"])["name"]
            print(f"{i:3d}  {d['name']}  ({host}, {d['max_input_channels']} in, "
                  f"default {d['default_samplerate']:.0f} Hz)")


def find_device(substring):
    import sounddevice as sd
    hits = [(i, d) for i, d in enumerate(sd.query_devices())
            if d["max_input_channels"] >= CHANNELS and substring.lower() in d["name"].lower()]
    if len(hits) != 1:
        names = ", ".join(f"{i}: {d['name']}" for i, d in hits) or "none"
        sys.exit(f"--device {substring!r} must match exactly one stereo input (matched {names}); "
                 "see --list-devices, or pass its number")
    return hits[0][0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list-devices", action="store_true", help="list input devices and exit")
    ap.add_argument("--device", help="input device: a name substring, or its number from --list-devices")
    ap.add_argument("--out", help="output directory (required to record; never inside git)")
    ap.add_argument("--mode-tag", choices=("iq", "audio"), help="the mode the QMX was set to by hand")
    ap.add_argument("--port", help="the QMX's serial port, for Q9; and FA; at each slot start")
    ap.add_argument("--dial-hz", type=int, help="dial frequency, recorded as given when CAT gives none")
    ap.add_argument("--alternate", type=int, metavar="N",
                    help="switch IQ mode over CAT every N slots, starting with --mode-tag (needs --port)")
    n = ap.add_mutually_exclusive_group()
    n.add_argument("--slots", type=int, help="stop after this many slots")
    n.add_argument("--minutes", type=float, help="stop after this many minutes of slots")
    args = ap.parse_args()

    if args.list_devices:
        list_devices()
        return
    missing = [f for f in ("device", "out", "mode_tag") if getattr(args, f) is None]
    if missing:
        ap.error("recording needs " + ", ".join("--" + m.replace("_", "-") for m in missing))
    slots = args.slots if args.slots is not None else (
        math.ceil(args.minutes * 60 / SLOT_S) if args.minutes is not None else None)
    if slots is not None and slots < 1:
        ap.error("--slots/--minutes must give at least one slot")
    if args.alternate is not None and (args.alternate < 1 or not args.port):
        ap.error("--alternate needs N of 1 or more, and --port to switch the mode with")

    import sounddevice as sd
    device = int(args.device) if args.device.isdigit() else find_device(args.device)
    os.makedirs(args.out, exist_ok=True)
    cat = Cat(args.port) if args.port else None

    def open_stream(callback):
        return sd.RawInputStream(samplerate=SAMPLE_RATE, channels=CHANNELS, dtype="int24",
                                 device=device, latency="high", callback=callback)

    rec = Recorder(open_stream, dict(out_dir=args.out, slots=slots, mode_tag=args.mode_tag, cat=cat,
                                     dial_hz=args.dial_hz, version=recorder_version(),
                                     alternate=args.alternate))
    try:
        written = rec.run()
    finally:
        if cat is not None:
            cat.close()
    total_over = sum(r["overflows"] for r in written)
    print(f"{len(written)} slot(s) written to {args.out}, {total_over} overflow(s)")


if __name__ == "__main__":
    main()
