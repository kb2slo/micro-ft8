#!/usr/bin/env python3
"""Recall report for the firmware decoder (RFC 0006 §2 and §5 P1).

One command, today's firmware settings unless flags say otherwise:

  python3 tools/decode_recall/report.py

Prints recall, SNR-bucketed recall, and discordant counts (missed vs extras)
against committed `jt9 -8 -d 3` truth (WSJT-X 3.0.1), and how far the firmware's
SNR sits from jt9's on the messages both decoded. CI never runs jt9.
`20m_busy` is fetched from howard0su/ft8_lib at 55ccf01 and is not committed.
The synthetic tier is GFSK plus calibrated AWGN; its truth is the message we encoded.

  python3 tools/decode_recall/report.py --write-truth     # needs jt9 on PATH
  python3 tools/decode_recall/report.py --write-baseline  # record CI floors
  python3 tools/decode_recall/report.py --discordant a.log b.log
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import wave
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
IN_TREE = REPO / "components" / "ft8_lib" / "vendor" / "test" / "wav"
TRUTH = HERE / "truth"
CACHE = HERE / ".cache"
HOWARD_URL = "https://github.com/howard0su/ft8_lib.git"
HOWARD_SHA = "55ccf01"
DECODER = HERE / "decode_recall"

def firmware_candidates():
    """The cap decode_monitor_results() uses. One definition, in decode_limits.h."""
    header = (REPO / "components" / "decode_pipeline" / "include" / "decode_limits.h").read_text()
    match = re.search(r"#define DECODE_MAX_CANDIDATES (\d+)", header)
    if not match:
        raise SystemExit("DECODE_MAX_CANDIDATES missing from decode_limits.h")
    return int(match.group(1))


# Firmware as shipped. A zero-slack change to these is a different configuration.
TODAY = {
    "time_osr": 2,
    "freq_osr": 2,
    "candidates": firmware_candidates(),
    "decodes": 32,
    "min_score": 5,
    "ldpc": 25,
    "sample_rate": 6000,
    "agc": "on",
    "levels": 256,
    "subtract_passes": 2,
    "subtract_neighbors": 1,
    "subtract_fraction": 0.5,
}

# CI fails if recall falls more than this many messages under the recorded hits.
# Two libm implementations can move a marginal candidate either way (RFC 0006 §3).
SLACK = {"in_tree": 4, "busy": 8}

# CI fails if the SNR's mean error against jt9 moves more than this from the recorded value,
# or its spread grows more than this. Tenths of a dB.
SNR_SLACK_X10 = 5
# CI fails if the synthetic grid's mean SNR at any level from SNR_CHECK_LOW up is further than
# this from the injected SNR (dB). Below that only the lucky draws decode, so the mean is biased.
SNR_SYNTH_TOLERANCE = 1.5
SNR_CHECK_LOW = -18

LINE = re.compile(
    r"^\s*\d{4,6}\s+([-+]?\d+(?:\.\d+)?)\s+[-+]?\d+\.\d+\s+\d+\s+~\s+(.*?)\s*$"
)
BUCKETS = [(-99, -20), (-19, -16), (-15, -12), (-11, -8), (-7, -4), (-3, 0), (1, 99)]


def normalise(text):
    text = re.sub(r"<[^>]*>", "<...>", text)
    text = re.sub(r"\s+a\d$", "", text)
    return " ".join(text.split())


def parse_messages(stdout):
    """Map normalised text -> strongest SNR on the line. Order is not significant."""
    found = {}
    for line in stdout.splitlines():
        match = LINE.match(line)
        if not match:
            continue
        text = normalise(match.group(2))
        snr = int(float(match.group(1)))
        if text not in found or snr > found[text]:
            found[text] = snr
    return found


def wavs_12k(directory):
    out = []
    for path in sorted(directory.glob("*.wav")):
        with wave.open(str(path)) as handle:
            if handle.getframerate() == 12000:
                out.append(path)
    return out


def fetch_busy():
    """Clone howard0su/ft8_lib and check out the pinned SHA. An abbreviated SHA is not a ref
    GitHub will fetch by itself, so this is a full clone into the gitignored cache."""
    dest = CACHE / "howard"
    if not (dest / ".git").exists():
        if dest.exists():
            subprocess.run(["rm", "-rf", str(dest)], check=True)
        subprocess.run(["git", "clone", "-q", HOWARD_URL, str(dest)], check=True)
    current = subprocess.run(
        ["git", "-C", str(dest), "rev-parse", "HEAD"], capture_output=True, text=True
    )
    if current.returncode != 0 or not current.stdout.startswith(HOWARD_SHA):
        subprocess.run(["git", "-C", str(dest), "checkout", "-q", HOWARD_SHA], check=True)
    got = subprocess.run(
        ["git", "-C", str(dest), "rev-parse", "HEAD"], capture_output=True, text=True, check=True
    ).stdout.strip()
    if not got.startswith(HOWARD_SHA):
        raise SystemExit(f"howard0su/ft8_lib at {got}, want {HOWARD_SHA}")
    wav_dir = dest / "test" / "wav" / "20m_busy"
    if not wav_dir.is_dir():
        raise SystemExit(f"missing {wav_dir}")
    return wav_dir


def jt9_file(path):
    scratch = tempfile.mkdtemp(prefix="jt9_")
    result = subprocess.run(
        ["jt9", "-8", "-d", "3", str(path)],
        capture_output=True, text=True, cwd=scratch,
    )
    if result.returncode != 0:
        raise SystemExit(f"jt9 failed on {path}: {result.stderr}")
    return path, result.stdout


def write_truth(paths, corpus):
    out_dir = TRUTH / corpus
    out_dir.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(max_workers=min(4, len(paths))) as pool:
        for path, stdout in pool.map(jt9_file, paths):
            (out_dir / (path.stem + ".txt")).write_text(stdout)
    print(f"wrote {len(paths)} truth files in {out_dir}", file=sys.stderr)


def load_truth(corpus, wav_path):
    path = TRUTH / corpus / (wav_path.stem + ".txt")
    if not path.is_file():
        raise SystemExit(f"missing {path}; generate it with --write-truth (jt9, not CI)")
    return parse_messages(path.read_text())


def build_decoder():
    subprocess.run(["make", "-C", str(HERE), "-j", str(os.cpu_count() or 2)], check=True)


def decoder_argv(config):
    args = [str(DECODER)]
    for key, value in config.items():
        args.extend([f"--{key.replace('_', '-')}", str(value)])
    return args


def run_decoder(config, wavs):
    if not wavs:
        return {}
    result = subprocess.run(
        decoder_argv(config) + [str(p) for p in wavs],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        raise SystemExit(f"decoder failed ({result.returncode})")
    by_file = {}
    current = None
    chunk = []
    wall = {}
    for line in result.stdout.splitlines():
        if line.startswith("FILE "):
            current = line[5:]
            chunk = []
        elif line.startswith("END "):
            by_file[current] = parse_messages("\n".join(chunk))
            match = re.search(r"wall_ms=(\d+)", line)
            wall[current] = int(match.group(1)) if match else 0
            current = None
        elif line.startswith("FAIL "):
            raise SystemExit(line)
        elif current is not None:
            chunk.append(line)
    missing = [str(p) for p in wavs if str(p) not in by_file]
    if missing:
        raise SystemExit("decoder produced no FILE block for " + missing[0])
    return by_file, wall, result.stdout


def bucket_name(snr):
    for low, high in BUCKETS:
        if low <= snr <= high:
            if low <= -99:
                return "<=-20"
            if high >= 99:
                return ">=1"
            return f"{low}..{high}"
    return "?"


def score_corpus(name, wavs, decoded_by_path):
    truth_n = 0
    hit_n = 0
    extra_n = 0
    buckets = defaultdict(lambda: [0, 0])
    missed = []
    extras = []
    snr_pairs = []  # (jt9's SNR, ours) for every message both decoded
    for wav in wavs:
        truth = load_truth(name, wav)
        got = decoded_by_path[str(wav)]
        truth_n += len(truth)
        hit_n += len(set(got) & set(truth))
        only_extra = set(got) - set(truth)
        extra_n += len(only_extra)
        for text in sorted(set(truth) - set(got)):
            missed.append((wav.name, text, truth[text]))
        for text in sorted(only_extra):
            extras.append((wav.name, text, got[text]))
        for text, snr in truth.items():
            slot = buckets[bucket_name(snr)]
            slot[1] += 1
            if text in got:
                slot[0] += 1
                snr_pairs.append((snr, got[text]))
    return {
        "truth": truth_n,
        "hits": hit_n,
        "extras": extra_n,
        "buckets": buckets,
        "missed": missed,
        "extra_list": extras,
        "snr": snr_error(snr_pairs),
    }


def percentile(values, q):
    ordered = sorted(values)
    rank = (len(ordered) - 1) * q
    low = int(rank)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (rank - low)


def snr_error(pairs):
    """Our SNR minus jt9's over (jt9, ours) pairs: mean, spread, tails, and the slope of ours on jt9's."""
    if not pairs:
        return None
    errors = [ours - truth for truth, ours in pairs]
    mean = sum(errors) / len(errors)
    sd = (sum((e - mean) ** 2 for e in errors) / len(errors)) ** 0.5
    truth_mean = sum(t for t, _ in pairs) / len(pairs)
    ours_mean = sum(o for _, o in pairs) / len(pairs)
    sxx = sum((t - truth_mean) ** 2 for t, _ in pairs)
    slope = sum((t - truth_mean) * (o - ours_mean) for t, o in pairs) / sxx if sxx else 0.0
    return {
        "n": len(errors),
        "mean": mean,
        "sd": sd,
        "p5": percentile(errors, 0.05),
        "p95": percentile(errors, 0.95),
        "within3": sum(abs(e) <= 3 for e in errors) / len(errors),
        "slope": slope,
    }


def print_score(name, score, wall_ms, n_files):
    truth = score["truth"]
    hits = score["hits"]
    recall = 100.0 * hits / truth if truth else 0.0
    mean = wall_ms / n_files if n_files else 0.0
    print(f"{name}: recall {recall:5.1f}% ({hits}/{truth})"
          f"  discordant missed {truth - hits}  extras {score['extras']}"
          f"  {mean:.0f} ms/slot decode")
    for wav_name, text, snr in score["extra_list"][:12]:
        print(f"    extra {wav_name}  {snr:+d} dB  {text}")
    if len(score["extra_list"]) > 12:
        print(f"    ... {len(score['extra_list']) - 12} more extras")
    print("  SNR buckets (jt9's SNR):")
    for low, high in BUCKETS:
        label = bucket_name(low if low > -99 else -20)
        if high >= 99:
            label = ">=1"
        elif low <= -99:
            label = "<=-20"
        else:
            label = f"{low}..{high}"
        got, total = score["buckets"].get(label, [0, 0])
        if total == 0:
            continue
        print(f"    {label:>8}  {100.0 * got / total:5.1f}% ({got}/{total})")
    snr = score["snr"]
    if snr:
        print(f"  SNR, ours minus jt9's over {snr['n']} messages: mean {snr['mean']:+.1f} dB, sd {snr['sd']:.1f},"
              f" p5..p95 {snr['p5']:+.0f}..{snr['p95']:+.0f}, within 3 dB {100.0 * snr['within3']:.0f}%,"
              f" slope {snr['slope']:.2f}")


def synth_jobs(directory):
    """Return (wav path, snr, message) from `decode_recall --synth` stdout saved beside the wavs."""
    manifest = directory / "manifest.tsv"
    jobs = []
    for line in manifest.read_text().splitlines():
        snr, _freq, message, path = line.split("\t")
        jobs.append((Path(path), int(snr), normalise(message)))
    return jobs


def write_synth():
    directory = CACHE / "synth"
    directory.mkdir(parents=True, exist_ok=True)
    # The grid does not depend on decoder flags; it is always the firmware rate.
    result = subprocess.run(
        [str(DECODER), "--synth", str(directory)],
        capture_output=True, text=True, check=True,
    )
    (directory / "manifest.tsv").write_text(result.stdout)
    return synth_jobs(directory)


def score_synth(jobs, decoded_by_path):
    by_snr = defaultdict(lambda: [0, 0, 0, []])  # hits, total, extras, our SNR for each hit
    strong_misses = []
    for path, snr, message in jobs:
        got = decoded_by_path[str(path)]
        slot = by_snr[snr]
        slot[1] += 1
        if message in got:
            slot[0] += 1
            slot[3].append(got[message])
        elif snr >= -6:
            strong_misses.append((path.name, message))
        slot[2] += len(set(got) - {message})
    return by_snr, strong_misses


def synth_snr_misses(by_snr):
    """Levels from SNR_CHECK_LOW up whose mean decoded SNR is off the injected SNR by more than the tolerance."""
    misses = []
    for snr, (_hits, _total, _extras, ours) in sorted(by_snr.items()):
        if snr < SNR_CHECK_LOW or not ours:
            continue
        mean = sum(ours) / len(ours)
        if abs(mean - snr) > SNR_SYNTH_TOLERANCE:
            misses.append((snr, mean))
    return misses


def print_synth(by_snr, strong_misses):
    print("synthetic GFSK + AWGN (truth is the encoded message; SNR is the injected SNR):")
    for snr in sorted(by_snr):
        hits, total, extras, ours = by_snr[snr]
        reads = f"  reads {sum(ours) / len(ours):+.1f} dB" if ours else ""
        print(f"    {snr:4d} dB  {100.0 * hits / total:5.1f}% ({hits}/{total})  extras {extras}{reads}")
    if strong_misses:
        print(f"  strong-signal misses (SNR >= -6): {len(strong_misses)}")
        for name, message in strong_misses[:8]:
            print(f"    {name}  {message}")


def floors_from(scores, strong_misses):
    lines = [
        "# CI floors for today's firmware settings. Hits may sit this many messages",
        "# under the recorded count before the job fails (libm can move a marginal candidate).",
    ]
    for name in ("in_tree", "busy"):
        hits = scores[name]["hits"]
        truth = scores[name]["truth"]
        minimum = hits - SLACK[name]
        lines.append(f"{name}_hits {hits}")
        lines.append(f"{name}_truth {truth}")
        lines.append(f"{name}_min {minimum}")
        lines.append(f"{name}_snr_mean_x10 {round(10 * scores[name]['snr']['mean'])}")
        lines.append(f"{name}_snr_sd_x10 {round(10 * scores[name]['snr']['sd'])}")
    lines.append(f"synth_strong_misses {len(strong_misses)}")
    return "\n".join(lines) + "\n"


def check_floors(path, scores, strong_misses, snr_misses):
    if not path.is_file():
        raise SystemExit(f"missing {path}; run with --write-baseline")
    recorded = {}
    for line in path.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        key, value = line.split()
        recorded[key] = int(value)
    failed = False
    for name in ("in_tree", "busy"):
        hits = scores[name]["hits"]
        minimum = recorded[f"{name}_min"]
        print(f"baseline {name}: {hits} hits, floor {minimum}")
        if hits < minimum:
            print(f"FAIL {name} recall dropped below the floor", file=sys.stderr)
            failed = True
        snr = scores[name]["snr"]
        mean_x10 = round(10 * snr["mean"])
        sd_x10 = round(10 * snr["sd"])
        want_mean = recorded[f"{name}_snr_mean_x10"]
        want_sd = recorded[f"{name}_snr_sd_x10"]
        print(f"baseline {name} SNR vs jt9: mean {mean_x10 / 10:+.1f} dB (recorded {want_mean / 10:+.1f}),"
              f" sd {sd_x10 / 10:.1f} (recorded {want_sd / 10:.1f})")
        if abs(mean_x10 - want_mean) > SNR_SLACK_X10 or sd_x10 > want_sd + SNR_SLACK_X10:
            print(f"FAIL {name} SNR moved away from jt9's", file=sys.stderr)
            failed = True
    for snr, mean in snr_misses:
        print(f"FAIL synthetic {snr:+d} dB reads {mean:+.1f} dB; refit DECODE_SNR_FT8_OFFSET_DB", file=sys.stderr)
        failed = True
    if strong_misses:
        print("FAIL a synthetic signal at SNR >= -6 did not decode", file=sys.stderr)
        failed = True
    if recorded.get("in_tree_truth") != scores["in_tree"]["truth"]:
        print("FAIL in-tree truth count changed; regenerate truth and the baseline", file=sys.stderr)
        failed = True
    if recorded.get("busy_truth") != scores["busy"]["truth"]:
        print("FAIL busy truth count changed; regenerate truth and the baseline", file=sys.stderr)
        failed = True
    return failed


def parse_log(path):
    text = Path(path).read_text()
    by_file = {}
    current = None
    chunk = []
    for line in text.splitlines():
        if line.startswith("FILE "):
            current = Path(line[5:]).name
            chunk = []
        elif line.startswith("END ") and current is not None:
            by_file[current] = parse_messages("\n".join(chunk))
            current = None
        elif current is not None:
            chunk.append(line)
    return by_file


def discordant(path_a, path_b):
    a = parse_log(path_a)
    b = parse_log(path_b)
    only_a = only_b = 0
    names = sorted(set(a) | set(b))
    for name in names:
        left = set(a.get(name, {}))
        right = set(b.get(name, {}))
        only_a += len(left - right)
        only_b += len(right - left)
    print(f"discordant {path_a} vs {path_b}: only-first {only_a}  only-second {only_b}  files {len(names)}")
    return 0


def is_today(config):
    return config == TODAY


def main():
    parser = argparse.ArgumentParser(description="Firmware decode recall report")
    parser.add_argument("--write-truth", action="store_true")
    parser.add_argument("--write-baseline", action="store_true")
    parser.add_argument("--no-check", action="store_true")
    parser.add_argument("--discordant", nargs=2, metavar="LOG")
    for key, value in TODAY.items():
        kind = type(value)
        parser.add_argument(f"--{key.replace('_', '-')}", type=kind, default=value)
    args = parser.parse_args()

    if args.discordant:
        return discordant(args.discordant[0], args.discordant[1])

    config = {key: getattr(args, key) for key in TODAY}
    in_tree = wavs_12k(IN_TREE)
    busy_dir = fetch_busy()
    busy = wavs_12k(busy_dir)
    if args.write_truth:
        write_truth(in_tree, "in_tree")
        write_truth(busy, "busy")
        manifest = TRUTH / "MANIFEST"
        manifest.write_text(
            "generator: jt9 -8 -d 3\n"
            "wsjt-x: 3.0.1\n"
            "match: normalised message text; hashed calls collapsed to <...>\n"
            f"in_tree: {len(in_tree)} 12 kHz WAVs under components/ft8_lib/vendor/test/wav\n"
            f"busy: howard0su/ft8_lib {HOWARD_SHA} test/wav/20m_busy ({len(busy)} WAVs)\n"
        )
        print(f"wrote {manifest}")

    build_decoder()
    print("config " + " ".join(f"{key}={value}" for key, value in config.items()))
    in_decoded, in_wall, _in_log = run_decoder(config, in_tree)
    busy_decoded, busy_wall, _busy_log = run_decoder(config, busy)
    scores = {
        "in_tree": score_corpus("in_tree", in_tree, in_decoded),
        "busy": score_corpus("busy", busy, busy_decoded),
    }
    print_score("in_tree", scores["in_tree"], sum(in_wall.values()), len(in_tree))
    print_score("busy", scores["busy"], sum(busy_wall.values()), len(busy))

    strong_misses = []
    snr_misses = []
    if config["sample_rate"] == 6000:
        jobs = write_synth()
        synth_decoded, _synth_wall, _synth_log = run_decoder(config, [job[0] for job in jobs])
        by_snr, strong_misses = score_synth(jobs, synth_decoded)
        snr_misses = synth_snr_misses(by_snr)
        print_synth(by_snr, strong_misses)
    else:
        print("synthetic skipped: the AWGN grid is 6 kHz")

    baseline = HERE / "baseline" / "today.txt"
    if args.write_baseline:
        if not is_today(config):
            raise SystemExit("refusing to write the today baseline from a non-default config")
        baseline.parent.mkdir(parents=True, exist_ok=True)
        baseline.write_text(floors_from(scores, strong_misses))
        print(f"wrote {baseline}")
        return 0
    if is_today(config) and not args.no_check:
        if check_floors(baseline, scores, strong_misses, snr_misses):
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
