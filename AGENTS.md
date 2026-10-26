# Working agreement (any agent)

This repo is tool-agnostic, and this file is the working agreement for every agent. Tool-specific
files (`CLAUDE.md`, `.cursor/rules/`, and any future equivalent) load it and carry no rules of their
own; if one disagrees with this file, the stub is the bug. Supporting a new tool means adding a stub,
never copying or moving rules into it. Do not adopt a vendor-specific format for anything that belongs
here or in `docs/` — workflow, style, roadmap, or RFCs.

At the start of a session, read `docs/ROADMAP.md` (and the architecture or RFC for the area you touch). Follow [STYLE.md](docs/STYLE.md) for our C/C++.

**Workflow matches documentation.** Committed docs are the workflow: this file, `ROADMAP.md`, `STYLE.md`, RFCs, and the architecture notes. Cite the relevant doc when proposing or making a change.

**A line earns its place here if it changes what an agent does.** If it only explains how a rule came about, it belongs in the commit message, not in this file.

## The work

**Push back before tools.** Classify the request against `ROADMAP.md` Now, the active RFC, and `STYLE.md` **before any edit of implementation, or before exploration spanning more than one file.** If it is not the current Now theme, not an explicit exception in those docs, or is still Ideas/Backlog (including “quick” UI/product tweaks), the first reply cites the doc and talks it through. No open-ended codebase exploration to be helpful — that is exactly the token spend this rule exists to stop. A single cheap lookup (one grep, one file read, "where is X defined") can just be answered without the gate. Tools only after the operator retracts, grants an exception, or updates the doc in the same turn — an exception is an explicit decision, not a shortcut, and never a silent divergence. Docs and roadmap questions are allowed without that gate.

Do not mix a feature change with an unrelated fix in the same commit.

