# Sidekick under load

A repeatable load on the sidekick's web server, and what it measures: the lowest free heap, events latency
and page reloads. It produced the runs recorded in [TEST_PLAN.md](../../docs/TEST_PLAN.md) (Pending, the
sidekick's worker row) and in [sidekick.md](../../docs/architecture/sidekick.md), **Tasks and cores**.

One run, per build:

1. Install the build over the air, which restarts the sidekick and so resets its since-boot heap low:
   `python3 webapp/tools/install_firmware.py --image <sidekick.bin> --host <sidekick>`. Wait a minute.
2. Start these three together, for 10 minutes, with no other client on the station:
   * `node tools/sidekick_load/load.js <sidekick> 10`: three pages, each reloaded every 20 s;
   * `python3 tools/sidekick_load/probe.py <sidekick> 600 0.5 probe.log`: an events read every 0.5 s;
   * `python3 tools/sidekick_load/heap.py <sidekick> 10.5 heap.jsonl`: `/api/config`'s heap and server facts every 5 s.
3. Read the last line of `heap.jsonl` (`sk_heap_min`, `sk_heap_largest`, `sk_httpd_held_peak_ms`), the
   slowest lines of `probe.log`, and the reload times in the load's output.

**Run each build at least twice.** The heap low is one worst moment in ten minutes, and on 2026-11-16 the
same image gave 46,664 and 23,344 bytes, a wider spread than any difference between builds. The load is far
heavier than an operator's (nine page loads a minute), so a low under 40 KB here is not a station's.

## A slow client

`slow_client.py` takes its reply slowly, as a phone in WiFi power save does: a small receive buffer, a
chunk every quarter second. Run it beside `probe.py` with no other client on the station:

* `python3 tools/sidekick_load/slow_client.py <sidekick> 60 /app.html 1024 0.25`
* `python3 tools/sidekick_load/probe.py <sidekick> 60 0.5 probe.log`

The bar ([sidekick.md](../../docs/architecture/sidekick.md), **Tasks and cores**) is the probe's reads under 500 ms throughout. On 2026-11-18 (sidekick `7c34a5a`, both run from
one computer on the same WiFi) the probe alone read in a median 48 ms (worst 623 ms, 46 reads); beside the
slow client all six of its reads timed out at 10 s, and the sidekick logged `SK slow GET /app.html 61181 ms`
and `SK httpd held over 30 s`. The slow client took 22,328 of the file's 53,055 bytes in 61 s: as long as it
takes something, nothing ends its send, and `httpd` serves no one else.

## Overnight

`overnight.sh` is a practical worst case for hours: two app pages left open and reloaded every 5 minutes,
`slow_client.py` for a minute every 10, `probe.py` every 2 s and `heap.py` every 5 s. The probe also keeps
every log line in the ring, the sidekick's (`SK ...`) and the main board's (slot timing, audio lost), and
notes any events it missed (`GAP`). Nothing is written to the station. Leave the radio decoding a busy band.

    CHROME=/usr/bin/google-chrome tools/sidekick_load/overnight.sh http://192.168.50.128 10 /tmp/overnight

Keep the computer awake for the run, for example under `systemd-inhibit` on Linux or `caffeinate` on macOS.

**Run 1** (2026-11-18, 04:17 to 11:57 UTC, sidekick `10c7635-dirty`: the sender pool, files on at most two
senders; 40 m with FT4 decoded). No leak and no fragmentation: the median free heap held at 87.4 to 87.8 KB
every hour and the median largest block at 40,960 bytes. But short dips took the free heap to **392 bytes**,
with every TCP slot in use, and in three hours about 100 events reads an hour took over 500 ms, the worst
13.7 s, and 21 failed. The pages had piled up events reads (four with one `since` out at once), and each
connection holds up to its send buffer unacknowledged. That led to the app's polls one at a time and the
2,880-byte send buffer ([sidekick.md](../../docs/architecture/sidekick.md), **Tasks and cores**).
