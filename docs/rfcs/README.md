# RFCs

Design for work not yet built ([../README.md](../README.md), **Doc types**).

## RFC lifecycle

An RFC is warranted when a row's design will not fit in the row ([maintainer/roadmap.md](../maintainer/roadmap.md), **Row size**), when work spans several rows or boards, or when real alternatives are still undecided.

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
   * **Status:** Graduated YYYY-MM-DD → [<doc>](../../<doc>)
   * Full text at graduation: `git show <sha>:docs/rfcs/NNNN-<slug>.md`
   ```

**Rejected** RFCs move to `docs/rfcs/rejected/` and stay whole and frozen — their reasoning is what stops the idea being re-proposed; out of `rfcs/`, they no longer sit beside live design work.
