# Working agreement (any agent)

This repo is tool-agnostic, and this file is the working agreement for every agent, whoever it works for.
Tool-specific files (`CLAUDE.md`, which imports this file, `.cursor/rules/`, and any future equivalent) load
it and carry no rules of their own; if one disagrees with this file, the stub is the bug. Supporting a new
tool means adding a stub, never copying or moving rules into it. Do not adopt a vendor-specific format for
anything that belongs here or in `docs/` — workflow, style, or the roadmap and its epics.

**Workflow matches documentation.** Committed docs are the workflow: this file, the roadmap (`ROADMAP.md` and its epic files), `STYLE.md`, and the architecture notes. Cite the relevant doc when proposing or making a change.

**A line earns its place here if it changes what an agent does.** If it only explains how a rule came about, it belongs in the commit message, not in this file.

## Who you work for

- **The person you work for** is whoever is driving you: the maintainer, or a contributor. They review in
  chat and say when to commit and push; "they" below means them.
- **The maintainer** (Jeff, KB2SLO) orders the roadmap, decides epics, reviews and merges pull requests,
  cuts releases, and runs the upstream watches. What only the maintainer does is in
  [docs/maintainer/](docs/maintainer/README.md); do it only when the person you work for is the maintainer.
- **An operator** is someone running a station. Where these docs say "operator", they mean that person,
  not the one you work for.

## Starting a session

Read `docs/ROADMAP.md` (what is next; Ideas are in `docs/roadmap/ideas.md`, read at intake, not here) and the architecture doc for the area you touch; before touching an epic's rows, read its file under `docs/roadmap/`. Follow
[STYLE.md](docs/STYLE.md) for our C/C++. How a change gets from a branch onto `main`:
[CONTRIBUTING.md](CONTRIBUTING.md). `tools/check.sh` runs every check CI runs; `--changed`, the ones a
change can affect. Working for the maintainer (they say so, or `git config user.name` is theirs), also read
[docs/maintainer/README.md](docs/maintainer/README.md) and do its **Session start**.

## The work

**Push back before tools.** Classify the request against `ROADMAP.md` Now, the epic in focus, and `STYLE.md` **before any edit of implementation, or before exploration spanning more than one file.** 

