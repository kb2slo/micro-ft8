# Release process

How a build becomes something an operator can download and flash. Companion to [AGENTS.md](../AGENTS.md): that
file is the working agreement, this one is what CI publishes and what a human must still do by hand.

Two paths, both defined in [`.github/workflows/ci.yml`](../.github/workflows/ci.yml). Nothing else publishes.

| Path | Trigger | Job | Published to | PoE-P4 image | Sidekick | OTA images | Source | Getting-started sheet |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Dev | push to `main` that can change the binary | `release-main` | the mirror (**Public Cloudflare mirror**); no git tag and no GitHub release | `microft8-poe-p4-dev.bin` | `microft8-sidekick-dev.bin` | `microft8-poe-p4-dev-ota.bin`, `microft8-sidekick-dev-ota.bin`, and each under `ota/` by its dated name | `microft8-source-dev.tar.gz` | `microft8-getting-started-dev.pdf` |
| Stable | push of a tag matching `v*` | `release` | a GitHub release on the tag you pushed, immovable, and the mirror | `microft8-poe-p4-<number>.bin` | `microft8-sidekick-<number>.bin` | `microft8-poe-p4-ota-<number>.bin`, `microft8-sidekick-ota-<number>.bin` | `microft8-source-<number>.tar.gz` | `microft8-getting-started-<number>.pdf` |

Both publish the same set: the M5Stack Unit PoE-P4 merged image (esp32p4, 0x0, custom LittleFS
`storage` partition in `poe_p4/partitions.csv`), a **standalone merged sidekick** image
(merged by `tools/merge_sidekick_bin.sh` in the sidekick job), a **history-free source tarball**
(vendored `ft8_lib`, no `.git`), each board's **OTA image** (below), and the channel's manifest. The names
above are a dev file's on the mirror and a stable file's on GitHub, `<number>` the tag's without its `v`; on
the mirror a stable file has the dev name with `stable` for `dev`. **A release also publishes the web app,**
built at the tag and stamped with the release's version: `microft8-app-<number>.tar.gz` on GitHub, and its
bundle under `app/<number>/` on the mirror, which the stable manifest names. The dev app is published apart
from the firmware, by `pages.yml` on every push that touches it ([webapp.md](architecture/webapp.md),
**Serving the site**). The firmware artifacts are skipped entirely for docs-only commits (the
`changes` path filter), so a roadmap-only merge neither rebuilds nor republishes any binary, and the dev
channel's build is the last commit that could change one, not always the tip of `main`.

**The dev channel has no git tag and no GitHub release.** A GitHub release needs a tag, and a tag that moved
on every publish named nothing the manifest does not: its `commit` is the published build's, every dev
version ends in its commit, and each OTA image is kept on the mirror under its dated name. GitHub releases
are for versioned tags and the decode corpora.

Both paths also publish the **getting-started sheet**, the one-page PDF that goes in the box with a unit.
The `getting-started-sheet` job prints it from `webapp/getting-started.md` on every push, which is also
what holds it to one page. It rides with the firmware: a merge that changes only the sheet republishes
nothing, so the posted PDF can trail the site's Getting started page, which deploys on its own. The sheet
is set in fonts the build embeds (`webapp/tools/getting_started.mjs`), so CI's copy breaks the same way
as one printed on a Mac.

**An image carries its version,** and every version names its commit. Both projects set `PROJECT_VER`
(`tools/git_version.cmake`), so it is in the image's app description and in each board's boot log
(`App version`); the web app shows both boards' versions in Settings, and the P4 reports its own in its HELLO
over Port A ([main_board.md](architecture/main_board.md), **Firmware updates**). Three shapes:

| Build | Version | Example |
| --- | --- | --- |
| CI's build of a `v<major>.<minor>.<patch>` tag (stable) | the tag's number, `+`, the short SHA | `0.1.0+0c5ccec` |
| CI's build of `main` (dev) | `b`, the number of commits on `main` up to and including it, `+`, the short SHA | `b1322+0c5ccec` |
| Any other: a local build, a pull request's | the short SHA, with `-dirty` from a changed tree | `0c5ccec`, `0c5ccec-dirty` |

