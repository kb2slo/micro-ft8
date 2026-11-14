# Release 1.0 (epic eu2t)

The first versioned release meant for someone other than the maintainer, and the end of pre-alpha ([AGENTS.md](../../AGENTS.md), **Pre-alpha: do not carry the past**; [RELEASE_PROCESS.md](../RELEASE_PROCESS.md)). Rows: [n75z](#n75z), [dfd7](#dfd7), [pn83](#pn83) ([ROADMAP.md](../ROADMAP.md)).

## Goal

**An operator who is not the maintainer unboxes it, sets up from the printed sheet, completes a QSO, and updates over the air.** Then `v1.0.0` is cut.

## Constraints

* **After 1.0 an update keeps the operator's settings and log.** What an update must keep is [n75z](#n75z)'s.
* **What 1.0 ships, we support.** A feature we will not support is removed before the tag, not shipped quietly.
* **We do not match WSJT-X on decode depth, and the site says so:** size, portability, low power and low cost, traded for decode depth. A number quoted goes in the site's **Experiments**, dated.

## Needs

Rows other epics and the index own; 1.0 is not cut until each has shipped. Each stays where it is: this list names them, it does not own them.

* **OTA, epic [ukk6](ota-updates.md):** [v697](ota-updates.md#v697), [hstv](ota-updates.md#hstv), [yme5](ota-updates.md#yme5), [nufg](ota-updates.md#nufg). First in order, so testers make the one USB reflash ([hstv](ota-updates.md#hstv)'s partition table) early and take everything after it over the air.
* **FT8 and FT4 at once on a QMX, operated, epic [u8mc](several-channels.md):** receiving both shipped on 2026-11-07, FT4 replies beside FT8 on 2026-11-08 and on 40 m on 2026-11-09 ([autoseq.md](../architecture/autoseq.md), **The queue**) ([decode.md](../architecture/decode.md), **FT4**), and a QMX transmits FT4 off FT8's dial ([radio.md](../architecture/radio.md), **TX timing on a QMX**); and next, queue entries carrying their mode ([tnrr](several-channels.md#tnrr)), then calling CQ in FT4 ([bdy4](several-channels.md#bdy4), operator, 2026-11-09), so the lead feature can start a QSO, not only answer one; then the operator choosing FT8, FT4 or both, with each channel's frequency shown ([bd3a](several-channels.md#bd3a), [zjy9](several-channels.md#zjy9), maintainer, 2026-11-13). The feature 1.0 leads with.
* **Autoseq:** [k4ag](smart-queue.md#k4ag) (epic [qxy5](smart-queue.md)).
* **Safety:** [u4dw](../ROADMAP.md#u4dw), [bfkb](../ROADMAP.md#bfkb), [vcng](../ROADMAP.md#vcng).
* **The settings format:** [mtd6](../ROADMAP.md#mtd6), so the first update after 1.0 is not a migration.
* **First run:** [k33u](../ROADMAP.md#k33u), [cg7h](../ROADMAP.md#cg7h).
* **The sidekick serving the app without stalls:** [dlu3](../ROADMAP.md#dlu3) and [p8yv](../ROADMAP.md#p8yv) (operator, 2026-11-09): a stranger's first session must not read No contact.
* **The HL2, receive only and labelled experimental:** [nngr](hl2-support.md#nngr) (epic [knpp](hl2-support.md)).
* **Field checks:** no [TEST_PLAN.md](../TEST_PLAN.md) Pending row on a path 1.0 claims (row [swsu](../ROADMAP.md#swsu)).
* **Wanted, not gating:** [qauk](../ROADMAP.md#qauk), [kkap](../ROADMAP.md#kkap).

**Not in 1.0:** HL2 transmit and the park ([abkw](hl2-support.md#abkw), [kgyh](hl2-support.md#kgyh)); epic [I18](activator-mode.md) ([rrsp](activator-mode.md#rrsp) included); epic [qxy5](smart-queue.md) past [k4ag](smart-queue.md#k4ag); epic [u8mc](several-channels.md) past [zjy9](several-channels.md#zjy9) (the CQ on any channel, each channel's timing in the slot bar, a CQ on each of two channels); [nxyk](ideas.md#nxyk), [syb2](ideas.md#syb2).

## Decided

* **The version line starts at `v1.0.0`.** No `v*` tag exists on `origin` (`git ls-remote --tags origin 'v*'`, 2026-11-06).
* **Port A may keep changing after 1.0.** Both boards update together, version pairing refuses a mismatched pair ([yme5](ota-updates.md#yme5)), and the `FW_*` messages are frozen ([ota-updates.md](ota-updates.md), **Decided**). So a Port A message, the QSO row's 255 bytes included ([syb2](ideas.md#syb2)), is not part of [n75z](#n75z).

## Children

In order. Only the next is a row in the index.

* <a id="n75z"></a>**n75z: settle what an update must keep.** Final partition tables on both boards, `storage` and `web` sizes included (read 2026-11-12: the main board's 16 MB has 4.9 MB unallocated and a 3 MB `storage`, 12 KB of it used; its 4 MB app slots can never fill, an image being staged in the sidekick's 3 MB slot; the sidekick's 8 MB is all allocated, and `web`, 1.92 MB, is what caps the bundle at 800 KB, now 78% used, while its images are 1.10 and 0.87 MB of 3 MB; unmeasured: bytes per logged QSO and what a day file costs the file system; nothing prints these against their limits ([b5bh](ideas.md#b5bh)). For `web`, 2026-11-13: the bundle now ships gzipped, 181 KB ([webapp.md](../architecture/webapp.md), **Build**), so settle the sizes with that in hand; an install stages a whole second copy, which halves the budget, and could stage only the assets that changed; app slots of 2.5 MB would give `web` 2.92 MB and cap both boards' images at 2.5 MB. Changing a partition table over the air is not planned: it is not safe against a power cut, and stays a last resort with the USB flasher behind it. Web content served from the main board over Port A was weighed and set aside: `app.html` alone would take about 15 s at Port A's rate, and the main board has no network by decision ([ota-updates.md](ota-updates.md), **Why not**); the park and summit store kept on the main board's spare flash and answered as lookups over Port A is reopened only if `web` is still short with the bundle compressed, since it would make the add screen need the main board); and for each format an update reads (`station.json`, the QSO log, the download manifest an older web app reads, the `FW_*` messages), its rule after 1.0, written beside the format in the doc that describes it. After [hstv](ota-updates.md#hstv) and [mtd6](../ROADMAP.md#mtd6), which set the tables and the settings file. **Done-when:** each format has its rule in its architecture doc; the partition tables are the ones 1.0 ships; a host test reads a settings file and a log the previous build wrote. **Lands in:** [main_board.md](../architecture/main_board.md) and [sidekick.md](../architecture/sidekick.md) (partitions); [logging.md](../architecture/logging.md) and the doc that describes `station.json`; [RELEASE_PROCESS.md](../RELEASE_PROCESS.md) (what a release must keep).
* <a id="dfd7"></a>**dfd7: a stranger's walk-through.** After every row in **Needs** except the wanted ones. Someone who is not the maintainer, with the printed sheet and nothing else: flash, set up, a QSO, an OTA update. Each snag is fixed or becomes a row before [pn83](#pn83). **Done-when:** one run end to end with no step needing help (field). **Lands in:** [Getting started](../../webapp/getting-started.md) and the site where the run shows a gap; TEST_PLAN.
* <a id="pn83"></a>**pn83: cut v1.0.0.** The site pass (the tradeoff stated, the HL2 marked experimental and receive only, **Coming soon** and **Verified on the air** matching what ships); RELEASE_PROCESS.md's stale sentence on this fork's tags corrected; AGENTS.md's **Pre-alpha** section removed; `python3 tools/cut_release.py 1.0.0`; row [c9yz](../ROADMAP.md#c9yz) out of Blocked for a decision, its pre-alpha condition cleared. **Done-when:** the tag is published and the site says 1.0. **Lands in:** README (status), the site, AGENTS.md, RELEASE_PROCESS.md. This commit deletes this file and its Epics row.

## Open

* **The space-weather card** (before [pn83](#pn83)): replaced by per-band conditions ([B80](../ROADMAP.md#B80)), or removed (maintainer, 2026-11-13: its raw numbers are not what an operator uses).
* **Bug reports from operators** (before [dfd7](#dfd7)): an issue template asking for both boards' versions and an offloaded log, or a report sent from the web app through the Worker to a GitHub issue. The Worker path needs a token on the Worker and spam protection, and an issue is public, so the app shows the operator exactly what it will send. Settled in its own session; becomes a child when designed.