**Fix defects in the slice.** A defect found while working in an area gets fixed in that slice, not filed for later. It still gets a `ROADMAP.md` row, but the row records what was wrong and why — it is not a promise to act later. Single-operator workflow: the reasons to defer (protecting someone else's sprint, batching context switches across a team) do not apply, and deferred fixes here have a poor track record of ever landing.

Judge by **verifiability**, not size, and say which of the three applied:

1. **You can prove it** — host test, dead-code proof (linker map plus `nm`, byte-identical binary), or the compiler. Fix it and prove it in the same commit.
2. **Only the operator can prove it** — anything needing the radio. Fix it and add the bench check to [TEST_PLAN.md](docs/TEST_PLAN.md) **Pending** with the exact keys and expected result.
3. **Neither can, yet** — an unreproducible hardware or timing bug. Prefer a change whose **worst case is no worse than current behaviour**, and ship the logging that makes the next occurrence diagnosable. Never let an unverified fix blend in with verified work: say plainly, in the commit and in chat, that it is unproven.

An unproven fix is still worth making. Pretending it is proven is not.

**Tier the model to the task, the same way work is tiered by verifiability above.** An agent can prove some things and not others; a model can carry some kinds of work well and not others, and a wrong choice here is not caught by the compiler the way a bad fix is. State which tier a task is before starting non-trivial work:

1. **Mechanical, low blast radius** — `git mv`-shaped extracts, renames, doc formatting, running `host_mock`/`tx_e2e`/`idf.py build` and reporting the result, applying a change the operator already fully specified. Verified by the compiler or a host test regardless of which model did it — use whatever is fast and cheap.
2. **Judgment under ambiguity** — RFC drafting, sizing a memory/timing budget, choosing an extract boundary, RFC graduation, changes to this process, anything **Push back before tools** above would stop for. Use the strongest available model. Nothing mechanical catches a wrong call here, which is exactly what makes it expensive.
3. **Field-only, no model tier substitutes** — USB/CDC/CAT, UAC timing, TX behaviour, anything [TEST_PLAN.md](docs/TEST_PLAN.md) marks Pending. This is category 2/3 of the verifiability rubric above wearing a model-choice hat: a bigger model does not turn an operator-only or hardware-only check into something an agent can close. Say so plainly rather than picking a bigger model and reporting more confidence than the check supports.

**Test plan matches the work too.** [TEST_PLAN.md](docs/TEST_PLAN.md) is a ledger, not a manual, and is updated in the **same turn** as the work:

- Touched a field-only path (USB/UAC, CAT, display, GPS, RTC, SD, flash, WiFi, TX timing) → say in chat what to flash, press or run and what result means it passed, and add **one Pending row** naming the change and that check, written as numbered steps, each with its expected result. "Test the radio" is not a check; "MENU P3 `5` reports `Copied OK`" is. The chat is how the operator hears about it; the row is how it is not forgotten.
- Added a host test → nothing to record here. What a test pins lives in its own source.
- Added, removed or renamed a check in the **Automated** table (a harness, a CI job) → update that table in the same commit.
- Shipped something no check is planned for → add it to **Unverified**, so it is not mistaken for proven.
- Removed code that was provably never in the image (linker map plus `nm` on the ELF, byte-identical `micro_ft8.bin`) → say so and add no row. That evidence *is* the test.

State which of these you did in the turn you report the work. **"No test-plan change needed" is a valid answer and must be said out loud**, exactly like the README audit — an audit you skipped and an audit you did are otherwise indistinguishable.

**Architecture docs match the code too.** A `docs/architecture/<area>.md` describes how things work now, so a change that alters behaviour one of them describes updates it in the **same commit**. As with the README audit, say it out loud when nothing needs changing: "no architecture-doc change needed". An RFC slice does this through graduation ([docs/rfcs/README.md](docs/rfcs/README.md), **RFC lifecycle**); this rule covers changes that come through no RFC. A doc may describe a decided but unbuilt design only under a label naming the roadmap rows that build it — "**not yet built: rows v697, hstv**" — so that when those rows ship and are dropped, `tools/roadmap_rows.sh dangling` flags the stale label.

**Never report a field check as passed.** An agent runs `tools/check.sh` and reports its summary honestly, a SKIP as a skip. Anything needing the P4, a radio, or an antenna is the operator's to run: propose it, put it in **Pending**, and wait. A green build is evidence about linkage, not about whether the radio transmitted.

**Fork README stays current.** The top of [`README.md`](README.md) (above the Wei delimiter) is the public “why this fork” copy. When work is operator-visible (TX UX, meters, logging, flashing, CQ, radios, …), audit that section in the **same turn**: propose the add, change, or drop in chat with the test plan and commit message. Include the accepted edit in the same commit and push as the feature. Do not rewrite Wei’s original below the delimiter. Skip agent, RFC, extract-only, or other work operators cannot feel on the radio.

**Triggers, stated so they cannot be missed.** Audit that section when a change adds, alters, or **removes** any of: a web-app screen or control; an on-screen string the README quotes; a documented workflow (log offload, flashing, time sync); or a supported radio/board. **Removal counts, and is the easy one to miss** — a README documenting a key that now does nothing is worse than one that never mentioned it. If Wei’s original below the delimiter documents the same thing, do **not** edit it: put the correction in the fork note and let the delimiter rule (“use the fork notes above where they disagree”) carry it. When it genuinely does not apply, say so explicitly in that turn, as above.

Keep new logic out of `poe_p4/main/` ([STYLE.md](docs/STYLE.md)). Radio and `ft8_lib` boundaries: [architecture/radio.md](docs/architecture/radio.md), [architecture/ft8_lib.md](docs/architecture/ft8_lib.md). Read the current Now from `ROADMAP.md` rather than trusting a name written here — this line has gone stale before.

This project ships on **`origin/main`** (`kb2slo/micro-ft8`, private). De-forked 2026-09-15 from `kb2slo/Micro-FT8` (now archived, public) — there is no `upstream` git remote. Wei's `wcheng95/Mini-FT8` is watched for portable fixes ([docs/maintainer/upstream.md](docs/maintainer/upstream.md)), not tracked as a fork; never push or open a PR there unless the operator explicitly asks.

## Clone (submodules)

`ft8_lib` is the git submodule `components/ft8_lib/vendor`. Host `tx_e2e` and `idf.py build` need it. GitHub’s default clone does not fetch it. The repo is private — clone needs your own GitHub auth (SSH key or PAT), not just the URL.

```bash
git clone --recurse-submodules git@github.com:kb2slo/micro-ft8.git
```

Existing trees: `git submodule update --init`. Pin and bump path: [architecture/ft8_lib.md](docs/architecture/ft8_lib.md).

## Reproducibility

The full dev environment — build, test, and the sidekick field-flash workflow — must be reproducible from a fresh `git clone` (submodules included) plus a capable agent, with no dependency on state that exists only on one operator's machine. When a step genuinely needs a one-time physical-hardware action (e.g. capturing a Nano's factory-state backup before it is ever field-flashed), document the exact command in the relevant README/script rather than committing a derived artifact whose redistribution terms aren't ours to decide. `sidekick/README.md`'s stock-backup section is the current example of this pattern.