- **Working for the maintainer:** if it is not the current Now theme, not an explicit exception in those docs, or is still Ideas/Backlog (including “quick” UI/product tweaks), the first reply cites the doc and talks it through. Tools only after the maintainer retracts, grants an exception, or updates the doc in the same turn — an exception is an explicit decision, not a shortcut, and never a silent divergence.
- **Before proposing a new row, epic or idea,** give a verdict first: whether it is worth doing, from the need it meets and the evidence that the need bites (a bug, a bench result, a row), what in the tree already covers it, and what it costs to keep while the code is pre-alpha; and name the smallest slice that gets most of the value. "Not worth it" is a valid first reply, and refining comes only after the verdict holds, however the request is worded ("help me refine it" included). Then search [docs/roadmap/declined.md](docs/roadmap/declined.md) for it, and cite the line that matches; **and test it against the epic criteria** ([docs/maintainer/roadmap.md](docs/maintainer/roadmap.md), **Epics**), saying the outcome in the proposal: a child of an existing epic (named, with its entry and **Lands in:** line drafted), the start of a new epic (the criteria it meets, and the epic proposed rather than the row), or a plain row (why: one row's work, or nothing that must outlive it). Do this however the request is worded: "add a row" or "add to the backlog" names a thing; its shape is the test's.
- **Working for a contributor:** a bug fix can go straight to a pull request. Anything else — a feature, a renamed label, a changed behaviour, a slice of a larger plan — needs an issue the maintainer has agreed to, however well a host test proves it: a test shows a change works, not that it is wanted; with none, draft the issue in chat (what, why, Done-when) and stop there.

No open-ended codebase exploration to be helpful — that is exactly the token spend this rule exists to stop. A single cheap lookup (one grep, one file read, "where is X defined") can just be answered without the gate. Docs and roadmap questions are allowed without it.

**Smallest fix first.** Do not build tooling, scanners, CI gates or config files nobody asked for. If a larger approach seems warranted, propose it in two or three sentences and wait for a yes before writing it. Throwaway scaffolding in a scratch directory, never committed, is not tooling. A scratch script that produces a result the docs record, or that would be run again to re-check one, is not throwaway: commit it next to the result it produced, for example a scorer beside its corpus in `tools/decode_bench/`.

**Smallest means the smallest correct fix, not the least code today.** Lead with the fix that stays, even when it costs a reflash or another board, and say what it costs and where it belongs. Code the correct fix would later delete is not a smaller fix: offer such a stopgap only when the correct fix is blocked, and say plainly that it is temporary and what replaces it.

Do not mix a feature change with an unrelated fix in the same commit.

**The roadmap is the maintainer's.** A change that ships a `ROADMAP.md` row drops the row in the same change. Any other roadmap edit — a new row, a new order, a move between sections — is the maintainer's ([docs/maintainer/roadmap.md](docs/maintainer/roadmap.md)), and goes on its own branch from `main`, never into a work branch, and straight to `main` with no pull request ([docs/maintainer/roadmap.md](docs/maintainer/roadmap.md), **Roadmap intake**, direct to `main`). A contributor's new idea is an issue.

**Fix defects in the slice.** A defect found while working in an area gets fixed in that slice, not filed for later. What was wrong and why is still recorded — in a `ROADMAP.md` row when working for the maintainer, in the pull request otherwise — and it is not a promise to act later.

Judge by **verifiability**, not size, and say which of the three applied:

1. **You can prove it** — host test, dead-code proof (linker map plus `nm`, byte-identical binary), or the compiler. Fix it and prove it in the same commit.
2. **Only someone with the hardware can prove it** — anything needing the radio. Fix it and add the bench check to [TEST_PLAN.md](docs/TEST_PLAN.md) **Pending** with the exact steps and expected results.
3. **Neither can, yet** — an unreproducible hardware or timing bug. Prefer a change whose **worst case is no worse than current behaviour**, and ship the logging that makes the next occurrence diagnosable. Never let an unverified fix blend in with verified work: say plainly, in the commit and in chat, that it is unproven.

An unproven fix is still worth making. Pretending it is proven is not.

**Tier the model to the task, the same way work is tiered by verifiability above.** An agent can prove some things and not others; a model can carry some kinds of work well and not others, and a wrong choice here is not caught by the compiler the way a bad fix is. State which tier a task is before starting non-trivial work:

1. **Mechanical, low blast radius** — `git mv`-shaped extracts, renames, doc formatting, running `tools/check.sh` and reporting the result, applying a change the person you work for already fully specified. Verified by the compiler or a host test regardless of which model did it — use whatever is fast and cheap.
2. **Judgment under ambiguity** — drafting an epic, sizing a memory/timing budget, choosing an extract boundary, moving an epic's design into an architecture doc, changes to this process, anything **Push back before tools** above would stop for. Use the strongest available model. Nothing mechanical catches a wrong call here, which is exactly what makes it expensive.

No model tier turns a hardware-only check into one an agent can close (**Never report a field check as passed**, below).

**Test plan matches the work too.** [TEST_PLAN.md](docs/TEST_PLAN.md) is a ledger, not a manual, and is updated in the **same turn** as the work:

- Touched a field-only path (USB/UAC, CAT, Port A, WiFi, flash, TX timing) → say in chat what to flash, press or run and what result means it passed, and add **one Pending row** naming the change and that check, written as numbered steps, each with its expected result. "Test the radio" is not a check; "unplug and replug the QMX: the radio bar reads No radio, then Ready" is. The chat is how the person you work for hears about it; the row is how it is not forgotten. How a check is asked for and closed: [TEST_PLAN.md](docs/TEST_PLAN.md), **Pending**.
- Added a host test → nothing to record here. What a test pins lives in its own source.
- Added, removed or renamed a check in the **Automated** table (a harness, a CI job) → update that table in the same commit.
- Shipped something no check is planned for → add it to **Unverified**, so it is not mistaken for proven.
- Removed code that was provably never in the image (linker map plus `nm` on the ELF, byte-identical `poe_p4.bin`) → say so and add no row. That evidence *is* the test.

State which of these you did in the turn you report the work, and in the pull request's **Audits**. **"No test-plan change needed" is a valid answer and must be said out loud**, exactly like the README and site audits — an audit you skipped and an audit you did are otherwise indistinguishable.

**Architecture docs match the code too.** A `docs/architecture/<area>.md` describes how things work now, so a change that alters behaviour one of them describes updates it in the **same commit**. As with the README and site audits, say it out loud when nothing needs changing: "no architecture-doc change needed". When an epic's row ships, the same commit moves what is now true out of the epic file and into the docs the row's **Lands in:** line names, and the pull request's audit says what moved where ([docs/maintainer/roadmap.md](docs/maintainer/roadmap.md), **Epics**). Detail deleted from an epic file without a move is lost. **Unbuilt design lives in the epic file, not the architecture doc.** An architecture doc may mention a decided but unbuilt change only in a sentence pointing at its epic, under a label naming the rows that build it — "**not yet built: rows [v697](docs/roadmap/ota-updates.md#v697), [hstv](docs/roadmap/ota-updates.md#hstv)**" — so that when those rows ship and are dropped, `tools/roadmap_rows.sh dangling` flags the stale label.

**Never report a field check as passed.** An agent runs `tools/check.sh` and reports its summary honestly. **A run is not clean unless it exits 0 with nothing skipped.** Before proposing a commit or a push, run `tools/check.sh --changed`; the pre-push hook (`.githooks/pre-push`, CONTRIBUTING.md **Setup**) runs it again on every push and refuses a push that fails it or would push something other than the checked-out commit, and `git push --no-verify` needs the same explicit say-so as `--allow-skip`. If it exits non-zero, or any check skipped or is allowed to skip, the first line of the reply says which checks did not run and why, and no commit or push is proposed until the cause is fixed. Pass `--allow-skip` only when the person you work for says to in so many words, and report what it left unverified wherever you report the result. Anything needing the P4, a radio, or an antenna is for someone with the hardware to run: propose it, put it in **Pending**, and wait for their result, step by step. A green build is evidence about linkage, not about whether the radio transmitted.

**What the person you work for reports from the hardware is evidence.** When they say something works on the bench or in the field, treat it as verified: do not call it unverified in chat or in a doc because you have not run it yourself. A TEST_PLAN **Pending** row still closes only on a result for each step.

**README and site stay current.** Two front doors, two readers, one home for each fact:

- **[`README.md`](README.md) is for contributors:** what the project is, its status, the hardware, how it works in one paragraph, the repository layout, build and test, how to contribute, acknowledgements, license.
- **The site (`webapp/`, published at micro-ft8.kalikstein.com) is for operators:** what it does, screenshots, what is verified on the air, downloads, and its **Experiments** section (`webapp/experiments/`), measurements as they ran. Setup is [`webapp/getting-started.md`](webapp/getting-started.md), which is also the printed sheet.

**The README restates nothing that has another home.** It says a thing in a sentence and links to where it lives: setup to `CONTRIBUTING.md`, what is verified to `TEST_PLAN.md`, how an area works to its architecture doc, features to the site. No feature list, changelog, release notes, on-screen labels, or numbers that live in code. Every line it holds is a line that can go stale, so the cheapest way to keep it current is to keep it short. `tools/check_docs.py` fails on a broken link or path in it, and on an ESP-IDF version CI does not build with.

**Audit the README in the same turn** when a change adds, alters, or **removes** any of: a supported board or radio, or one first verified on hardware; a build, flash, or check command; the toolchain version; a top-level directory; the project's status (a release cut, pre-alpha ending); the contribution path (`CONTRIBUTING.md`, the issue rule); or a dependency, a submodule, or a project whose code or design we drew on (**Acknowledgements**).

**Audit the site in the same turn** when a change adds, alters, or **removes** any of: a web-app screen or control; an on-screen string the site or Getting started quotes; a documented workflow (log offload, flashing, time sync, pairing); a supported radio or board; what is verified (a field check that passes moves a claim to **Verified on the air**, and one that fails takes it down); or an experiment (a run added to one under `webapp/experiments/`, or the numbers in a corpus README one draws on changed). `webapp/tools/getting_started.mjs` fails when a label Getting started quotes leaves the UI; nothing checks the landing page, so read it. **The site describes what ships, and what we measured** ([docs/README.md](docs/README.md)): nothing unbuilt beyond its **Coming soon** list, nothing only upstream had, and a measurement only in **Experiments**, dated and labelled an experiment.

**Removal counts, and is the easy one to miss**: a page documenting a control that now does nothing is worse than one that never mentioned it. For either audit, propose the add, change, or drop in chat with the test plan and commit message, and put the accepted edit in the same commit as the work. When it does not apply, say so in that turn: "README: no change needed", "site: no change needed". Agent, roadmap, extract-only and other work nobody outside the repo can see triggers neither.

**Credit what we use, and never copy GPL code.** This repository is MIT. Code from a permissively licensed project keeps its copyright notice and is credited in the README's **Acknowledgements** in the same change. A GPL project, such as WSJT-X or qmx-panadapter, may be read to learn how something works and credited as a reference, but none of its code is copied or translated line by line into this tree; write from the behaviour, not the source.

What belongs in `poe_p4/main/`: [STYLE.md](docs/STYLE.md). Radio and `ft8_lib` boundaries: [architecture/radio.md](docs/architecture/radio.md), [architecture/ft8_lib.md](docs/architecture/ft8_lib.md).

Changes reach `main` (`kb2slo/micro-ft8`) through a pull request ([CONTRIBUTING.md](CONTRIBUTING.md)), except three kinds of the maintainer's change, which go straight to `main` with no pull request: roadmap-only ([docs/maintainer/roadmap.md](docs/maintainer/roadmap.md), **Roadmap intake**, direct to `main`), test-plan-only ([TEST_PLAN.md](docs/TEST_PLAN.md), **Pending**), and agent-only, touching nothing but this file, the tool stubs that load it and `docs/maintainer/`. Each is committed on a branch from `main`, checked with `tools/check.sh --changed`, and pushed to `main` when the maintainer says to push; `CONTRIBUTING.md` and `STYLE.md` bind contributors too, so a change to them is a pull request. Its base is always `main`: never stack a pull request on another branch (a merge there never reaches `main`), and state the base in chat before opening one. There is no `upstream` git remote: Wei's `wcheng95/Mini-FT8` is watched for portable fixes ([docs/maintainer/upstream.md](docs/maintainer/upstream.md)), not tracked as a fork. Never push to, or open a pull request on, any repo but this one unless the person you work for asks in so many words.

## Reproducibility

The full dev environment — build, test, and the sidekick field-flash workflow — must be reproducible from a fresh `git clone` (submodules included) plus a capable agent, with no dependency on state that exists only on one person's machine. When a step genuinely needs a one-time physical-hardware action (e.g. capturing a sidekick's factory-state backup before it is ever field-flashed), document the exact command in the relevant README/script rather than committing a derived artifact whose redistribution terms aren't ours to decide. `sidekick/README.md`'s stock-backup section is the current example of this pattern.