`tools/build_version.py` writes the first two and CI hands its line to the build as `MICROFT8_VERSION`; its
`parse()` reads them back. A dev build's number is `git rev-list --count <commit>`, so anyone can count it again
from a clone, and it only rises on `main`. A `v*` tag that is not three numbers fails its build. Only CI's
builds are on a channel: a local build reports its SHA at a tagged commit too, a changed tree never takes a
version it is handed, and one naming another commit stops the build, so nothing built at a bench is taken for
a release. The boards compare versions as whole strings, never by order
([sidekick.md](architecture/sidekick.md), **Firmware updates**). The web app's build id is its commit and date, and a
release's app's its version and date ([webapp.md](architecture/webapp.md), **Build**).

**An OTA image is a board's app image alone** (`poe_p4.bin`, `sidekick.bin`), which is what an over-the-air
update writes into an OTA slot ([sidekick.md](architecture/sidekick.md), **Firmware updates**); the merged
`0x0` images stay for first install and recovery over USB. The manifest describes each in an `ota` entry under
its board (**Public Cloudflare mirror**).

## Public Cloudflare mirror (anonymous downloads)

`release-main` publishes the dev channel, and `release` the stable one, to Cloudflare **R2** bucket
`micro-ft8-downloads`, served under the site origin by [`webapp/worker.js`](../webapp/worker.js), whose `/download/*` route is one of the two that run the Worker ([webapp.md](architecture/webapp.md), **Serving the site**):

| Stable URL | Contents |
| --- | --- |
| `https://micro-ft8.kalikstein.com/download/microft8-poe-p4-dev.bin` | PoE-P4 merged image |
| `https://micro-ft8.kalikstein.com/download/microft8-sidekick-dev.bin` | Sidekick merged image |
| `https://micro-ft8.kalikstein.com/download/microft8-poe-p4-dev-ota.bin` | PoE-P4 OTA image |
| `https://micro-ft8.kalikstein.com/download/microft8-sidekick-dev-ota.bin` | Sidekick OTA image |
| `https://micro-ft8.kalikstein.com/download/microft8-source-dev.tar.gz` | Source tarball |
| `https://micro-ft8.kalikstein.com/download/microft8-getting-started-dev.pdf` | Getting-started sheet |
| `https://micro-ft8.kalikstein.com/download/manifest.json` | The dev channel's manifest: commit, version, each file's own name and SHA-256, each board's `ota` entry |
| `https://micro-ft8.kalikstein.com/download/ota/<dated OTA filename>` | Each dev OTA image again by its dated name, never overwritten or deleted |
| the six file names above with `stable` for `dev` | The newest stable release's files, replaced at each release |
| `https://micro-ft8.kalikstein.com/download/manifest-stable.json` | The stable channel's manifest, in the same form |
| `https://micro-ft8.kalikstein.com/download/ota/microft8-<board>-ota-<number>.bin` | Each stable OTA image by its release's number, never overwritten or deleted |
| `https://micro-ft8.kalikstein.com/download/app/<number>/<file>` | Each release's web app bundle, as built: its `manifest.json`, `build.json` and assets, never overwritten or deleted |

