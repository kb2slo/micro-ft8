# docs/

Human- and tool-readable project memory. Prefer this tree over editor-specific rule files, vendor instruction stubs, or chat transcripts.

| File | Role |
|---|---|
| [ROADMAP.md](ROADMAP.md) | Plan: Next groom / Now / Backlog / Blocked / Ideas / Declined. Chat is intake; this file is truth. Shipped work is dropped, not archived — git history is the record. |
| [../README.md](../README.md) | Operator landing. Fork notes (why this tree) above the Wei delimiter; Wei’s original below. |
| [STYLE.md](STYLE.md) | Coding standard for our C/C++ (not vendored `M5*` / `ft8_lib`). |
| [TEST_PLAN.md](TEST_PLAN.md) | What is verified and by whom: agent-run automation vs operator field checks, plus field checks owed on the current branch. |
| [RELEASE_PROCESS.md](RELEASE_PROCESS.md) | How a build becomes a download: the rolling `continuous` tag, versioned `v*` releases, what a version tag does **not** set, and what a rename/de-fork would break. |
| [AUTOSEQ_ARCHITECTURE.md](AUTOSEQ_ARCHITECTURE.md) | Sequencer design (slot events, tick vs decode). |
| [AUTOSEQ_INACTIVE_QUEUE.md](AUTOSEQ_INACTIVE_QUEUE.md) | Retry exhaustion / reincarnation / inactive zone. |
| [FT8 Free-Text Reference Extension.md](FT8%20Free-Text%20Reference%20Extension.md) | Free-text / SOTA-style payload notes. |
| `rfcs/` | Design for work not yet built. Lifecycle and graduation: **Doc types** below. |

Git history is the changelog. Do not keep a narrative log in these files.

## Working agreement (any agent)

At the start of a session, read `docs/ROADMAP.md` (and the architecture or RFC for the area you touch). Follow [STYLE.md](STYLE.md) for our C/C++.

**Workflow matches documentation.** Committed docs are the workflow: this file, `ROADMAP.md`, `STYLE.md`, RFCs, and the architecture notes. Cite the relevant doc when proposing or making a change.

**A line earns its place here if it changes what an agent does.** If it only explains how a rule came about, it belongs in the commit message, not in this file.

### Doc types

Each document answers one question. Put content where its question is answered.

| Question | Where | Kind |
|---|---|---|
| What next? | `ROADMAP.md` | Plan rows |
| Should we, and how? (not built yet) | `docs/rfcs/NNNN-*.md` | RFC |
| How does this work now, and why? | `docs/<AREA>_ARCHITECTURE.md` | Architecture doc |
| How do I build, flash, or use this part? | That directory's `README.md` | How-to |
| What must two codebases agree on? | The code | One source of truth, plus a test holding the other side to it |
| What can an operator do with it? | `webapp/` → `micro-ft8.kalikstein.com` | Public site |

**No prose specs.** A contract that crosses a language or repo boundary (C ↔ webapp JS, our encoder ↔ WSJT-X) gets one side as the source of truth in code and a test that holds the other side to it — not a document restating it.

**The site describes what ships.** Nothing unbuilt, unmeasured, or upstream-only goes on it.

#### RFC lifecycle

An RFC is warranted when a row's design will not fit in the row (**Row size** below), when work spans several rows or boards, or when real alternatives are still undecided.

1. **Draft** — open questions, alternatives, an *Ask*. Changes freely. An RFC gets its number when its file is created, never earlier: a row for an unwritten RFC says "an RFC", not a number that could go stale or collide.
2. **Accepted** — the operator says so. Backlog/Now rows point at its sections. After this it changes only by dated decision.
3. **Graduate as each slice ships**, in the same commit as the slice:
   - Move what is still true into an architecture doc (new or existing): how it works, the constraints, measurements that still hold, rejected alternatives as short "why not X" notes.
   - Delete the rest — the Ask, phase plans, status history, resolved open questions. Git keeps them.
   - Repoint every citation of the moved sections (`git grep -n 'RFC NNNN'`) at the architecture doc. Code never cites a graduated RFC.
   - Commit message: `RFC NNNN: graduate §x–y to <doc>`.

   **Exception:** a slice only CI can validate (Worker or Pages deploy, release workflow, PR merge-commit build) graduates in the next commit, once CI is green; its commit message says "graduation pending CI". Field checks are not an exception — `TEST_PLAN.md` Pending tracks those.
