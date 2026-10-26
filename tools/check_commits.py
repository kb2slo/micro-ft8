#!/usr/bin/env python3
"""Fails when a commit message is not in the shape AGENTS.md (Chat gates, 3) asks for.

  python3 tools/check_commits.py [RANGE]     default: origin/main..HEAD

A subject of at most LIMIT characters, then a blank line before any body. The subject says what
changed; the why goes in the body. `git log --oneline` and every tool that lists commits show only
the subject, and an agent writing its first commit here copies the ones it sees. Merge commits, and
the subjects `git revert` writes, are left alone.

LIMIT is the only place the number lives.
"""
import subprocess
import sys

LIMIT = 72


def main():
    rng = sys.argv[1] if len(sys.argv) > 1 else 'origin/main..HEAD'
    log = subprocess.run(['git', 'log', '--no-merges', '--format=%h%x1f%B%x1e', rng],
                         capture_output=True, text=True)
    if log.returncode != 0:
        print(f'check_commits: {log.stderr.strip()}', file=sys.stderr)
        return 2

    commits = [c.lstrip('\n') for c in log.stdout.split('\x1e') if c.strip()]
    failures = 0
    for commit in commits:
        sha, _, message = commit.partition('\x1f')
        lines = message.rstrip('\n').split('\n')
        subject = lines[0]
        if subject.startswith('Revert "'):
            continue
        problems = []
        if len(subject) > LIMIT:
            problems.append(f'subject is {len(subject)} characters, the limit is {LIMIT}')
        if len(lines) > 1 and lines[1].strip():
            problems.append('no blank line after the subject, so git reads the next line as more subject')
        if problems:
            failures += 1
            shown = subject if len(subject) <= 60 else subject[:57] + '...'
            print(f'{sha} {shown}: ' + '; '.join(problems))

    print(f'{len(commits)} commit(s) in {rng}, {failures} to fix')
    if failures:
        print('Say what changed in the subject and put the why in the body (AGENTS.md, Chat gates, 3). '
              'Reword the last commit with `git commit --amend`; an earlier one, with an interactive rebase '
              'before it is pushed.')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