Human entry: [`https://micro-ft8.kalikstein.com/downloads.html`](https://micro-ft8.kalikstein.com/downloads.html).

**The mirror is the dev channel's only home;** a stable release is on GitHub too, published there first. The
site, the web flasher and the printed sheet link to names that never change, and those keys are overwritten
every run; only an OTA image is also kept under a name of its own (below). Firmware merges do **not** redeploy the Worker (`pages.yml` stays
path-filtered to `webapp/**`).

**Each channel has its own manifest, written by `tools/make_manifest.py`** before its job publishes anything:
one file a job, so neither rewrites part of the other's. `channel` is `dev` or `stable`. Under
`assets`, each of `poe_p4`, `sidekick`, `source` and `getting_started` has `stable` (its fixed name here),
`filename` (its own name: a dev file's dated one, `YYYYMMDD-microft8-<kind>-<commit>`, the date the commit's in UTC, and a stable file's by its release's number; on the mirror only an OTA image is stored under it) and `sha256`. The two boards also have an `ota` entry: `stable`,
`filename`, `size` in bytes, `sha256`, `version` (the string built into the image) and `proto` (the
`PORTA_PROTO_VERSION` that build speaks). A stable manifest's `app` names the release's web app: `path` (`app/<number>/`, under the mirror's
`/download/`), and `build` and `size` from the bundle's `build.json`; the script refuses an app that was not
built as that release. Beside `commit` and `short`, the manifest's own `version` is the
build's and `build` its number: a dev version's own, and for a stable release the number of commits up to
its commit, counted the same way, so the one number orders the builds of both channels. The script reads the
version out of the image and refuses to write a
manifest, which fails the job with nothing published, when the version it is given is not a channel's build
of the commit, or an OTA image is not an app image, was built as the other board's project, reports another
version, or is larger than the sidekick's 3 MB OTA slot, where every image is staged. Host-tested (`tools/test_make_manifest.py`).

**The stable files are replaced one at a time, the manifest last.** For the seconds between, the old manifest
names a hash the new image does not have, so a download checked against it fails and is retried; it cannot
install the wrong image.

**An update downloads by the dated name, under `ota/`,** which no later run overwrites (`updates.js` builds the
URL from the `ota` entry's `filename`). An update the operator accepted therefore installs that build to the
end, its second board included, however many builds are published while it runs; the station then offers the
newer one. The stable `-ota` names stay for people and scripts. Uploaded before the manifest, like the rest.
**Nothing expires them:** firmware is published only when a push touches it, so the build the manifest names can
be months old, and an age rule would delete the image every update needs. They cost about 2 MB a firmware
push against R2's 10 GB free tier; prune by hand, never the build the manifest names. The Worker serves one
name, one under `ota/`, or one under `app/<number>/`, and nothing else nested (`webapp/tests/test_worker.js`).

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
names the published file, and the image reports the tag's number (above).

It refuses, rather than warns, on: a dirty tree, not being on `main`, `HEAD` disagreeing with
`origin/main`, a tag that already exists locally or on the remote, a number not greater than every release
already tagged, a commit whose dev build was not published, and a commit CI has not run green.
**A missing CI run counts as a failure, not as permission** (`--skip-ci-check` overrides deliberately, and
says so on stdout). Then it prints the tag and commit and asks before pushing;
`--yes` skips the prompt.

**A stable tag goes only on a commit whose dev build was published,** so a release is code that could be
installed as a dev build, and run at the bench, first. Published means both boards' OTA images are on the
mirror under their dated names (`tools/dev_build.py`, which asks the mirror). The script refuses first, and
the tag's `release` job asks again before it publishes anything, so a tag pushed by hand does not pass. A
commit that changed no firmware has no dev build, since CI built none: `gh workflow run CI --ref main` runs
the firmware jobs and `release-main` on the tip of `main` whatever changed, and the tag can then be cut. Run
it only for that: on a commit already published it uploads the two dated OTA images again.

**The first release, `v0.1.0`, makes the project a beta.** A `0.x` release is for testers and promises nothing
across an update, a station's settings and log included: [AGENTS.md](../AGENTS.md), **Before 1.0: do not
carry the past**. Back-compat becomes an obligation at 1.0 ([roadmap/release-1-0.md](roadmap/release-1-0.md)).

Version tags are immovable by convention: re-pointing one breaks the single guarantee a version number
carries. To fix a bad release, cut the next one.

Use lowercase `v`. No `v*` tag has been cut here yet; Wei's upstream tags are uppercase (`V1.1` …
`V2.1b`). Git refs are case-sensitive and the trigger glob is `v*`, so a `V3.0.0` would silently build
nothing.

## If the project is renamed or de-forked

Recorded because it is a stated direction, not a hypothetical. Renaming touches more than strings, and one of
these has a field consequence rather than a cosmetic one.

Renaming touches names that are operator-visible:

- File names in `ci.yml` (`microft8-poe-p4-<number>.bin`, `YYYYMMDD-microft8-<kind>-<commit>.bin`).
- `MICROFT8_GIT_SHA` in `ci.yml` and `sidekick/CMakeLists.txt`, and `project(sidekick)`.
- The mDNS hostname `micro-ft8.local` ([system.md](architecture/system.md) (**Why not**)), which is in operators' browser history.
- Clone URLs and release links in `README.md` and `docs/`. GitHub redirects a renamed repo, so these keep
  working — but redirects are not a plan.

**The version number is the de-fork.** Nothing displays a product version, so the first `v*` tag chooses this
project's own version line from scratch. That makes the first number a product decision about where this tree
starts counting, not an edit to a constant.