4. **Graduated** — nothing undecided remains; the file becomes a stub:

   ```markdown
   # RFC NNNN: <title>
   * **Status:** Graduated YYYY-MM-DD → [<doc>](../<doc>)
   * Full text at graduation: `git show <sha>:docs/rfcs/NNNN-<slug>.md`
   ```

**Rejected** RFCs move to `docs/rfcs/rejected/` and stay whole and frozen — their reasoning is what stops the idea being re-proposed; out of `rfcs/`, they no longer sit beside live design work.

#### Roadmap rows

**Order is priority.** Within Now and Backlog, the top row is next. Grooming re-orders; a new row is placed deliberately, not appended by default.

**Blocked is a section, not a tag,** so Backlog order stays pure priority. A Blocked row names **Blocked on:** (the condition that clears it) and keeps a Done-when; an optional **Collects:** lists work that attaches to it if it is ever done. Order within Blocked does not matter. Same size limit as every row — an overgrown **Collects:** becomes an RFC.

**Declined is the last section.** A rejected row moves there as one line — ID, name, why — so the idea is not re-proposed. Git keeps the full row.

**`ADV-only` rows.** A row that only matters for the Cardputer ADV carries `` `ADV-only` `` at the start of its name and sits at the bottom of Backlog (or in Ideas); if ADV support is dropped (row `za6n`), they go with it — `git grep -n ADV-only docs/ROADMAP.md` finds them.

**IDs are opaque.** New rows get a random ID from `tools/roadmap_rows.sh new`, which checks it has never appeared in git history. Existing `B`/`I` IDs stay. Section is stage, position is priority; the ID encodes neither. Lineage — split from, merged into, replaced by — goes in the commit message that made the change, not in the row; `git log -S'<id>' --reverse -- docs/ROADMAP.md` finds where a row came from.

**Row size.** `tools/roadmap_rows.sh` lists rows over its `LIMIT` (the only place the number lives). Flag any such row in chat — in the turn that writes it, before proposing the commit, and at grooming. Route the overflow by kind:

- Design for unbuilt work → an RFC (an existing one if it fits).
- Facts about current code → an architecture doc.
- History ("built…", "extended…", "note, same day…") → delete; git has it.

What stays is ID, name, a line of why, Done-when, and a link. A row stays long only on the operator's say-so, marked `` `long-ok` `` so the script counts it. Each grooming retro reads the script's output: many `long-ok` rows mean `LIMIT` is too low; rows under it that still needed trimming mean it is too high.

**Push back before tools.** Classify the request against `ROADMAP.md` Now, the active RFC, and `STYLE.md` **before any edit of implementation, or before exploration spanning more than one file.** If it is not the current Now theme, not an explicit exception in those docs, or is still Ideas/Backlog (including “quick” UI/product tweaks), the first reply cites the doc and talks it through. No open-ended codebase exploration to be helpful — that is exactly the token spend this rule exists to stop. A single cheap lookup (one grep, one file read, "where is X defined") can just be answered without the gate. Tools only after the operator retracts, grants an exception, or updates the doc in the same turn — an exception is an explicit decision, not a shortcut, and never a silent divergence. Docs and roadmap questions are allowed without that gate.

Update `ROADMAP.md` in the **same turn** as the work:

- New idea → **Ideas** (do not implement until moved up)
- Agreed / sequenced → **Backlog**
- Decided or conditional, waiting on a named condition → **Blocked** (not picked up until it clears)
- Next to build → **Now** (one theme)
- Rejected → one line in **Declined**
- Shipped → drop the row from Now/Backlog. No separate Done archive — git history (and the commit that dropped the row) is the record.

Each row is an ID plus a name. Do not mix a feature change with an unrelated fix in the same commit.