## Chat gates

The operator reviews in chat, then green-lights each step. A good previous turn is not permission to skip a gate.

1. **Design in chat.** Architecture and “should we” questions get an answer first. Do not implement until they say to (e.g. “go”, “do it”). Docs and roadmap questions do not need that wait.

2. **Test plan with the result.** When work is done, the reply includes a short test plan: host command(s) they can run (`tools/check.sh --changed` runs every host check the change can affect), and a field check if the path is field-only. They often run tests and flash themselves and will say when it is solid. Do not flash the device unless they ask.

3. **Commit message in chat.** Propose the message here: a subject of one sentence, at most 72 characters, saying what changed, then a blank line and the why in the body. `tools/check_commits.py` holds commits to that shape, in CI on pull requests. For operator-visible work, also propose the fork-README delta (or say why that section needs no change). Wait for them to accept or edit. Do not commit in the same turn as the proposal unless they already asked to commit.

4. **Commit / push only when asked.** Local edits are fine when they asked for the work. `git commit` waits until they have seen the plan or diff and said to commit. `git push origin` waits until they said to push. “Commit and push” is both, using the agreed message. Do not treat “ship on main”, a green host test, or “local commits are fine” as standing permission.

   **A scope instruction is not a push instruction, even mid-session.** Field-found 2026-09-17: after several rounds of the operator explicitly saying "commit and push" earlier in one long session, an agent then received "yes fix it now" — approval to make a specific correction — and pushed it without being asked, reasoning from the session's *pattern* rather than that turn's actual words. The rule above already said not to do this ("a good previous turn is not permission to skip a gate," restated here because it still happened): each push needs its own explicit trigger in that turn — "push", "commit and push", or equivalent — not inferred from momentum, from how the last few turns went, or from the change being correct and well-tested. Being right about the fix does not retroactively authorize the push.

5. **Undo with git, not a second edit.** Do not invert your last turn by re-editing files. That is slow and easy to get wrong when other uncommitted work is in the same tree.
   - Already a commit: `git revert` (or drop a *local, unpushed* commit only if they asked).
   - Uncommitted, and those paths contain *only* the work to undo: `git restore -- <paths>`.
   - Uncommitted mixed with other dirty files: do **not** `git restore` (that drops the other work). One sentence in chat: cheap undo needs a commit. Ask them to checkpoint, then revert.
   Before an experiment they may want to unwind, ask for a checkpoint commit first.

## Review before commit — nominate what is worth the operator's eyes

The operator trades review time for speed and does not read every line. So do the triage: **with every commit
proposal, give a "worth your eyes" list of one to three items**, each a `file.cpp:line` and one sentence on
what could be wrong, then say plainly that the rest is mechanical. An unranked diff handed over is the same as
handing over nothing.

Prefer, in this order:

1. **What needs the operator's domain knowledge and not the agent's** — what a real ADIF consumer accepts,
   what an operator expects a key to do, whether a report format is right. The agent cannot grade these at all.
