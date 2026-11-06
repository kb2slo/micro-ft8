# The roadmap: rows, epics, intake and grooming

How the roadmap is kept: the index [ROADMAP.md](../ROADMAP.md), the epic files beside it in [roadmap/](../roadmap/), and [roadmap/declined.md](../roadmap/declined.md).

## The shape

* **`ROADMAP.md` is the index:** Next groom, Epics, Now, Backlog, Blocked, Ideas. It holds the order, and every session reads it, so it stays small.
* **An epic file, `roadmap/<name>.md`, holds one epic:** a body of work too big for one row. Only a session touching that epic's rows reads it.
* **`roadmap/declined.md`** holds rejected ideas, one line each, so they are not proposed again. Read it when an idea comes up, not at session start.

**Not Jira.** The roadmap is plain markdown that a person can edit by hand. These limits are the point; a change that loosens one is a process change, proposed on its own:

* **Status is the section a row sits in.** No status field.
* **A row has ID, Item, and Why with a Done-when.** No priority, estimate, assignee, label or sprint field; order is priority.
* **One level of nesting:** epic, then rows. No sub-epics; a row that needs children is an epic.
* **Few epics:** a handful at a time.
* **No tooling beyond `tools/roadmap_rows.sh`.** If keeping the roadmap needs more, the process is too heavy.
* **Now reads in a minute,** and a fresh agent gets the plan from the index plus at most the epic it touches.

## Roadmap rows

**Order is priority.** Within Now and Backlog, the top row is next. Grooming re-orders; a new row is placed deliberately, not appended by default.

**Blocked is a section, not a tag,** so Backlog order stays pure priority. A Blocked row names **Blocked on:** (the condition that clears it) and keeps a Done-when; an optional **Collects:** lists work that attaches to it if it is ever done. Order within Blocked does not matter. Same size limit as every row; an overgrown **Collects:** becomes an epic.

**Declined lives in [roadmap/declined.md](../roadmap/declined.md).** A rejected row or epic moves there as one line — ID, name, why. The why is the reason that also rules out the idea's close variants, so a reworded proposal still meets it. Git keeps the full row; a line for something long, such as a rejected RFC, ends with the `git show <sha>:<path>` that prints it. An outside idea declined in its issue needs no line: the closed issue says why.

**Every row has an anchor named by its ID:** `<a id="<id>"></a>` at the start of its Item cell in the index or `declined.md`, and at the start of its entry in its epic file. A row in an epic links to that entry (`Epic [knpp](roadmap/hl2-support.md#abkw)`), so a click from the index lands on its detail. Mentions of a row anywhere link to it ([AGENTS.md](../../AGENTS.md), **Link every roadmap row you name**); `tools/check_docs.py` fails a roadmap link whose anchor is gone, and `tools/roadmap_rows.sh` measures a row without its anchors and link targets.

**IDs are opaque.** New rows and epics get a random ID from `tools/roadmap_rows.sh new`, which checks it has never appeared in git history. Existing `B`/`I` IDs stay. Section is stage, position is priority; the ID encodes neither. Lineage — split from, merged into, replaced by — goes in the commit message that made the change, not in the row; `git log -S'<id>' --reverse -- docs/ROADMAP.md` finds where a row came from.

**Row size.** `tools/roadmap_rows.sh` lists rows over its `LIMIT` (the only place the number lives). Flag any such row in chat — in the turn that writes it, before proposing the commit, and at grooming. Route the overflow by kind:

- Detail of planned work → its epic's file (a new epic if none fits).
- Facts about current code → an architecture doc.
- History ("built…", "extended…", "note, same day…") → delete; git has it.

What stays is ID, name, a line of why, Done-when, and a link. A row stays long only on the maintainer's say-so, marked `` `long-ok` `` so the script counts it. Each grooming retro reads the script's output: many `long-ok` rows mean `LIMIT` is too low; rows under it that still needed trimming mean it is too high.