**Fix defects in the slice.** A defect found while working in an area gets fixed in that slice, not filed for later. It still gets a `ROADMAP.md` row, but the row records what was wrong and why — it is not a promise to act later. Single-operator workflow: the reasons to defer (protecting someone else's sprint, batching context switches across a team) do not apply, and deferred fixes here have a poor track record of ever landing.

Judge by **verifiability**, not size, and say which of the three applied:

1. **You can prove it** — host test, dead-code proof (linker map plus `nm`, byte-identical binary), or the compiler. Fix it and prove it in the same commit.
2. **Only the operator can prove it** — anything needing the radio. Fix it and add the bench check to [TEST_PLAN.md](TEST_PLAN.md) **Pending** with the exact keys and expected result.
3. **Neither can, yet** — an unreproducible hardware or timing bug. Prefer a change whose **worst case is no worse than current behaviour**, and ship the logging that makes the next occurrence diagnosable. Never let an unverified fix blend in with verified work: say plainly, in the commit and in chat, that it is unproven.

An unproven fix is still worth making. Pretending it is proven is not.

**Tier the model to the task, the same way work is tiered by verifiability above.** An agent can prove some things and not others; a model can carry some kinds of work well and not others, and a wrong choice here is not caught by the compiler the way a bad fix is. State which tier a task is before starting non-trivial work:

1. **Mechanical, low blast radius** — `git mv`-shaped extracts, renames, doc formatting, running `host_mock`/`tx_e2e`/`idf.py build` and reporting the result, applying a change the operator already fully specified. Verified by the compiler or a host test regardless of which model did it — use whatever is fast and cheap.
2. **Judgment under ambiguity** — RFC drafting, sizing a memory/timing budget, choosing an extract boundary, RFC graduation, changes to this process, anything **Push back before tools** above would stop for. Use the strongest available model. Nothing mechanical catches a wrong call here, which is exactly what makes it expensive.
3. **Field-only, no model tier substitutes** — USB/CDC/CAT, UAC timing, TX behaviour, anything [TEST_PLAN.md](TEST_PLAN.md) marks Pending. This is category 2/3 of the verifiability rubric above wearing a model-choice hat: a bigger model does not turn an operator-only or hardware-only check into something an agent can close. Say so plainly rather than picking a bigger model and reporting more confidence than the check supports.

**Grooming spans tiers.** Running `tools/roadmap_rows.sh`, deleting history from rows, and applying decisions the operator already made are tier 1. Triage recommendations are tier 2 by default. The risk is not a wrong recommendation — the operator overrides it — but a finding never surfaced: a defect hiding in a row, a row contradicting a rule here, a stale reference. Weigh the model observations below when choosing, and ask for `file:line` or commit evidence behind every claim; verification, more than model size, is what surfaced findings.

**Model observations (triage).** One blind triage of the same roadmap per model, 2026-09-24 — a single sample each, so treat as leads, not verdicts. Add a line when a groom shows something new; delete a line a later groom contradicts.

- **Claude Opus** — strongest on judgment: defect severity (a TX-cancel bug as a safety issue), whether a row is still real work (obsolete, field-check-only, superseded-but-unbuilt). Missed stale facts that only a code check finds (a menu count, a dropped ID cited by an RFC). Flagged a row as breaking the pre-alpha rule from its text alone; the code had no such compat path (Composer made the same call).
- **Cursor Composer (Auto)** — strongest on verification: cited `file:line` for nearly every claim, swept dangling IDs broadly, caught stale counts and a parked defect conflicting with **Fix defects in the slice**. Weaker on severity ranking; dropped a row whose remaining work was unbuilt.
- **Claude Sonnet** — strongest on mechanical compliance (every missing Done-when, oversize row, dangling reference; the only one to catch an RFC citing a dropped row). Missed most judgment findings, and contradicted itself once against the pre-alpha rule.

**Test plan matches the work too.** [TEST_PLAN.md](TEST_PLAN.md) is the same kind of living file as the roadmap, and gets updated in the **same turn** as the work:

- Added or changed a host-testable unit (parse / format / policy) → add or update its row under `host_mock` coverage, in the same commit as the code.
- Touched a field-only path (USB/UAC, CAT, display, GPS, RTC, SD, flash, TX timing) → add a row to **Pending** naming the commit and the exact check. "Test the radio" is not a check; "MENU P3 `5` reports `Copied OK`" is.
- Removed code that was provably never in the image — linker map plus `nm` on the ELF, byte-identical `micro_ft8.bin` — say so and add no row. That evidence *is* the test.
- Removed a harness or a check → drop its row and say why in the commit message.

State which of these you did in the turn you report the work. **"No test-plan change needed" is a valid answer and must be said out loud**, exactly like the README audit — an audit you skipped and an audit you did are otherwise indistinguishable.

**Never report a field check as passed.** An agent runs `host_mock`, `tx_e2e`, and `idf.py build` and reports those honestly. Anything needing a Cardputer, a radio, or an antenna is the operator's to run: propose it, put it in **Pending**, and wait. A green build is evidence about linkage, not about whether the radio transmitted.

**Fork README stays current.** The top of [`README.md`](../README.md) (above the Wei delimiter) is the public “why this fork” copy. When work is operator-visible (TX UX, meters, logging, flash/Launcher, Charge Mode, CQ/beacon, radios, …), audit that section in the **same turn**: propose the add, change, or drop in chat with the test plan and commit message. Include the accepted edit in the same commit and push as the feature. Do not rewrite Wei’s original below the delimiter. Skip agent, RFC, extract-only, or other work operators cannot feel on the radio.

**Triggers, stated so they cannot be missed.** Audit that section when a change adds, alters, or **removes** any of: a key binding or screen; an on-screen string the README quotes; a documented workflow (log offload, flash/Launcher, charge, time sync); or a supported radio/board. **Removal counts, and is the easy one to miss** — a README documenting a key that now does nothing is worse than one that never mentioned it. If Wei’s original below the delimiter documents the same thing, do **not** edit it: put the correction in the fork note and let the delimiter rule (“use the fork notes above where they disagree”) carry it. When it genuinely does not apply, say so explicitly in that turn, as above. CI enforces the mechanical half of this (`readme-audit` in `.github/workflows/ci.yml`): a PR that removes a `UIMode` enumerator or changes a key binding under `main/` without touching `README.md` fails, unless the PR body says `README-audit: none - <why>`. The check catches signals, not judgement — it will not notice a behaviour change that alters no enum and no key.

Do not expand `main.cpp` without extracting a tested function. Extract/style/radio/`ft8_lib` campaign [RFC 0002](rfcs/0002-extract-and-boundaries.md) is Done. STYLE still applies. Radio-profile remainder is Backlog B14. Read the current Now from `ROADMAP.md` rather than trusting a name written here — this line has gone stale before.

This project ships on **`origin/main`** (`kb2slo/micro-ft8`, private). De-forked 2026-09-15 from `kb2slo/Micro-FT8` (now archived, public) — there is no `upstream` git remote. Wei's `wcheng95/Micro-FT8` is watched for portable fixes (**Upstream watches** below), not tracked as a fork; never push or open a PR there unless the operator explicitly asks.

### Clone (submodules)

`ft8_lib` is the git submodule `components/ft8_lib/vendor`. Host `tx_e2e` and `idf.py build` need it. GitHub’s default clone does not fetch it. The repo is private — clone needs your own GitHub auth (SSH key or PAT), not just the URL.

```bash
git clone --recurse-submodules git@github.com:kb2slo/micro-ft8.git
```

Existing trees: `git submodule update --init`. Pin and bump path: [RFC 0002](rfcs/0002-extract-and-boundaries.md) §6.

### Reproducibility

The full dev environment — build, test, and the sidekick field-flash workflow — must be reproducible from a fresh `git clone` (submodules included) plus a capable agent, with no dependency on state that exists only on one operator's machine. When a step genuinely needs a one-time physical-hardware action (e.g. capturing a Nano's factory-state backup before it is ever field-flashed), document the exact command in the relevant README/script rather than committing a derived artifact whose redistribution terms aren't ours to decide. `sidekick/README.md`'s stock-backup section is the current example of this pattern.

