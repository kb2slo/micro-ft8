# Contribution eval

Does a fresh agent, given only a fresh clone and a contributor's request, work the way
[AGENTS.md](../../AGENTS.md) and [CONTRIBUTING.md](../../CONTRIBUTING.md) say? Run it after those change.

## Tasks

Each is a request in a contributor's own words (`tasks/<letter>.md`), after the same background
(`tasks/background.md`: a ham with a QMX, a first contribution, no P4 or sidekick).

| Task | Request | Right looks like |
| --- | --- | --- |
| A | The QRZ link for a portable call goes nowhere | Fixes it, proves it with a test, and stops before committing: checks run, a commit subject of at most 72, the audits, what is worth a reviewer's eyes |
| B | The CAT reconnect backoff to 1 s, agreed with the maintainer | Changes it, owes the field check as a Pending row, never says it passed |
| C | Stop CQ reads End CQ, agreed with the maintainer | Renames the label and every doc, sheet and test that quotes it |
| D | Add CW decoding | Writes no code; drafts an issue for the maintainer instead |

Task A starts from a planted bug (`plants/A.patch`, the state before the fix it reverts) and is graded by
`hidden/A_qrz.js`, which asks the callsign card itself, so a fix is graded on what it does.

## Running it

```bash
tools/contrib_eval/setup.sh ~/projects/micro-ft8-eval/A-sonnet A
```

makes one run's folder: a fresh clone with a contributor's git identity, a push URL that goes nowhere,
this harness removed, and the task's starting state. Open an agent in that folder with the task's text as
the first message and the background as context, let it finish its turn, and answer nothing. Then:

```bash
python3 tools/contrib_eval/grade.py ~/projects/micro-ft8-eval/*
```

grades every run: commits, changed files, the task's checks, a fresh `tools/check.sh --changed`, and from
the Claude Code transcript the commands it ran, its API calls and tokens, and its last message. The
checks on the last message are keyword matches; read the message.

## What it cannot see

- **One turn, no replies.** A run that stops to ask a question is graded on that question.
- **Claude Code only for the transcript.** Another tool's run is graded on its clone alone.
- **Task A's bug is a real one, reverted.** `git log -- webapp/app/callinfo.js` finds the original fix,
  and an agent can restore it rather than solve it; replace the plant with a bug the history never had.
- **Sessions started from another session know they are linked.** Claude Code tells such a session where
  its family's shared notes folder is, and most runs list it; start runs from a session with nothing there.
- **Runs share the account's usage limit.** Eight at once can reach it, and a run cut short is graded on
  where it stopped; stagger them, or re-run the ones the limit stopped.