**No workstation OS is assumed.** Tools, scripts and documented steps run on Linux, macOS and Windows alike: prefer cross-platform pieces (Python, pyserial, PortAudio) over one OS's own tools, and never name an OS where a step works on any. A step that truly needs one OS says which and why.

## Chat gates

The person you work for reviews in chat, then green-lights each step. A good previous turn is not permission to skip a gate.

1. **Design in chat.** Architecture and “should we” questions get an answer first. Do not implement until they say to (e.g. “go”, “do it”). Docs and roadmap questions do not need that wait.

2. **Test plan with the result.** When work is done, the reply includes a short test plan: host command(s) they can run (`tools/check.sh --changed` runs every host check the change can affect), and a field check if the path is field-only. They often run tests and flash themselves and will say when it is solid. Do not flash the device unless they ask.

3. **Commit message in chat.** Propose the message here: a subject of one sentence, at most 72 characters, saying what changed, then a blank line and the why in the body. `tools/check_commits.py` holds commits to that shape, in CI on pull requests, and with them a pull request's title: a squash merge makes it, or a lone commit's subject, the subject on `main`, with ` (#N)` added. Also propose the README and site deltas, or say why neither needs a change (**README and site stay current**). Wait for them to accept or edit. Do not commit in the same turn as the proposal unless they already asked to commit.