2. **What compiles, passes, and is still wrong** — encoding, units, a flash offset, a label paired with the
   wrong action.
3. **Choices made unilaterally**, where another call was reasonable.
4. **What is hard to undo** — pushed history, partition tables, key handling.

Do not nominate what the compiler, a host test or CI already proves, or mechanical refactors. Saying "the rest
is mechanical" is part of the job — it is what makes the nomination worth trusting.

Nominate the places of least confidence, not the ones that look best: a highlight reel steers attention away
from the risk. Never let "I verified it" stand in for evidence — say what was checked and give the command
that re-checks it. Name what only hardware can catch, separately from what review can.

If more than three items feel essential, the change is too big to review. Say so and propose splitting it.

## Operating notes

Working preferences the operator has stated. They live here, in the repo, because that is the only place
every agent and every operator can see them — see "Memory lives in this repo" below.

- **Build locally; do not round-trip CI for a test binary.** `idf.py build` is ~40 s against ~5 min for the
  firmware CI job, and during bench debugging that latency dominates. The merged image lands in `build/`
  (`POST_BUILD` writes `<sha>-microft8-dev.bin` alongside `MicroFT8_Merged_Auto.bin`). CI is for pre-merge
  verification, where its value is real: it builds the PR *merge commit*, which is what actually lands, and
  it runs the sidekick and host-test jobs too. Stated 2026-09-04. A **field check** may run on either a local
  build or a CI image (`continuous` asset) — operator's call per check. The pass records which: the SHA
  (and whether `-dirty`), or the asset name.

