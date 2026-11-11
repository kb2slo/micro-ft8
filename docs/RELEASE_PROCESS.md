# Release process

How a build becomes something an operator can download and flash. Companion to [AGENTS.md](../AGENTS.md): that
file is the working agreement, this one is what CI publishes and what a human must still do by hand.

Two paths, both defined in [`.github/workflows/ci.yml`](../.github/workflows/ci.yml). Nothing else publishes.

| Path | Trigger | Job | Tag | PoE-P4 asset | Sidekick | OTA images | Source | Getting-started sheet |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Rolling | push to `main` that can change the binary | `release-main` | `continuous`, force-moved every run | `YYYYMMDD-microft8-poe-p4-<commit>.bin` | `YYYYMMDD-microft8-sidekick-<commit>.bin` | `YYYYMMDD-microft8-poe-p4-ota-<commit>.bin`, `YYYYMMDD-microft8-sidekick-ota-<commit>.bin` | `YYYYMMDD-microft8-source-<commit>.tar.gz` | `YYYYMMDD-microft8-getting-started-<commit>.pdf` |
| Versioned | push of a tag matching `v*` | `release` | the tag you pushed, immovable | `MicroFT8-PoE-P4-<tag>-Merged.bin` | — | — | — | `MicroFT8-Getting-Started-<tag>.pdf` |

Both rolling and versioned publish the M5Stack Unit PoE-P4 merged image (esp32p4, 0x0, custom LittleFS
`storage` partition in `poe_p4/partitions.csv`). Rolling also publishes a **standalone merged sidekick** image
(merged by `tools/merge_sidekick_bin.sh` in the sidekick job) and a **history-free source tarball**
(vendored `ft8_lib`, no `.git`), and each board's **OTA image** (below). The firmware artifacts are skipped entirely for docs-only commits (the
`changes` path filter), so a roadmap-only merge neither rebuilds nor re-tags any binary.

Both paths also publish the **getting-started sheet**, the one-page PDF that goes in the box with a unit.
The `getting-started-sheet` job prints it from `webapp/getting-started.md` on every push, which is also
what holds it to one page. It rides with the firmware: a merge that changes only the sheet republishes
nothing, so the posted PDF can trail the site's Getting started page, which deploys on its own. The sheet
is set in fonts the build embeds (`webapp/tools/getting_started.mjs`), so CI's copy breaks the same way
as one printed on a Mac.