4. **Commit / push only when asked.** Local edits are fine when they asked for the work. `git commit` waits until they have seen the plan or diff and said to commit. `git push` waits until they said to push, and so does opening a pull request. Before opening a pull request or changing its title, run `python3 tools/check_commits.py origin/main..HEAD --pr-title "<title>" --pr-number <N>`, N being the next pull request number (`gh pr list --state all -L 1 --json number`, plus one), and open or retitle it only when that passes: CI checks the subject a squash merge would write, the title and ` (#N)`, and an eyeballed count misses the suffix. “Commit and push” is both, using the agreed message. Do not treat “ship on main”, a green host test, or “local commits are fine” as standing permission. The one standing exception is an approved Ideas-only roadmap change, which is pushed straight to `main` ([docs/maintainer/roadmap.md](docs/maintainer/roadmap.md), **Roadmap intake**, direct to `main`).

   **"Push" means `git push` and nothing else**, in chat and in these docs. Putting the web app on a sidekick is *installing the app* (`webapp/tools/install_app.py`), and writing firmware to a board is *flashing*: neither is a push, and permission for one is never permission for the other.

   **A scope instruction is not a push instruction, even mid-session.** Each push needs its own explicit trigger in that turn — "push", "commit and push", or equivalent — not inferred from momentum, from how the last few turns went, or from the change being correct and well-tested.

