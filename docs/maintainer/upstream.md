# Upstream watches (maintainer sessions)

Three watches, different cadences. Network fail → skip (any of them); do not interrupt an in-progress coding task.

## `ft8_lib` pin — every maintainer session

Scoped to the submodule pin ([architecture/ft8_lib.md](../architecture/ft8_lib.md)) only. General Micro-FT8 firmware drift is the separate watch below, not this one. The pin has **two** parents and both need watching. GitHub **Sync fork** on `kb2slo/ft8_lib` is off (parent is Wei, not Karlis).

**Karlis** — protocol upstream:

```bash
git ls-remote https://github.com/kgoba/ft8_lib.git refs/heads/master
git -C components/ft8_lib/vendor merge-base --is-ancestor <kgoba-sha> HEAD
```

If the submodule is missing, skip. If Karlis’s SHA **is** an ancestor of the pin, we already have him. If it is **not**, he moved.

**Wei** — the other parent. Do **not** watch `wcheng95/ft8_lib`: that repo is stale at `bb3d94d` (2026-03) and already fully absorbed into the pin. His protocol fixes land in Micro-FT8’s *in-tree* [`components/ft8_lib/ft8/`](https://github.com/wcheng95/Mini-FT8/tree/main/components/ft8_lib/ft8) instead, and reach us only by hand — his tree has no submodule, so merging `upstream/main` cannot move our pin:

```bash
git fetch https://github.com/wcheng95/Mini-FT8.git main
for f in $(git ls-tree -r --name-only FETCH_HEAD components/ft8_lib/ft8/ | sed 's|.*/ft8/||'); do
  diff -q <(git show FETCH_HEAD:components/ft8_lib/ft8/$f) \
          <(git -C components/ft8_lib/vendor show HEAD:ft8/$f) >/dev/null 2>&1 || echo "DIFFERS $f"
done
```

Expect `message.c` and `message.h` to differ by our own changes: the `stpcpy_compat` removal (`f211146`), and his telemetry-buffer fix (`491e757`) ported with named sizes. Anything else is his.

**Why both.** Karlis’s master has not moved since **2025-08-23**, while the protocol fix worth having since then came from Wei (`491e757`). Watching only Karlis watches the quiet parent.

Then draft a Backlog row in chat. Do not commit it yet. Do not merge, bump the submodule, or open a kgoba PR in that turn. Done-when: written take (sync now / wait / drop). Sync uses [architecture/ft8_lib.md](../architecture/ft8_lib.md) **Bumping the pin** and is a separate Now. Goldens gate the pin. If a row for that SHA already exists, remind; do not duplicate.

## Wei's Mini-FT8 (`wcheng95/Mini-FT8`) — at least weekly

Watched, not tracked: no `upstream` remote exists, so the check below fetches Wei's `main` straight from its URL into `FETCH_HEAD` rather than a persistent remote-tracking branch.

**Last checked:** 2026-10-14 — nothing new on Wei's `main` since 2026-09-12: the same 11 commits not in ours, `491e757` (ported by hand) and his RX-1A test tooling. His last push, 2026-10-03, went to the `msc_export` branch. No row drafted.

General drift, not just the `ft8_lib` files this repo shares with Wei's tree (that's the watch above).

```bash
git fetch https://github.com/wcheng95/Mini-FT8.git main
git log --oneline main..FETCH_HEAD
```

If the date above is more than **7 days** old, run it even though it repeats what might be a no-op week to week, and update the date (and the one-line finding) regardless of outcome — that line is the only record of when this last actually ran. Something worth taking → draft an Ideas/Backlog row in chat citing the commit, same discipline as the pin watch: do not merge, do not commit the date bump silently — say what you found first.

## QMX panadapter project (`SteffenLav/qmx-panadapter`) — at least weekly

**Last checked:** 2026-10-14 — v1.16.9 to v1.18.0 (2026-09-30 to 10-12). **Its licence changed from MIT to GPL-3 on 2026-10-09**, for JS8's coding tables, which exist only in JS8Call. What it says about QMX CAT is in [architecture/radio.md](../architecture/radio.md). It is a shipping FT8/FT4 station on the **M5Stack Tab5 (ESP32-P4)**: QMX as a USB host (UAC audio + CAT), on-device FT8/FT4 decode and TX, a browser-streamed panadapter UI, ADIF logging with upload to QRZ/eQSL/LoTW/Cloudlog/Wavelog, POTA/SOTA activation logging, and live PSK Reporter/DX-cluster/POTA spotting. So much of it overlaps open roadmap work that what it finds is worth the maintainer's attention rather than an agent's judgment call.

Check for anything new via the repo's own version history ([`docs/version-history.md`](https://github.com/SteffenLav/qmx-panadapter/blob/main/docs/version-history.md) in that repo) rather than a git remote — it is not a fork parent, just a project worth watching for ideas:

```bash
gh repo view SteffenLav/qmx-panadapter --json pushedAt,description
```

If the date above is more than **7 days** old, look at what changed since the last check (the linked version history is newest-last) and update the date and one-line finding regardless of outcome. Insight, not code — do not port or copy from it without the maintainer's own read of the license and the code.