### Chat gates

The operator reviews in chat, then green-lights each step. A good previous turn is not permission to skip a gate.

1. **Design in chat.** Architecture and “should we” questions get an answer first. Do not implement until they say to (e.g. “go”, “do it”). Docs and roadmap questions do not need that wait.

2. **Test plan with the result.** When work is done, the reply includes a short test plan: host command(s) they can run, and a field check if the path is field-only. They often run tests and flash themselves and will say when it is solid. Do not flash the device unless they ask.

3. **Commit message in chat.** Propose the message here (match recent `git log`: one sentence, why). For operator-visible work, also propose the fork-README delta (or say why that section needs no change). Wait for them to accept or edit. Do not commit in the same turn as the proposal unless they already asked to commit.

4. **Commit / push only when asked.** Local edits are fine when they asked for the work. `git commit` waits until they have seen the plan or diff and said to commit. `git push origin` waits until they said to push. “Commit and push” is both, using the agreed message. Do not treat “ship on main”, a green host test, or “local commits are fine” as standing permission.

   **A scope instruction is not a push instruction, even mid-session.** Field-found 2026-09-17: after several rounds of the operator explicitly saying "commit and push" earlier in one long session, an agent then received "yes fix it now" — approval to make a specific correction — and pushed it without being asked, reasoning from the session's *pattern* rather than that turn's actual words. The rule above already said not to do this ("a good previous turn is not permission to skip a gate," restated here because it still happened): each push needs its own explicit trigger in that turn — "push", "commit and push", or equivalent — not inferred from momentum, from how the last few turns went, or from the change being correct and well-tested. Being right about the fix does not retroactively authorize the push.

