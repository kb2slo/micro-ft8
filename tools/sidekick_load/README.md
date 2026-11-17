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