5. **Undo with git, not a second edit.** Do not invert your last turn by re-editing files. That is slow and easy to get wrong when other uncommitted work is in the same tree.
   - Already a commit: `git revert` (or drop a *local, unpushed* commit only if they asked).
   - Uncommitted, and those paths contain *only* the work to undo: `git restore -- <paths>`.
   - Uncommitted mixed with other dirty files: do **not** `git restore` (that drops the other work). One sentence in chat: cheap undo needs a commit. Ask them to checkpoint, then revert.
   Before an experiment they may want to unwind, ask for a checkpoint commit first.

## Review before commit — nominate what is worth the reviewer's eyes

Reviewers trade review time for speed and do not read every line. So do the triage: **with every commit
proposal, and in every pull request's Worth your eyes section, give a list of one to three items**, each a
`file.cpp:line` and one sentence on what could be wrong, then say plainly that the rest is mechanical. An
unranked diff handed over is the same as handing over nothing.

Prefer, in this order:

1. **What needs the reviewer's domain knowledge and not the agent's** — what a real ADIF consumer accepts,
   what an operator expects a control to do, whether a report format is right. The agent cannot grade these at
   all.
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

Working preferences the maintainer has stated, for every agent. They live here, in the repo, because that is
the only place every agent and every person can see them — see "Memory lives in this repo" below.

- **Build locally; do not round-trip CI for a test binary.** `idf.py build` is ~40 s against ~5 min for the
  firmware CI job, and during bench debugging that latency dominates. The merged image is
  `poe_p4/build/poe_p4_merged.bin`. CI is for pre-merge verification, where its value is real: it builds the
  PR *merge commit*, which is what actually lands, and it runs the sidekick and host-test jobs too. A **field
  check** may run on either a local build or a CI image (`continuous` asset) — the call of whoever runs it.
  The pass records which: the SHA (and whether `-dirty`), or the asset name.

- **Before patching a vendored library, diff a working reference.** When a bug traces into vendored or managed
  code (e.g. `usb_host_uac`), first compare how a working reference drives it (our
  `poe_p4/main/qmx_transport.cpp` against `SteffenLav/qmx-panadapter`). Patch the library only if those
  comparisons still point at it. Read the library's changelog since our pin too (`tools/dep_watch.py` links
  it): the fix may already be released.

- **`webapp/app/` is source; what ships is its build** (`python3 webapp/tools/build.py`, after
  `npm ci --prefix webapp/tools` once per clone): minified into `webapp/dist/app/`, manifest written from the output,
  size budget checked ([architecture/webapp.md](docs/architecture/webapp.md), **Build**). Never commit a
  manifest or build output; both are ignored. Pages, CI's Host tests and `install_app.py` each build, so an edit
  under `webapp/app/` needs no extra step. Before pushing one, run `tools/check.sh --changed`: it runs that
  build, as CI does.

- **Link every roadmap row you name, in chat and in any `.md` file.** A row's anchor is its ID: a row in an
  epic links to its entry in the epic file, any other row to its row in the index, a declined one to
  `declined.md`, and an epic to its file. In a doc the link is relative (`[u4dw](../ROADMAP.md#u4dw)`). In chat
  it is an absolute GitHub URL, since a chat surface has no repo to resolve a relative one against:
  `https://github.com/kb2slo/micro-ft8/blob/main/docs/ROADMAP.md#u4dw`, on `main` when the row is there, or on the
  pushed branch that holds it; for a row only on an unpushed branch, say so and gloss it, since no link can reach it
  yet. `tools/check_docs.py` fails a roadmap link whose anchor is gone. Left bare: commit messages and pull request titles, code comments, code spans and headings.