5. **Undo with git, not a second edit.** Do not invert your last turn by re-editing files. That is slow and easy to get wrong when other uncommitted work is in the same tree.
   - Already a commit: `git revert` (or drop a *local, unpushed* commit only if they asked).
   - Uncommitted, and those paths contain *only* the work to undo: `git restore -- <paths>`.
   - Uncommitted mixed with other dirty files: do **not** `git restore` (that drops the other work). One sentence in chat: cheap undo needs a commit. Ask them to checkpoint, then revert.
   Before an experiment they may want to unwind, ask for a checkpoint commit first.

### Review before commit — nominate what is worth the operator's eyes

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

### Operating notes

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
  code (e.g. `usb_host_uac`), first compare how our other board already drives it successfully (ADV
  `main/audio/stream_uac.cpp` vs. P4 `poe_p4/main/qmx_transport.cpp`) and any cited reference project
  (`SteffenLav/qmx-panadapter`). Patch the library only if those comparisons still point at it. The P4 UAC crash
  (2026-09-18) looked like a library bug but was our port running UAC open/start on the same task as the USB
  event pump. Stated 2026-09-18.

- **Assume another agent may be in this worktree.** The operator sometimes runs a second Claude Code session
  on the same checkout. One worktree means one index, so a concurrent `git add -A` can sweep up files another
  session wrote seconds earlier and commit them under an unrelated message — this happened on 2026-09-08
  (`75e3ed0`). Re-check `git log -1` and `git status` immediately before committing rather than trusting
  state read earlier in the turn, and stage explicit paths rather than `-A` unless staging everything is
  genuinely the intent. If files have already been swept into someone else's pushed commit, say so and
  cross-reference that SHA; do not rewrite pushed history to reclaim them.

- **Gloss roadmap IDs in chat.** Write "B44 (the sidekick pulls its own firmware over HTTPS)", not "B44".
  The operator does not hold the ID-to-topic mapping in their head, and a bare ID makes them go look it up
  mid-conversation. Applies to backlog (`B*`) and initiative (`I*`) IDs and RFC section numbers. Test-plan
  row IDs are fine bare when the surrounding text already says what the check does. Inside `ROADMAP.md` and
  commit messages the bare ID is correct — this is about chat. Stated 2026-09-10. **When asking for a
  decision, put all the context it needs in the chat itself** — what the row said, what was found, what each
  option does — so the operator never has to open a row, an RFC, or an earlier message to answer. Stated
  2026-09-24.

- **Announce a field/bench test in chat, not just as a `TEST_PLAN.md` row.** When a change needs an
  operator-run check — anything landing in `TEST_PLAN.md`'s Pending table per this doc's verifiability
  tiers — say so directly in the same chat turn: what to flash/press/run, and what result confirms it
  passed. The doc row is the durable record; a row nobody was told about in chat is easy to leave unrun,
  since the operator does not necessarily re-read that file after every turn. Stated 2026-09-16.

- **Prose is authoritative; a diagram is derived.** Never let a fact live only in a diagram. Agents read
  the prose and may not reconstruct a constraint that exists only as an arrow, and a diagram that is the sole
  source of something goes stale without anyone noticing. Where a diagram earns its place, write it as
  **mermaid in the markdown**: GitHub renders it for humans, its source *is* the graph semantics rather than
  coordinates an agent has to re-derive, and a changed edge is a one-line diff instead of two pictures to
  compare. Do not commit SVG or raster diagrams to `docs/` — SVG costs an agent far more to read than the
  content is worth, and images diff and grep not at all. Stated 2026-09-10.

