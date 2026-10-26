# Contributing

Micro-FT8 is a headless FT8 station: a [PoE-P4](docs/architecture/main_board.md) main board drives a QMX,
and an AtomS3 Lite sidekick serves the web app you operate it from. How the pieces fit:
[docs/architecture/system.md](docs/architecture/system.md).

It is **pre-alpha**: anything can change or break between commits, and nothing carries an old state
forward ([AGENTS.md](AGENTS.md), **Pre-alpha**). The maintainer is Jeff, KB2SLO.

## Before you start

- **A bug fix, or a change a host test proves,** can go straight to a pull request.
- **A feature** starts as an issue the maintainer has agreed to. `docs/ROADMAP.md` is the plan; an idea
  that is not on it is an issue first, not a pull request.

## Setup

While the repo is private, cloning needs access from the maintainer. `ft8_lib` is a submodule, which
GitHub's default clone leaves empty:

```bash
git clone --recurse-submodules git@github.com:kb2slo/micro-ft8.git
```

An existing tree: `git submodule update --init`. The checks need Python 3, Node, `make`, a C/C++
compiler and CMake. The firmware needs ESP-IDF v5.5.5: source its `export.sh`, then
[poe_p4/README.md](poe_p4/README.md) and [sidekick/README.md](sidekick/README.md) say how to build and
flash each board.

## Checks

```bash
tools/check.sh --changed
```

runs every check your change can affect; `tools/check.sh` runs all of them, and `--list` says which
would run and why. CI runs the same script. Without `idf.py` on the PATH the two firmware builds report
**SKIP**, which is not a pass. What each check covers, and what no check can see:
[docs/TEST_PLAN.md](docs/TEST_PLAN.md).

## Commits and pull requests

- Work on a branch. One change per pull request; a feature and an unrelated fix are two.
- A commit's subject is one sentence of at most 72 characters saying what changed, then a blank line
  and the why in the body. CI checks the shape.
- A squash merge makes the pull request's title the commit's subject, or its one commit's subject
  when it has just one, and adds ` (#N)`. Keep that within 72 too; CI checks it.
- The pull request template asks what the change is, how it was verified, which checks ran, the field
  checks, the doc audits, and the one to three places most worth a reviewer's eyes. The maintainer
  reviews and merges.
- C and C++ follow [docs/STYLE.md](docs/STYLE.md). Where a fact or a doc goes:
  [docs/README.md](docs/README.md).

## Field checks

Some things only the hardware can show: USB audio and CAT, TX timing, the sidekick link, WiFi. A change
that touches one adds a numbered check to **Pending** in [docs/TEST_PLAN.md](docs/TEST_PLAN.md). If you
have a PoE-P4, a QMX and a sidekick, run it and put each step's result in the pull request, with the
build it ran on. Without the hardware, start with work a host test proves: the logic in `components/`,
the web app's tested functions, the docs.

## Working with an AI agent

Most coding agents read [AGENTS.md](AGENTS.md) on their own; Claude Code reads it through `CLAUDE.md`.
It treats you as the person it works for: it designs in chat before building, asks before it commits or
pushes, says which checks it ran, and never reports a field check as passed — that result is yours.

## Licence

MIT ([LICENSE](LICENSE)). A contribution is under the same licence.
