#!/usr/bin/env python3
"""Fails when a commit message is not in the shape AGENTS.md (Chat gates, 3) asks for.

  python3 tools/check_commits.py [RANGE]     default: origin/main..HEAD
  python3 tools/check_commits.py RANGE --pr-title TITLE --pr-number N

A subject of at most LIMIT characters, then a blank line before any body. The subject says what
changed; the why goes in the body. `git log --oneline` and every tool that lists commits show only
the subject, and an agent writing its first commit here copies the ones it sees. Merge commits, and
the subjects `git revert` writes, are left alone.

With a pull request's title and number, RANGE being its commits, it also checks the subject a squash
merge would write: the lone commit's subject, or the title when there are more, then ` (#N)` (this
repo's squash setting, COMMIT_OR_PR_TITLE). A rebase or a merge commit adds no suffix, so this is
stricter than they need.

LIMIT is the only place the number lives.
"""
import argparse
import subprocess
import sys

LIMIT = 72


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('range', nargs='?', default='origin/main..HEAD')
    parser.add_argument('--pr-title', help="the pull request's title")
    parser.add_argument('--pr-number', type=int, help="the pull request's number")
    args = parser.parse_args()
    rng = args.range
    log = subprocess.run(['git', 'log', '--no-merges', '--format=%h%x1f%B%x1e', rng],
                         capture_output=True, text=True)
    if log.returncode != 0:
        print(f'check_commits: {log.stderr.strip()}', file=sys.stderr)
        return 2

    commits = [c.lstrip('\n') for c in log.stdout.split('\x1e') if c.strip()]
    failures = title_failure = 0
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

    if args.pr_title is not None and args.pr_number is not None:
        lone = commits[0].partition('\x1f')[2].split('\n')[0] if len(commits) == 1 else None
        squash = f'{lone if lone is not None else args.pr_title} (#{args.pr_number})'
        if len(squash) > LIMIT:
            title_failure = 1
            source = 'its one commit\'s subject' if lone is not None else 'its title'
            print(f'pull request #{args.pr_number}: a squash merge would write a {len(squash)}-character subject, '
                  f'from {source} and " (#{args.pr_number})"; the limit is {LIMIT}')

    print(f'{len(commits)} commit(s) in {rng}, {failures + title_failure} to fix')
    if failures:
        print('Say what changed in the subject and put the why in the body (AGENTS.md, Chat gates, 3). '
              'Reword the last commit with `git commit --amend`; an earlier one, with an interactive rebase '
              'before it is pushed.')
    if title_failure:
        print(f'Shorten the title (`gh pr edit {args.pr_number} --title ...`), or the lone commit\'s subject, '
              'until it fits with the suffix.')
    return 1 if failures or title_failure else 0


if __name__ == '__main__':
    sys.exit(main())