### Pre-alpha: do not carry the past

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
[`../README.md`](../README.md) above the delimiter, and divergence notes in RFCs and roadmap rows. Someone
arriving from upstream needs to know what is different here, and an agent needs it to avoid "fixing" a
deliberate divergence back to upstream behaviour.

**This ends when we cut a versioned release meant for someone other than the operator**
([RELEASE_PROCESS.md](RELEASE_PROCESS.md)). From then on back-compat is a real obligation and this section
comes out.

### Memory lives in this repo

Agents with a private per-machine memory store must not use it for anything about this project. These docs
are the memory: `README.md` for working agreement and operating notes, `ROADMAP.md` for plan and history,
`TEST_PLAN.md` for what has and has not been verified on hardware, `STYLE.md` for code conventions.

**Why:** a private store is scoped to one machine and one account. It is invisible to the operator, to a
second agent on another machine, to a future operator, and to code review — so it silently diverges from the
repo and cannot be corrected by anyone who has not got it. Four such notes existed on 2026-09-10 and were
migrated here; one of them duplicated a `ROADMAP.md` row and said so in its own text, and another stated a
commit-hygiene rule that the agent holding it broke twice the same day. A rule only one agent can read is
not a rule.

**How to apply:** when the operator states a preference worth keeping, put it in the right doc in the same
turn and propose the commit. If something genuinely should not be published, say so rather than filing it
somewhere the operator cannot see — this repo is public.

### Roadmap intake (ideas)

If the operator asks to add an idea or “add to the backlog”:
1. Draft the `docs/ROADMAP.md` row in chat. Do not commit yet.
2. New items go in **Ideas**. Use **Backlog** only when they give a Done-when or say it is sequenced.
3. After they approve, commit that row on `main`. Push `origin` only when they ask to push. Never `upstream` unless they explicitly ask.

### Backlog grooming

**A groom is a timeboxed session.**

