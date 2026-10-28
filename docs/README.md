# docs/

Human- and tool-readable project memory. Prefer this tree over editor-specific rule files, vendor instruction stubs, or chat transcripts.

| File | Role |
|---|---|
| [../AGENTS.md](../AGENTS.md) | The working agreement: how any agent works in this repo, whoever it works for. |
| [../CONTRIBUTING.md](../CONTRIBUTING.md) | How a change gets from a branch onto `main`: setup, checks, the pull request, field checks. |
| [ROADMAP.md](ROADMAP.md) | Plan: Next groom / Now / Backlog / Blocked / Ideas / Declined. Chat is intake; this file is truth. Shipped work is dropped, not archived — git history is the record. |
| [../README.md](../README.md) | Contributor landing: what it is, status, hardware, layout, build and test, contributing, acknowledgements. Operators go to the site. What it holds and when to audit it: [AGENTS.md](../AGENTS.md), **README and site stay current**. |
| [STYLE.md](STYLE.md) | Coding standard for our C/C++ (not vendored `M5*` / `ft8_lib`). |
| [TEST_PLAN.md](TEST_PLAN.md) | Ledger: what automation checks, what it cannot see, and the field checks still owed (**Pending**), with how one is asked for and closed. |
| [RELEASE_PROCESS.md](RELEASE_PROCESS.md) | How a build becomes a download: the rolling `continuous` tag, versioned `v*` releases, what a version tag does **not** set, and what a rename/de-fork would break. |
| `architecture/` | How it works now, and why, one doc per area. Start at [system.md](architecture/system.md); it links the rest. Doc type: **Doc types** below. |
| [FT8 Free-Text Reference Extension.md](FT8%20Free-Text%20Reference%20Extension.md) | Free-text / SOTA-style payload notes. |
| `rfcs/` | Design for work not yet built. Lifecycle and graduation: [rfcs/README.md](rfcs/README.md). |
| `maintainer/` | What only the maintainer does: how the roadmap is kept and groomed, the upstream watches, how agents are run here. Start at [maintainer/README.md](maintainer/README.md). |

Git history is the changelog. Do not keep a narrative log in these files.

## Doc types

Each document answers one question. Put content where its question is answered.

| Question | Where | Kind |
|---|---|---|
| What next? | `ROADMAP.md` | Plan rows |
| Should we, and how? (not built yet) | `docs/rfcs/NNNN-*.md` | RFC |
| How does this work now, and why? | `docs/architecture/<area>.md` | Architecture doc |
| What is this project, and how do I start contributing? | `README.md` | Contributor landing |
| How do I build, flash, or use this part? | That directory's `README.md` | How-to |
| What must two codebases agree on? | The code | One source of truth, plus a test holding the other side to it |
| What can an operator do with it? | `webapp/` → `micro-ft8.kalikstein.com` | Public site |
| How does an operator set up a station? | `webapp/getting-started.md` → the site's Getting started page and the printed sheet in the box | One source for both; `webapp/tools/getting_started.mjs` builds it and fails when a label it quotes leaves the UI |

**One fact, one home.** A fact lives in the doc of the component that implements it. Other docs link to it and restate at most the one sentence needed to follow the link — the trust argument for the firmware download is in `system.md`, its mechanism in `sidekick.md`.

**No prose specs.** A contract that crosses a language or repo boundary (C ↔ webapp JS, our encoder ↔ WSJT-X) gets one side as the source of truth in code and a test that holds the other side to it — not a document restating it.

**The site describes what ships.** Nothing unbuilt, unmeasured, or upstream-only goes on it.

**Docs point at what exists.** `tools/check_docs.py` (part of `tools/check.sh`) fails on a relative link or a repo path in `code` that no longer resolves, and on a row ID that newly dangles. A file in another repo is written as a link to it.