Update the roadmap in the **same turn** as the work:

- New idea → **Ideas** (do not implement until moved up)
- Agreed / sequenced → **Backlog**
- Decided or conditional, waiting on a named condition → **Blocked** (not picked up until it clears)
- Next to build → **Now**
- Rejected → one line in `roadmap/declined.md`
- Shipped → drop the row. No separate Done archive: git history (and the commit that dropped the row) is the record.

## Epics

**An epic is planned work too big for one row:** several rows toward one goal, with design that is decided but not built. It replaces the RFC; there is no separate design document.

**What qualifies.** An epic needs both:

* **several rows toward one goal,** where finishing them is the point, not only a shared topic; and
* **knowledge that must outlive any one row:** decisions, facts, open questions or design the rows depend on, too much to repeat in each.

Missing either, it is rows. **Not an epic:** a single large row, however large (its detail goes in the row and an architecture doc); a theme of unrelated rows ("cleanup", "docs"), which is grooming's; a standing row (`swsu`); a category kept open for rows to come, since an epic ends when its goal is met. **A new row joins an existing epic** when it serves that epic's goal; serving two, it belongs to the one whose goal it finishes and names the other as **After:**. **Dissolving an RFC** applies the same test: one whose remaining plan is a row or two becomes rows and an architecture doc, not an epic. Every intake runs this test ([AGENTS.md](../../AGENTS.md), **Before proposing a new row, epic or idea**).

**In the index,** an epic is one row in **Epics**: its ID, its name and a link to its file. The file is named for what the epic is, in plain words (`hl2-support.md`), not for its ID; its title carries the ID. Its child rows sit in Now, Backlog or Blocked like any row, in priority order, each starting its Why with the epic's link. A child row in the index is one line of why and a Done-when; its detail is in the epic file.

**The epic file holds what is planned, and only while it is planned:**

* **Goal** and **constraints.**
* **Decided:** what holds now. No dates, no decision log, no "was X": a decision that changes is edited in place, and git keeps the old one.
* **Design** of what is not built yet, and the facts it rests on, each with its source.
* **Children:** each child row's detail, the order they depend on each other in, and its **Lands in:** line; then **Later**, the slices not yet rows, one line each.
* **Open:** questions still to settle, each named for the child that needs the answer.
* **Why not:** short notes on the alternatives turned down, so they are not proposed again.

**An epic grows by grooming.** It starts high-level: a goal, its constraints, one child row, and the rest as a short **Later** list. Each groom that reaches it adds detail as late as it can and as early as the next work needs: design is filled in ahead of the child that builds it, a Later slice becomes a row with a Done-when when it is next, and Open questions are settled or named for the child that needs them. Leave out anything that is not useful to the next child's work. A file past about 200 lines is doing an architecture doc's job: move what is true now there.

**Lands in.** Each child names, on one **Lands in:** line, the architecture docs its commit moves truth into (and any new one it creates), plus the README, site and TEST_PLAN audits it triggers. A section no child obviously builds (facts, why-not notes, parity, open questions) gets its own **Lands in:** line naming the child and the doc; design sections go with the child that builds them and carry none. The destination is decided when the child is written, while the context is fresh, and grooming corrects it as the design moves. **A child that measures rather than builds lands too:** its data in a corpus README, and the facts it establishes in the architecture doc they describe, beside any rule they justify or question; a code change the finding calls for is its own row.

**When a child ships,** its commit drops the row and moves what is now true about the code into the docs its **Lands in:** line names ([AGENTS.md](../../AGENTS.md), **Architecture docs match the code too**), deleting it from the epic file. So the epic file describes only the plan, and architecture docs only what is built.

**When the last child ships or is dropped,** that commit deletes the file and the Epics row, after moving anything still true into an architecture doc. An epic abandoned before then is deleted with one line in `declined.md`.