- **Before patching a vendored library, diff a working reference.** When a bug traces into vendored or managed
  code (e.g. `usb_host_uac`), first compare how a working reference drives it (`poe_p4/main/qmx_transport.cpp` against the cited project
  (`SteffenLav/qmx-panadapter`). Patch the library only if those comparisons still point at it. The P4 UAC crash
  (2026-09-18) looked like a library bug but was our port running UAC open/start on the same task as the USB
  event pump. Stated 2026-09-18.

- **`webapp/app/` is source; what ships is its build** (`python3 webapp/tools/build.py`, after
  `npm ci --prefix webapp/tools` once per clone): minified into `webapp/dist/app/`, manifest written from the
  output, size budget checked ([architecture/webapp.md](docs/architecture/webapp.md), **Build**). Never commit a
  manifest or build output; both are ignored. Pages, CI's Host tests and `desk_push.py` each build, so an edit
  under `webapp/app/` needs no extra step. Before pushing one, run `tools/check.sh --changed`: it runs that build, as CI does.
  The committed manifest this replaced went stale on 2026-09-25 and again 2026-09-27, failing Pages both times.

- **Gloss roadmap IDs in chat.** Write "B44 (the sidekick pulls its own firmware over HTTPS)", not "B44".
  The operator does not hold the ID-to-topic mapping in their head, and a bare ID makes them go look it up
  mid-conversation. Applies to backlog (`B*`) and initiative (`I*`) IDs and RFC section numbers. Test-plan
  row IDs are fine bare when the surrounding text already says what the check does. Inside `ROADMAP.md` and
  commit messages the bare ID is correct — this is about chat. Stated 2026-09-10. **When asking for a
  decision, put all the context it needs in the chat itself** — what the row said, what was found, what each
  option does — so the operator never has to open a row, an RFC, or an earlier message to answer. Stated
  2026-09-24.

- **WSJT-X is the on-air standard.** Where our behaviour on the air differs from WSJT-X's (the message
  sequence, a message's format, what a tap sends), WSJT-X's is the default, and we differ only by a recorded
  decision. Check its source first: the `WSJTX/wsjtx` mirror, `widgets/mainwindow.cpp` for sequencing.
  Stated 2026-10-14.

- **Prose is authoritative; a diagram is derived.** Never let a fact live only in a diagram. Agents read
  the prose and may not reconstruct a constraint that exists only as an arrow, and a diagram that is the sole
  source of something goes stale without anyone noticing. Where a diagram earns its place, write it as
  **mermaid in the markdown**: GitHub renders it for humans, its source *is* the graph semantics rather than
  coordinates an agent has to re-derive, and a changed edge is a one-line diff instead of two pictures to
  compare. Do not commit SVG or raster diagrams to `docs/` — SVG costs an agent far more to read than the
  content is worth, and images diff and grep not at all. Stated 2026-09-10.

## Pre-alpha: do not carry the past

This project is **pre-alpha**. No external users, no fleet anyone else owns, no promise that anything keeps
working across a commit. Act accordingly: when something changes, change it and delete what it replaced. Do
not write migration notes, deprecation notes, or "was X, now Y" bookkeeping, and do not keep code whose only
job is accepting an older state.

The boundary matters, because over-applying this would delete the reasoning the rest of these docs are built
on:

- **Keep: why the code is the way it is now.** A constraint, an approach that failed, an API that behaves
  surprisingly. A future agent needs it to avoid re-breaking the thing. That is not legacy documentation.
- **Drop: how to get from an older state to this one.** Upgrade steps, one-time reflash rows, "an old link
  will 404", enum values retired but kept for an old file format.

The test: **would a fresh clone and a fresh flash need this line?** If only an existing install needs it, it
goes.

**The one exception is upstream.** Deltas from Wei's Micro-FT8 stay documented — the fork section of
[`README.md`](README.md) above the delimiter, and divergence notes in RFCs and roadmap rows. Someone
arriving from upstream needs to know what is different here, and an agent needs it to avoid "fixing" a
deliberate divergence back to upstream behaviour.

**This ends when we cut a versioned release meant for someone other than the operator**
([RELEASE_PROCESS.md](docs/RELEASE_PROCESS.md)). From then on back-compat is a real obligation and this section
comes out.

## Memory lives in this repo

Agents with a private per-machine memory store must not use it for anything about this project. These docs
are the memory: `AGENTS.md` for working agreement and operating notes, `ROADMAP.md` for plan and history,
`TEST_PLAN.md` for the field checks still owed and what has never run on hardware, `STYLE.md` for code conventions.

**Why:** a private store is scoped to one machine and one account. It is invisible to the operator, to a
second agent on another machine, to a future operator, and to code review — so it silently diverges from the
repo and cannot be corrected by anyone who has not got it. Four such notes existed on 2026-09-10 and were
migrated here; one of them duplicated a `ROADMAP.md` row and said so in its own text, and another stated a
commit-hygiene rule that the agent holding it broke twice the same day. A rule only one agent can read is
not a rule.

**How to apply:** when the operator states a preference worth keeping, put it in the right doc in the same
turn and propose the commit. If something genuinely should not be published, say so rather than filing it
somewhere the operator cannot see — this repo is public.

## Local IDF build / flash

**Local build first. CI is not an iteration loop.** Iterate with a local `idf.py build` in `poe_p4/` and hand the operator the merged image straight from `poe_p4/build/poe_p4_merged.bin`. Do **not** push a branch and wait on a CI artifact merely to produce a test binary: during bench debugging that latency dominates the session. CI is for **pre-merge verification** (see the bullet above).

Clone with submodules first (see **Clone (submodules)** above). After `source` of `esp-idf/export.sh` (v5.5.5):

```bash
cd poe_p4
idf.py build flash
```

Do not split into separate `build` then `flash` unless the user asks. Flash over the **PROG** USB-C port next to the RJ45; the QMX stays on the Host port, so `idf.py monitor` works with the radio attached ([architecture/main_board.md](docs/architecture/main_board.md), **Two USB-C ports**).

The sidekick is its own project: `cd sidekick && idf.py build flash`. `tools/merge_sidekick_bin.sh` merges its build into one `0x0` image.

Loader stubs (`AGENTS.md`, `CLAUDE.md`, `.cursor/rules/`, and similar) exist only so a given tool loads this agreement. They point here. Do not duplicate the plan in those files. `AGENTS.md` states the rule; adding a tool means adding a stub, never moving rules into it.