- **In chat, name a row, epic or child by what it is, not its ID** (maintainer, 2026-11-09): "the transmit
  watchdog that stops a runaway transmitter", linked as above where a link helps, not "u4dw". Nobody holds the
  ID-to-topic mapping in their head. Give the ID only where a decision or an edit needs the exact row, and then
  beside the name. Applies to every row ID (the four-character ones and the older `B*`/`I*`) and epic section
  numbers. Test-plan row IDs are fine bare when the surrounding text already says what the check does.
  **When asking for a decision, put all the
  context it needs in the chat itself** — what the row said, what was found, what each option does — so the
  person you work for never has to open a row, an epic file, or an earlier message to answer.

- **Times in chat are the maintainer's local time, US Central.** Logs, commits, ADIF and the docs keep
  UTC; a time relayed in chat is converted and labelled (CST or CDT). A clock with no zone, such as a
  logger's, is checked against a known event before it is relayed.

- **Read an outside branch before running anything on it.** A contributor's branch can change any script in it,
  including the read-only checks `.claude/settings.json` lets run without a prompt and the pre-push hook. Before
  running a check, a test or a build on such a branch, the person you work for has seen its diff. A tool's own
  permission file, like that one, lists only read-only checks and carries no rules of its own.

- **An agent cannot answer a password prompt.** For `sudo` or any command that asks for a password, give
  the exact command, ask the person you work for to run it in their own terminal, and wait for them to say it
  ran. Do not run it through the agent's shell.

- **Long runs report as they go.** For an on-device test or any run longer than a few minutes, give a status
  at each estimated checkpoint; when an estimate passes, say why and give a new one. Before starting, remind
  the person you work for to keep their machine from sleeping; how is theirs to choose.

- **Read the station before asking about it.** On the bench the sidekick is on the LAN at `http://micro-ft8.local`, and its reads need no token ([sidekick.md](docs/architecture/sidekick.md), **Port A: the link to the main board**): `GET /api/config` is the station's settings (call, grid, radio, band, active bands), `GET /api/state?slots=<n>` the station as it is now, and `GET /api/events?since=<seq>` every decode and radio event as Operate shows it, and the three builds a bench pass records: the sidekick's in `/api/config` (`sk_fw_version`), the web app's as the `<sha> <date>` string in `GET /app.html` (what Settings shows as **App:**), and the P4's in its boot log (`App version`). The sidekick keeps only the last 128 events, under a minute on a busy band, so to follow a run, poll every few seconds. The P4's own console is its PROG port (an Espressif USB serial device, `/dev/ttyACM*` on Linux). Before asking the person you work for what the station shows or is set to (decodes, band, grid, radio), read it there, and ask only for what neither shows: a screen, a sound, a cable.

  **Token-guarded calls** (`PUT /api/config` and the rest) go through `webapp/tools/sidekick.py`, as `install_app.py` does. It caches the pairing token at `webapp/tools/.sidekick_token` (git-ignored, owner-only), claims one with the sidekick's button the first time or when the cache is refused, and retries on a 401. Each write needs the person you work for to approve it explicitly, in that turn: say what you will send and what it changes, then wait. A write that can make the station transmit (a CQ, a reply, Tune) also needs them at the station as its control operator, since the licence is theirs. What an agent reads this way is evidence from a run they set up; the TEST_PLAN rules on who closes a field check are unchanged.

  **One write needs no approval: setting the clock.** When the main board's clock is unset (SLOT indices count from boot, events carry `t` 0) or reads more than a second off, an agent sends `POST /api/time` with its own machine's time, the call the web app makes, provided that machine's clock is synced to NTP (check it first), and says in chat that it did. It sets the clock and nothing else, so it cannot key the radio or change a setting. Every other write still needs approval in that turn.

- **WSJT-X is the on-air standard; station policy is ours.** On the wire WSJT-X decides: a message's format,
  what each message of a QSO means and which follows which, what a tap sends, reports, timing and parity. We
  differ there only by a recorded decision; check its source first (the `WSJTX/wsjtx` mirror,
  `widgets/mainwindow.cpp` for sequencing). **Station policy** (whom to call or answer and in what order,
  when, how many times, how many QSOs are queued) has no single standard: WSJT-X's is deliberately minimal,
  and JTDX, MSHV, FT8CN and WSJT-X Improved go further. Decide it on evidence, say what those engines do, and
  record it in [autoseq.md](docs/architecture/autoseq.md), a difference from WSJT-X included, so nobody
  "fixes" it back. What binds station policy is the control operator's licence and award rules such as
  ARRL's contemporaneous initiation, not WSJT-X.