**One epic per row.** A row that serves two epics belongs to the one whose goal it finishes, and names the other as **After:**.

**Undecided design is not an epic yet.** "Should we, and how?" is discussed in a GitHub issue (**Issues**, below). The pull request that settles it adds the epic file, the Epics row and the first child rows together.

## Issues

GitHub issues are where the roadmap's inputs arrive and are talked through; the roadmap and the epic files are where the outcome lands.

* **Uses:** a contributor's idea, a bug or field report from an operator, and design discussion for an epic not yet decided.
* **Never the only home of a decision.** When an issue settles something, the pull request that acts on it puts the outcome in the index, an epic file or an architecture doc, and cites `#N` for the history. Closing the issue then loses nothing.
* **Plain issues only:** the templates in `.github/ISSUE_TEMPLATE/` and a few labels. No Projects boards, milestones, assignees or sub-issues; epics are files here, not issues.
* **Agents read issues freely,** and comment on, open or close one only when the person they work for says to in that turn ([AGENTS.md](../../AGENTS.md), **Commit / push only when asked**): an issue comment is public.

## Roadmap intake (ideas)

If the maintainer asks to add an idea or “add to the backlog”:
1. Draft it in chat with its epic test's outcome ([AGENTS.md](../../AGENTS.md), **Before proposing a new row, epic or idea**): a row in an epic is drafted with its child entry and **Lands in:** line, together; a new epic is proposed as one. Do not commit yet.
2. New items go in **Ideas**. Use **Backlog** only when they give a Done-when or say it is sequenced.
3. After they approve, commit that row. Push `origin` only when they ask to push. Never `upstream` unless they explicitly ask.
4. **Fast path for Ideas.** A change that only adds or edits rows in **Ideas**, and touches nothing else in any file, goes straight to `main`: once the maintainer approves the row, commit it on a branch from `main`, run `tools/check.sh --changed`, and push it to `main` with no pull request. Approving the row is the push instruction, since an idea is unscheduled and one revert undoes it. Everything else on the roadmap (a Backlog or Now row, an order change, an epic file, `declined.md`) keeps its own branch and pull request, pushed only when they ask.

## Backlog grooming

**A groom is a timeboxed session.**

