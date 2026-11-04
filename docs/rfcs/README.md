# RFCs (retired)

New design is no longer written as an RFC. "Should we, and how?" is discussed in a GitHub issue, and the pull request that settles it adds an epic: a row in [ROADMAP.md](../ROADMAP.md) and a file under [roadmap/](../roadmap/) ([maintainer/roadmap.md](../maintainer/roadmap.md), **Epics**). No new RFC numbers are taken.

* **This directory is temporary.** The commit that dissolves the last RFC here deletes it, `rejected/` included: each rejected RFC becomes one line in [roadmap/declined.md](../roadmap/declined.md), saying why, with the `git show` that gives its full text. Until then `rejected/` stays whole and frozen. An idea rejected from now on gets its line in `declined.md` directly, and its discussion stays in its issue.
* **The RFCs still here are dissolved into epics, one at a time, at grooming:** what is still planned goes to an epic file, what is true of the code to its architecture doc, the rows link the epic, and the RFC file is deleted with its citations repointed (`git grep -n 'RFC NNNN'`).
* **Until then, an RFC here is that area's plan,** read and kept the way an epic file is: edited in place to say what holds now, with no dated amendments. A slice that ships moves what is now true into the architecture doc and repoints the citations of what it moved; code never cites an RFC section that is gone.
