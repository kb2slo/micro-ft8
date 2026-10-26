# Maintainer

Procedures that are the maintainer's, or that an agent runs only when working for the maintainer.
The agreement every agent follows is [AGENTS.md](../../AGENTS.md).

- [roadmap.md](roadmap.md) — how `ROADMAP.md` is kept: rows, intake, grooming, health.
- [upstream.md](upstream.md) — the upstream watches.
- [../RELEASE_PROCESS.md](../RELEASE_PROCESS.md) — how a build becomes a download.

## Session start

When the person you work for is the maintainer, once per session: `tools/roadmap_rows.sh health`
([roadmap.md](roadmap.md), **Health, not the calendar, decides when**), and the upstream watches that are due
([upstream.md](upstream.md)). A contributor's session runs neither.

## Running agents

**Slices handed to Sonnet.** A fully specified slice goes to a fresh Sonnet session from a seed prompt; Sonnet builds it, reviews it itself, commits locally, and the maintainer benches it. An Opus review is for firmware, the Port A protocol, and anything that keys or retunes the radio, and only there: a fresh, short session with the diff and the build notes, reporting findings for Sonnet to fix, never fixing in a long session. An app-only slice gets no Opus review unless the bench fails and Sonnet cannot fix it. The seed prompt's self-review: cite `file:line` for every claim about firmware or the main board; say what the code does after every confirmation or message; `git grep` the citations of any graduated RFC section; `tools/check.sh --changed` passes. One slice per session, no push, and no bench check recorded that the maintainer has not reported step by step.

**Assume another agent may be in this worktree.** The maintainer sometimes runs a second Claude Code session
on the same checkout. One worktree means one index, so a concurrent `git add -A` can sweep up files another
session wrote seconds earlier and commit them under an unrelated message. Re-check `git log -1` and `git status`
immediately before committing rather than trusting state read earlier in the turn, and stage explicit paths
rather than `-A` unless staging everything is genuinely the intent. If files have already been swept into
someone else's pushed commit, say so and cross-reference that SHA; do not rewrite pushed history to reclaim
them.