- **Prose is authoritative; a diagram is derived.** Never let a fact live only in a diagram. Agents read
  the prose and may not reconstruct a constraint that exists only as an arrow, and a diagram that is the sole
  source of something goes stale without anyone noticing. Where a diagram earns its place, write it as
  **mermaid in the markdown**: GitHub renders it for humans, its source *is* the graph semantics rather than
  coordinates an agent has to re-derive, and a changed edge is a one-line diff instead of two pictures to
  compare. Do not commit SVG or raster diagrams to `docs/` — SVG costs an agent far more to read than the
  content is worth, and images diff and grep not at all.

## Pre-alpha: do not carry the past

This project is **pre-alpha**. No promise to anyone — a contributor, or an operator trying an early build —
that anything keeps working across a commit; [CONTRIBUTING.md](CONTRIBUTING.md) and the site say so. Act
accordingly: when something changes, change it and delete what it replaced. Do not write migration notes,
deprecation notes, or "was X, now Y" bookkeeping, and do not keep code whose only job is accepting an older
state.

The boundary matters, because over-applying this would delete the reasoning the rest of these docs are built
on:

- **Keep: why the code is the way it is now.** A constraint, an approach that failed, an API that behaves
  surprisingly. A future agent needs it to avoid re-breaking the thing. That is not legacy documentation.
- **Drop: how to get from an older state to this one.** Upgrade steps, one-time reflash rows, "an old link
  will 404", enum values retired but kept for an old file format.

The test: **would a fresh clone and a fresh flash need this line?** If only an existing install needs it, it
goes.

**The one exception is upstream.** Where code here still shares lineage with Wei's Mini-FT8 (the
sequencer, logging, the ft8_lib integration), a deliberate divergence stays documented in the architecture
doc, epic file or roadmap row for that area, so an agent porting a fix from upstream does not "fix" it back to
upstream behaviour ([docs/maintainer/upstream.md](docs/maintainer/upstream.md)). The README credits
Mini-FT8; it does not list what changed since.

**This ends when we cut a versioned release meant for someone other than the maintainer**
([RELEASE_PROCESS.md](docs/RELEASE_PROCESS.md)). From then on back-compat is a real obligation and this section
comes out.

## Memory lives in this repo

Agents with a private per-machine memory store must not use it for anything about this project. These docs
are the memory: `AGENTS.md` for working agreement and operating notes, `ROADMAP.md` and its epic files for the plan,
`TEST_PLAN.md` for the field checks still owed and what has never run on hardware, `STYLE.md` for code conventions.

**Why:** a private store is scoped to one machine and one account. It is invisible to the person you work for, to a
second agent on another machine, to a future maintainer, and to code review — so it silently diverges from the
repo and cannot be corrected by anyone who has not got it. A rule only one agent can read is not a rule.

**GitHub issues are discussion, not memory.** An issue is where an idea or a report arrives and is talked
through; it is never the only home of a decision. The pull request that acts on one puts the outcome in these docs
and cites `#N`. Read issues freely; comment on, open or close one only when the person you work for says to in that
turn, as for a push: an issue is public ([docs/maintainer/roadmap.md](docs/maintainer/roadmap.md), **Issues**).

**How to apply:** when the maintainer states a preference worth keeping, put it in the right doc in the same
turn and propose the commit. A contributor's preference about how this repo works is a pull request to that
doc, which the maintainer accepts or not. If something genuinely should not be published, say so rather than
filing it somewhere the maintainer cannot see: write everything here as if it were public.

## Local IDF build / flash

Iterate with a local build (**Build locally**, above). Clone with submodules first ([CONTRIBUTING.md](CONTRIBUTING.md), **Setup**). When they ask you to flash, after `source` of `esp-idf/export.sh` (v5.5.5):

```bash
cd poe_p4
idf.py build flash
```

Do not split into separate `build` then `flash` unless they ask. Flash over the **PROG** USB-C port next to the RJ45; the QMX stays on the Host port, so `idf.py monitor` works with the radio attached ([architecture/main_board.md](docs/architecture/main_board.md), **Two USB-C ports**).

The sidekick is its own project: `cd sidekick && idf.py build flash`. `tools/merge_sidekick_bin.sh` merges its build into one `0x0` image.