1. **Open:** ask the operator for a rough timebox (e.g. 30 min, 1 h) and tune the pace to it. Read `ROADMAP.md`'s **Next groom** section — carried items come first. Run `tools/roadmap_rows.sh health` if you can and state the budget in one line.
2. **Passes, in order, until the timebox runs out:** ① Now — drop what shipped, move out what is not the one theme; ② Backlog — drops, demotions, splits, missing Done-whens, over-size rows, then order; ③ Blocked — does each condition still hold; ④ Ideas — drops, Declined, over-size rows; ⑤ pick Now (one theme).
3. **Wrap:** re-run `health`; rewrite **Next groom** in `ROADMAP.md` with what was not reached (delete the section's items, or the section, when nothing carries over); a short retro — what in this process to change, applied to this file in the same wrap; propose the commits (process changes separate from the groom). **Nothing is queued in chat:** each follow-up becomes a row or is done in the wrap. Carry-over state lives in the file every agent reads, never only in a commit message.

**"Timebox"** — the operator can say it at any point. Stop proposing, apply nothing not yet accepted, and go straight to the wrap. An open proposal is listed as not reached, not applied.

**Grooming is a conversation, not a sweep.** Propose one change at a time — or a small group of the same kind — with the evidence, and wait for the operator to accept or change it before moving on. A full-roadmap triage may be prepared as input, but it is never applied as a batch; the operator walks through it.

**Health, not the calendar, decides when.** The backlog may drift from ideal for velocity; `tools/roadmap_rows.sh health` measures how far. Its budget constants live in the script, like `LIMIT`.

- Once per session start, run `tools/roadmap_rows.sh health`. If the budget reads **OVER**, tell the operator once, with the numbers, that grooming comes before feature work. The operator may defer — that is allowed drift, chosen explicitly; say so and move on.
- `health --at <sha>` gives any past state, so trends need no stored data.
- Owed field checks are part of the budget (standing row `swsu`). After a bench session, drop the Pending rows that passed — no archive, git is the record.
- While grooming, run `tools/roadmap_rows.sh` and apply **Row size** above — name each row it flags and let the operator decide; do not trim or extract unprompted.
- The groom commit's subject is `ROADMAP: groom YYYY-MM-DD` (the script finds the last groom by it). Its body carries the `health` output before and after, lineage (splits, merges, drops, declines), the model that made the recommendations, and how many recommendations the operator overrode.
- Each grooming retro asks whether each metric still tracks something worth caring about. A metric no retro uses gets deleted. Never game one — declining a good idea to improve flow is not health.
- Do not repeat the reminder in later turns of the same session, and do not interrupt an in-progress coding task.

### Upstream watches (session start)

Three watches, different cadences. Network fail → skip (any of them); do not interrupt an in-progress coding task.

#### `ft8_lib` pin — every session start

Scoped to the submodule pin ([RFC 0002](rfcs/0002-extract-and-boundaries.md) §6) only. General Micro-FT8 firmware drift is the separate watch below, not this one. The pin has **two** parents and both need watching. GitHub **Sync fork** on `kb2slo/ft8_lib` is off (parent is Wei, not Karlis).

**Karlis** — protocol upstream:

```bash
git ls-remote https://github.com/kgoba/ft8_lib.git refs/heads/master
git -C components/ft8_lib/vendor merge-base --is-ancestor <kgoba-sha> HEAD
```

If the submodule is missing, skip. If Karlis’s SHA **is** an ancestor of the pin, we already have him. If it is **not**, he moved.

**Wei** — the other parent. Do **not** watch `wcheng95/ft8_lib`: that repo is stale at `bb3d94d` (2026-03) and already fully absorbed into the pin. His protocol fixes land in Micro-FT8’s *in-tree* `components/ft8_lib/ft8/` instead, and reach us only by hand — his tree has no submodule, so merging `upstream/main` cannot move our pin:

```bash
git fetch https://github.com/wcheng95/Micro-FT8.git main
for f in $(git ls-tree -r --name-only FETCH_HEAD components/ft8_lib/ft8/ | sed 's|.*/ft8/||'); do
  diff -q <(git show FETCH_HEAD:components/ft8_lib/ft8/$f) \
          <(git -C components/ft8_lib/vendor show HEAD:ft8/$f) >/dev/null 2>&1 || echo "DIFFERS $f"
done
```

Expect `message.c` to differ by our own `stpcpy_compat` removal (`f211146`); anything else is his.

**Why both.** Written 2026-09-08, after this watch missed a live stack overflow (B41): Karlis’s master has not moved since **2025-08-23**, while the one protocol fix worth having in that window came from Wei. Watching only Karlis watches the quiet parent.

Then draft a Backlog row in chat. Do not commit it yet. Do not merge, bump the submodule, or open a kgoba PR in that turn. Done-when: written take (sync now / wait / drop). Sync uses RFC 0002 §6 and is a separate Now. Goldens gate the pin. If a row for that SHA already exists, remind; do not duplicate.

#### Wei's Micro-FT8 (`wcheng95/Micro-FT8`) — at least weekly

Watched, not tracked: no `upstream` remote exists (this repo de-forked 2026-09-15), so the check below fetches Wei's `main` straight from its URL into `FETCH_HEAD` rather than a persistent remote-tracking branch.

**Last checked:** 2026-09-12 — 11 commits on Wei's `main` not in our `main`. `491e757` ("Fix telemetry decode buffer overflow") is already ported by hand as B41; the other 10 are upstream's own CI/test-tooling commits for an "RX-1A" reference-dump/regression harness, nothing that looks portable here. No row drafted.

General drift, not just the `ft8_lib` files this repo shares with Wei's tree (that's the watch above). Feeds [I12](ROADMAP.md) ("Mine public Micro-FT8 forks"), which is Ideas, not a standing workflow — this session-start check is what keeps it from going stale between the occasional full sweep (the kind B39 did once by hand).

```bash
git fetch https://github.com/wcheng95/Micro-FT8.git main
git log --oneline main..FETCH_HEAD
```

If the date above is more than **7 days** old, run it even though it repeats what might be a no-op week to week, and update the date (and the one-line finding) regardless of outcome — that line is the only record of when this last actually ran. Something worth taking → draft an Ideas/Backlog row in chat citing the commit, same discipline as the pin watch: do not merge, do not commit the date bump silently — say what you found first.

#### QMX panadapter project (`SteffenLav/qmx-panadapter`) — at least weekly