1. **Open:** ask the maintainer for a rough timebox (e.g. 30 min, 1 h) and tune the pace to it. Read `ROADMAP.md`'s **Next groom** section — carried items come first. Run `tools/roadmap_rows.sh health` if you can and state the budget in one line.
2. **Passes, in order, until the timebox runs out:** ① Now: drop what shipped, move out what is not the epic or theme in focus; ② Epics: for each, does the next child have the design it needs, which Later slices are now next (they become rows), which Open questions can be settled or need naming for a child, is the file still only plan, does each still have live children, and does each still meet **What qualifies**; ③ Backlog: drops, demotions, splits, missing Done-whens, over-size rows, then order; ④ Blocked: does each condition still hold; ⑤ Ideas: drops, declines, over-size rows; ⑥ pick Now.
3. **Wrap:** re-run `health`; rewrite **Next groom** in `ROADMAP.md` with what was not reached (delete the section's items, or the section, when nothing carries over); a short retro — what in this process to change, applied to the doc it changes in the same wrap; propose the commits (process changes separate from the groom). **Nothing is queued in chat:** each follow-up becomes a row or is done in the wrap. Carry-over state lives in the file every agent reads, never only in a commit message.

**A groom may focus on one epic.** The passes then run over its file, its rows and the rows that bear on them; the rest waits for a full groom.

**"Timebox"** — the maintainer can say it at any point. Stop proposing, apply nothing not yet accepted, and go straight to the wrap. An open proposal is listed as not reached, not applied.

**Grooming is a conversation, not a sweep.** Propose one change at a time — or a small group of the same kind — with the evidence, and wait for the maintainer to accept or change it before moving on. A full-roadmap triage may be prepared as input, but it is never applied as a batch; the maintainer walks through it.

**Inline work.** When a groom turns up a row small enough to finish on the spot, the maintainer may say to build it then. It qualifies when the compiler or a host test proves it (a field check still goes to [TEST_PLAN.md](../TEST_PLAN.md) Pending), it needs no design decision, and it is small. The maintainer saying so is the exception **Push back before tools** ([AGENTS.md](../../AGENTS.md)) asks for. The groom pauses while it is built; the work is committed on its own, never inside the groom commit, and its row is dropped in that commit rather than moved through Backlog. A defect it uncovers that needs a decision is asked in chat before the groom resumes. Anything field-only or still needing design becomes a row instead.

**Health, not the calendar, decides when.** The backlog may drift from ideal for velocity; `tools/roadmap_rows.sh health` measures how far. Its budget constants live in the script, like `LIMIT`.

- Once per maintainer session ([README.md](README.md), **Session start**), run `tools/roadmap_rows.sh health`. If the budget reads **OVER**, tell the maintainer once, with the numbers, that grooming comes before feature work. The maintainer may defer — that is allowed drift, chosen explicitly; say so and move on.
- `health --at <sha>` gives any past state, so trends need no stored data.
- Owed field checks are part of the budget (standing row `swsu`). After a bench session, drop the Pending rows that passed — no archive, git is the record.
- While grooming, run `tools/roadmap_rows.sh` and apply **Row size** above — name each row it flags and let the maintainer decide; do not trim or extract unprompted.
- The groom commit's subject is `ROADMAP: groom YYYY-MM-DD` (the script finds the last groom by it). Its body carries the `health` output before and after, lineage (splits, merges, drops, declines), the model that made the recommendations, and how many recommendations the maintainer overrode.
- Each grooming retro asks whether each metric still tracks something worth caring about. A metric no retro uses gets deleted. Never game one — declining a good idea to improve flow is not health.
- Do not repeat the reminder in later turns of the same session, and do not interrupt an in-progress coding task.

**Grooming spans tiers.** Running `tools/roadmap_rows.sh`, deleting history from rows, and applying decisions the maintainer already made are tier 1 ([AGENTS.md](../../AGENTS.md), **Tier the model to the task**). Triage recommendations are tier 2 by default. The risk is not a wrong recommendation — the maintainer overrides it — but a finding never surfaced: a defect hiding in a row, a row contradicting a rule here, a stale reference. Weigh the model observations below when choosing, and ask for `file:line` or commit evidence behind every claim; verification, more than model size, is what surfaced findings.

**Model observations (triage).** One blind triage of the same roadmap per model, 2026-09-24 — a single sample each, so treat as leads, not verdicts. Add a line when a groom shows something new; delete a line a later groom contradicts.

- **Claude Opus** — strongest on judgment: defect severity (a TX-cancel bug as a safety issue), whether a row is still real work (obsolete, field-check-only, superseded-but-unbuilt). Missed stale facts that only a code check finds (a menu count, a dropped ID cited by an RFC). Flagged a row as breaking the pre-alpha rule from its text alone; the code had no such compat path (Composer made the same call).
- **Cursor Composer (Auto)** — strongest on verification: cited `file:line` for nearly every claim, swept dangling IDs broadly, caught stale counts and a parked defect conflicting with **Fix defects in the slice**. Weaker on severity ranking; dropped a row whose remaining work was unbuilt.
- **Claude Opus 5.5** (focused groom, 2026-10-24) — checking rows against the code found three stale facts (a row's "the QSOs tab already loads today's log", a doc's worked/new flags, an RFC's Port A speed). The one figure it quoted from an RFC without that check was itself stale; it corrected it a turn later.
- **Claude Sonnet** — strongest on mechanical compliance (every missing Done-when, oversize row, dangling reference; the only one to catch an RFC citing a dropped row). Missed most judgment findings, and contradicted itself once against the pre-alpha rule.
