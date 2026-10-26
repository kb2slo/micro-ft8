# The roadmap: rows, intake and grooming

How [ROADMAP.md](../ROADMAP.md) is kept.

## Roadmap rows

**Order is priority.** Within Now and Backlog, the top row is next. Grooming re-orders; a new row is placed deliberately, not appended by default.

**Blocked is a section, not a tag,** so Backlog order stays pure priority. A Blocked row names **Blocked on:** (the condition that clears it) and keeps a Done-when; an optional **Collects:** lists work that attaches to it if it is ever done. Order within Blocked does not matter. Same size limit as every row — an overgrown **Collects:** becomes an RFC.

**Declined is the last section.** A rejected row moves there as one line — ID, name, why — so the idea is not re-proposed. Git keeps the full row.

**IDs are opaque.** New rows get a random ID from `tools/roadmap_rows.sh new`, which checks it has never appeared in git history. Existing `B`/`I` IDs stay. Section is stage, position is priority; the ID encodes neither. Lineage — split from, merged into, replaced by — goes in the commit message that made the change, not in the row; `git log -S'<id>' --reverse -- docs/ROADMAP.md` finds where a row came from.

**Row size.** `tools/roadmap_rows.sh` lists rows over its `LIMIT` (the only place the number lives). Flag any such row in chat — in the turn that writes it, before proposing the commit, and at grooming. Route the overflow by kind:

- Design for unbuilt work → an RFC (an existing one if it fits).
- Facts about current code → an architecture doc.
- History ("built…", "extended…", "note, same day…") → delete; git has it.

What stays is ID, name, a line of why, Done-when, and a link. A row stays long only on the maintainer's say-so, marked `` `long-ok` `` so the script counts it. Each grooming retro reads the script's output: many `long-ok` rows mean `LIMIT` is too low; rows under it that still needed trimming mean it is too high.

Update `ROADMAP.md` in the **same turn** as the work:

- New idea → **Ideas** (do not implement until moved up)
- Agreed / sequenced → **Backlog**
- Decided or conditional, waiting on a named condition → **Blocked** (not picked up until it clears)
- Next to build → **Now** (one theme)
- Rejected → one line in **Declined**
- Shipped → drop the row from Now/Backlog. No separate Done archive — git history (and the commit that dropped the row) is the record.

Each row is an ID plus a name.

## Roadmap intake (ideas)

If the maintainer asks to add an idea or “add to the backlog”:
1. Draft the `docs/ROADMAP.md` row in chat. Do not commit yet.
2. New items go in **Ideas**. Use **Backlog** only when they give a Done-when or say it is sequenced.
3. After they approve, commit that row. Push `origin` only when they ask to push. Never `upstream` unless they explicitly ask.

## Backlog grooming

**A groom is a timeboxed session.**

1. **Open:** ask the maintainer for a rough timebox (e.g. 30 min, 1 h) and tune the pace to it. Read `ROADMAP.md`'s **Next groom** section — carried items come first. Run `tools/roadmap_rows.sh health` if you can and state the budget in one line.
2. **Passes, in order, until the timebox runs out:** ① Now — drop what shipped, move out what is not the one theme; ② Backlog — drops, demotions, splits, missing Done-whens, over-size rows, then order; ③ Blocked — does each condition still hold; ④ Ideas — drops, Declined, over-size rows; ⑤ pick Now (one theme).
3. **Wrap:** re-run `health`; rewrite **Next groom** in `ROADMAP.md` with what was not reached (delete the section's items, or the section, when nothing carries over); a short retro — what in this process to change, applied to the doc it changes in the same wrap; propose the commits (process changes separate from the groom). **Nothing is queued in chat:** each follow-up becomes a row or is done in the wrap. Carry-over state lives in the file every agent reads, never only in a commit message.

**A groom may focus on one area** (stated 2026-10-14), such as one RFC's rows. The passes then run over that area's rows and the rows that bear on them; the rest waits for a full groom.

**"Timebox"** — the maintainer can say it at any point. Stop proposing, apply nothing not yet accepted, and go straight to the wrap. An open proposal is listed as not reached, not applied.

**Grooming is a conversation, not a sweep.** Propose one change at a time — or a small group of the same kind — with the evidence, and wait for the maintainer to accept or change it before moving on. A full-roadmap triage may be prepared as input, but it is never applied as a batch; the maintainer walks through it.

**Inline work** (stated 2026-10-14). When a groom turns up a row small enough to finish on the spot, the maintainer may say to build it then. It qualifies when the compiler or a host test proves it (a field check still goes to [TEST_PLAN.md](../TEST_PLAN.md) Pending), it needs no design decision, and it is small. The maintainer saying so is the exception **Push back before tools** ([AGENTS.md](../../AGENTS.md)) asks for. The groom pauses while it is built; the work is committed on its own, never inside the groom commit, and its row is dropped in that commit rather than moved through Backlog. A defect it uncovers that needs a decision is asked in chat before the groom resumes. Anything field-only or still needing design becomes a row instead.

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