**Last checked:** 2026-09-12 — large overlap with several open roadmap items, worth the operator's attention rather than an agent's judgment call. This is a shipping FT8/FT4 station on the **M5Stack Tab5 (ESP32-P4)**: QMX as a USB host (UAC audio + CAT), on-device FT8/FT4 decode and TX, a browser-streamed panadapter UI, ADIF logging with upload to QRZ/eQSL/LoTW/Cloudlog/Wavelog, POTA/SOTA activation logging, and live PSK Reporter/DX-cluster/POTA spotting. That bears directly on [B26](ROADMAP.md) (does a P4 host a QMX at all — this project says yes, in production), [I27](ROADMAP.md) (P4 headless design), [I26](ROADMAP.md) (decode headroom on P4), and [I28](ROADMAP.md)/[B48](ROADMAP.md) (headless web app, QRZ/PSKReporter from the browser). **Resolved 2026-09-16:** the operator accepted this project's production use as B26's feasibility answer rather than running B26's own bench test — see [RFC 0005](rfcs/0005-p4-poe-headless-port.md) §3 for exactly what that does and does not establish (it's evidence about the ESP32-P4 family and the Tab5 board, not about our own U213 module's wiring, which RFC 0005 still bench-confirms before the port depends on it).

Check for anything new via the repo's own version history (`docs/version-history.md` in that repo) rather than a git remote — it is not a fork parent, just a project worth watching for ideas:

```bash
gh repo view SteffenLav/qmx-panadapter --json pushedAt,description
```

If the date above is more than **7 days** old, look at what changed since the last check (the linked version history is newest-last) and update the date and one-line finding regardless of outcome. Insight, not code — do not port or copy from it without the operator's own read of the license and the code.

### Local IDF build / flash

**Local build first. CI is not an iteration loop.** Iterate with local `idf.py build` — roughly 15–40 s incremental — and hand the operator the merged image straight from `build/` (`POST_BUILD` writes `<sha>-microft8-dev.bin` beside `MicroFT8_Merged_Auto.bin`). Do **not** push a branch and wait on a CI artifact merely to produce a test binary: the firmware job alone is ~4–5 minutes, and during bench debugging that latency dominates the session. CI is for **pre-merge verification** — it builds the PR *merge commit* (what actually lands, not the branch tip) and runs the sidekick and host-test jobs that a local ADV build does not. Pushing is also gated by **Chat gates** below: it waits until the operator asks. Do not push in order to get a build.

Clone with submodules first (see **Clone (submodules)** above).

After `source` of the idf5.5 venv and `esp-idf/export.sh`. Radio unplugged from Cardputer USB-C. Leave Launcher **USB** (MSC) first so `/dev/cu.usbmodem*` exists.

**Keep Launcher** (daily iterate once Micro-FT8 has been installed from Launcher once):

```bash
python tools/flash_keep_launcher.py
```

Reads the *device* partition table and writes `build/micro_ft8.bin` into an OTA slot only. Never factory (Launcher), bootloader, or the table. `--dry-run` prints the layout. `--no-build` skips `idf.py build`.

This Cardputer’s Micro-FT8 slot is **`bt4000`** (Launcher PMan name). The helper prefers that partition when it exists; `--partition NAME` overrides.

Do **not** use `idf.py flash` or `idf.py app-flash` on a Launcher unit: both write `0x10000` / factory and replace Launcher.

**No Launcher** (factory-only Micro-FT8 layout):

```bash
idf.py build flash
```

Do not split into separate `build` then `flash` unless the user asks.

**`monitor`:** The Cardputer USB-C is either ESP USB Serial/JTAG (Mac ↔ firmware logs) **or** USB host for the radio (QMX / QDX) — not both. So `idf.py … flash monitor` works with the **radio unplugged** from USB-C. Live logs while the radio is connected need the console UART on **G4 (TX) / G5 (RX)** (USB–TTL adapter), not PORTA/Grove (`G1`/`G2`, GPS/CAT). That G4/G5 path is off when `GNSS_LoRa:ON`.

Loader stubs (`AGENTS.md`, `CLAUDE.md`, `.cursor/rules/`, and similar) exist only so a given tool loads this agreement. They point here. Do not duplicate the plan in those files. `AGENTS.md` states the rule; adding a tool means adding a stub, never moving rules into it.