**An image's version is its commit.** Both projects set `PROJECT_VER` to the short SHA
(`tools/git_version.cmake`), with `-dirty` added for a build from a changed tree, so it is in the image's app
description and in each board's boot log (`App version`). No image carries a release number: a build from a
`v*` tag reports its SHA like any other, until versioned releases do (row [nufg](roadmap/ota-updates.md#nufg)).
The web app shows both boards' versions in Settings; the P4 reports its own in its HELLO over Port A
([main_board.md](architecture/main_board.md), **Firmware updates**).

**An OTA image is a board's app image alone** (`poe_p4.bin`, `sidekick.bin`), which is what an over-the-air
update writes into an OTA slot ([sidekick.md](architecture/sidekick.md), **Firmware updates**); the merged
`0x0` images stay for first install and recovery over USB. The manifest describes each in an `ota` entry under
its board (**Public Cloudflare mirror**).

## The rolling `continuous` release

**The invariant: the `continuous` tag is the tip of `main`.** Not a description of it — the actual ref, so
`git checkout continuous` and the published binary are the same commit.

That invariant needs the explicit `git push --force origin <sha>:refs/tags/continuous` step in `release-main`.
Keep that step, and keep knowing why: GitHub's release API **ignores `target_commitish` once a tag of that
name exists** — it is honoured only when the tag is created. A workflow that merely passes `target_commitish`
therefore places the tag on its first run and never moves it again, while still replacing the release body
and assets correctly every time. The failure is silent and reads backwards — the download is current, the
release page quotes the right commit, and only the ref is stale — so nothing surfaces it. Delete the push
step as redundant and the tag quietly freezes again.

Corollaries an agent should not re-derive:

- The release is **updated in place**, not recreated, so GitHub keeps its original `publishedAt`. The date on
  the release page is when the release object was first made and will not advance. Judge freshness by the
  commit in the body or by the tag, never by that date.
- `prerelease: true` and `make_latest: false` are deliberate: they keep the rolling build from taking the
  "Latest release" badge away from the newest real version. This is also why the tag is not called `latest`.
- **The asset name changes every merge, on purpose.** `YYYYMMDD-microft8-<kind>-<commit>.bin` still identifies
  itself after download, where several same-named files in one Downloads folder do not. The date is the *commit* date in UTC, so rebuilding a commit reproduces the
  name. The cost is that there is no stable download URL; a script should resolve the asset from the release
  rather than hardcode a filename.
- Because the name changes, `release-main` **deletes the release's existing assets** before uploading. Without
  that they accumulate on one release forever. The release object itself is never deleted.

## Public Cloudflare mirror (anonymous downloads)

GitHub's `continuous` assets carry dated names that change every run, so nothing can link to them for long. After
each successful `release-main`, CI also mirrors the rolling payloads to Cloudflare **R2** bucket
`micro-ft8-downloads`, served under the site origin by [`webapp/worker.js`](../webapp/worker.js), whose `/download/*` route is one of the two that run the Worker ([webapp.md](architecture/webapp.md), **Serving the site**):

| Stable URL | Contents |
| --- | --- |
| `https://micro-ft8.kalikstein.com/download/microft8-poe-p4-continuous.bin` | PoE-P4 merged image |
| `https://micro-ft8.kalikstein.com/download/microft8-sidekick-continuous.bin` | Sidekick merged image |
| `https://micro-ft8.kalikstein.com/download/microft8-poe-p4-continuous-ota.bin` | PoE-P4 OTA image |
| `https://micro-ft8.kalikstein.com/download/microft8-sidekick-continuous-ota.bin` | Sidekick OTA image |
| `https://micro-ft8.kalikstein.com/download/microft8-source-continuous.tar.gz` | Source tarball |
| `https://micro-ft8.kalikstein.com/download/microft8-getting-started-continuous.pdf` | Getting-started sheet |
| `https://micro-ft8.kalikstein.com/download/manifest.json` | Commit, dated filenames, SHA-256; each board's `ota` entry |

Human entry: [`https://micro-ft8.kalikstein.com/downloads.html`](https://micro-ft8.kalikstein.com/downloads.html).

**This is a public copy of what `release-main` just published, not a second source of truth.** GitHub
remains the operator/dev record; R2 exists so the site and the printed sheet can link to names that never change.
Stable keys are overwritten every run. Firmware merges do **not** redeploy the Worker (`pages.yml` stays
path-filtered to `webapp/**`).

**The manifest is written by `tools/make_manifest.py`,** before `release-main` publishes anything. Under
`assets`, each of `poe_p4`, `sidekick`, `source` and `getting_started` has `stable` (its name here),
`filename` (its dated name on GitHub) and `sha256`. The two boards also have an `ota` entry: `stable`,
`filename`, `size` in bytes, `sha256`, `version` (the string built into the image) and `proto` (the
`PORTA_PROTO_VERSION` that build speaks). The script reads the version out of the image and refuses to write a
manifest, which fails the job with nothing published, when an OTA image is not an app image, was built as
the other board's project, reports a version other than the commit's, or is larger than the sidekick's 3 MB
OTA slot, where every image is staged. Host-tested (`tools/test_make_manifest.py`).

**The stable files are replaced one at a time, the manifest last.** For the seconds between, the old manifest
names a hash the new image does not have, so a download checked against it fails and is retried; it cannot
install the wrong image.

### One-time setup

1. Create the bucket (once), or let the next `pages.yml` deploy attempt
   `wrangler r2 bucket create micro-ft8-downloads` (`continue-on-error` if it already
   exists). Same Cloudflare account as the Worker.
2. Ensure GitHub secret `CLOUDFLARE_API_TOKEN` includes **Account · R2 · Edit** in addition to the
   Workers deploy permission already used by `pages.yml`. `CLOUDFLARE_ACCOUNT_ID` is unchanged.
3. Deploy `webapp/` so the Worker picks up the `DOWNLOADS` R2 binding (`wrangler.jsonc`), then wait for
   the next firmware merge (or re-run `release-main`) to fill the bucket.

## Cutting a versioned release

```bash
python3 tools/cut_release.py 3.0.0
```

That is the whole procedure. The script creates and pushes the annotated `v3.0.0` tag; CI does the rest.
It **edits no source file**, so there is no number to bump and therefore no bump commit for the tag to land
on the wrong side of, which is the classic way a hand-run release ends up tagging the wrong thing. The tag
names the published file; the image itself carries no version (above).

It refuses, rather than warns, on: a dirty tree, not being on `main`, `HEAD` disagreeing with
`origin/main`, a tag that already exists locally or on the remote, and a commit CI has not run green.
**A missing CI run counts as a failure, not as permission** (`--skip-ci-check` overrides deliberately, and
says so on stdout). Then it prints the tag and commit and asks before pushing;
`--yes` skips the prompt.

Cutting a release is also the event that ends pre-alpha — **Pre-alpha: do not carry the past** in
[AGENTS.md](../AGENTS.md) — because a version aimed at anyone but the maintainer is the first time back-compat
becomes an obligation rather than a choice. None have been cut under this process yet.

Version tags are immovable by convention: re-pointing one breaks the single guarantee a version number
carries. To fix a bad release, cut the next one.

Use lowercase `v`. This fork's tags are `v0.1` … `v1.1.2`; Wei's upstream tags are uppercase (`V1.1` …
`V2.1b`) and are in this repo's history too. Git refs are case-sensitive and the trigger glob is `v*`, so
upstream's tags do not fire CI and ours do — a `V3.0.0` would silently build nothing.

## If the project is renamed or de-forked

Recorded because it is a stated direction, not a hypothetical. Renaming touches more than strings, and one of
these has a field consequence rather than a cosmetic one.

Renaming touches names that are operator-visible:

- Asset and tag names in `ci.yml` (`MicroFT8-PoE-P4-<tag>-Merged.bin`, `YYYYMMDD-microft8-<kind>-<commit>.bin`).
- `MICROFT8_GIT_SHA` in `ci.yml` and `sidekick/CMakeLists.txt`, and `project(sidekick)`.
- The mDNS hostname `micro-ft8.local` ([system.md](architecture/system.md) (**Why not**)), which is in operators' browser history.
- Clone URLs and release links in `README.md` and `docs/`. GitHub redirects a renamed repo, so these keep
  working — but redirects are not a plan.

**The version number is the de-fork.** Nothing displays a product version, so the first `v*` tag chooses this
project's own version line from scratch. That makes the first number a product decision about where this tree
starts counting, not an edit to a constant.
