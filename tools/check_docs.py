#!/usr/bin/env python3
"""Fails when a doc points at something that is no longer there.

  python3 tools/check_docs.py [--base REV]

Reads every tracked .md file of ours (not the vendored M5 libraries or ft8_lib), outside fenced
code blocks, and fails on:

  - a relative Markdown link whose file or directory is gone;
  - a repo path in `code` that is gone: a token that starts with one of our top-level directories,
    such as `poe_p4/main/main.cpp:386`. A path in another repo is written as a link to it;
  - a row ID cited after its row left docs/ROADMAP.md, when REV's docs did not already cite it
    (tools/roadmap_rows.sh dangling). Citations REV already had are counted, not failed.

A gitignored path passes (a build output need not exist yet), and so does a path inside a
submodule that is not checked out. #anchors and web links are not checked. RFCs get the link check
only, since an RFC designs files that do not exist yet, and rejected RFCs are frozen and skipped.
"""
import argparse
import os
import posixpath
import re
import subprocess
import sys
import urllib.parse

ROOT = subprocess.run(['git', 'rev-parse', '--show-toplevel'], check=True,
                      capture_output=True, text=True).stdout.strip()

FENCE = re.compile(r'^\s{0,3}(`{3,}|~{3,})')
CODE_SPAN = re.compile(r'(`+)(.+?)\1')
LINK = re.compile(r'!?\[(?:[^\[\]]|\[[^\]]*\])*\]\(\s*<?([^)\s>]+)>?(?:\s+"[^"]*")?\s*\)')
REF_DEF = re.compile(r'^\s{0,3}\[[^\]]+\]:\s*<?(\S+?)>?(?:\s|$)')
SCHEME = re.compile(r'^[A-Za-z][A-Za-z0-9+.-]*:')
EXTERNAL_LINK = re.compile(r'!?\[(?:[^\[\]]|\[[^\]]*\])*\]\(\s*<?[A-Za-z][A-Za-z0-9+.-]*:[^)]*\)')
PATH_TOKEN = re.compile(r'^[A-Za-z0-9_.@+-]+(?:/[A-Za-z0-9_.@+-]*)+$')
LINE_SUFFIX = re.compile(r'^(.*?):\d+(?:-\d+)?$')


def git(*args):
    return subprocess.run(['git', *args], cwd=ROOT, check=True, capture_output=True,
                          text=True).stdout


def ours(path):
    return (path.endswith('.md') and not path.startswith(('components/M5', 'docs/rfcs/rejected/'))
            and '/vendor/' not in path)


class Repo:
    def __init__(self):
        tracked = git('ls-files', '-z').split('\0')
        self.top = {p.split('/', 1)[0] for p in tracked if '/' in p}
        self.docs = sorted(p for p in tracked if ours(p))
        self.submodules = [line.split('\t', 1)[1] for line in git('ls-files', '-s').splitlines()
                           if line.startswith('160000 ')]

    def ignored(self, paths):
        """The subset of paths .gitignore covers."""
        if not paths:
            return set()
        out = subprocess.run(['git', 'check-ignore', '--no-index', '--stdin', '-z'], cwd=ROOT,
                             input='\0'.join(paths) + '\0', capture_output=True, text=True).stdout
        return set(p for p in out.split('\0') if p)

    def missing(self, paths):
        """The subset of repo-relative paths that resolve to nothing."""
        gone = []
        for p in paths:
            if os.path.exists(os.path.join(ROOT, p)):
                continue
            stripped = LINE_SUFFIX.match(p)
            if stripped and os.path.exists(os.path.join(ROOT, stripped.group(1))):
                continue
            sub = next((s for s in self.submodules if p.startswith(s + '/')), None)
            if sub and not os.listdir(os.path.join(ROOT, sub)):
                continue
            gone.append(p)
        # A directory pattern (`build/`) matches only a path written as a directory.
        ignored = self.ignored(gone + [p + '/' for p in gone])
        return [p for p in gone if p not in ignored and p + '/' not in ignored]


def references(text):
    """(line number, kind, target) for each relative link and repo-path candidate in a doc."""
    fence = None
    for n, line in enumerate(text.splitlines(), 1):
        m = FENCE.match(line)
        if m:
            mark = m.group(1)
            if fence is None:
                fence = mark
            elif mark[0] == fence[0] and len(mark) >= len(fence):
                fence = None
            continue
        if fence is not None:
            continue
        ref = REF_DEF.match(line)
        if ref:
            yield n, 'link', ref.group(1)
            continue
        # Code inside the text of a web link names something in that other repo.
        local = EXTERNAL_LINK.sub('', line)
        spans = [m.group(2) for m in CODE_SPAN.finditer(local)]
        for target in LINK.findall(CODE_SPAN.sub('', local)):
            yield n, 'link', target
        for span in spans:
            span = span.replace('\\|', '|').strip()
            if os.path.exists(os.path.join(ROOT, span)):
                continue  # a whole path, spaces and all
            for token in span.replace('|', ' ').split():
                yield n, 'code', token


def resolve_link(doc, target):
    if target.startswith('#') or SCHEME.match(target) or '{{' in target:
        return None
    target = urllib.parse.unquote(target.split('#', 1)[0].split('?', 1)[0])
    if not target:
        return None
    base = '' if target.startswith('/') else posixpath.dirname(doc)
    return posixpath.normpath(posixpath.join(base, target.lstrip('/')))


def resolve_code(repo, token):
    token = token.strip('"\'()[],;').rstrip('.:')
    token = token.split('#', 1)[0]
    stripped = LINE_SUFFIX.match(token)
    if stripped:
        token = stripped.group(1)
    if not PATH_TOKEN.match(token) or token.split('/', 1)[0] not in repo.top:
        return None
    return posixpath.normpath(token)


def dangling(rev=None):
    """{(file, id)} from tools/roadmap_rows.sh dangling, now or as of REV."""
    cmd = [os.path.join(ROOT, 'tools', 'roadmap_rows.sh'), 'dangling'] + (['--at', rev] if rev else [])
    out = subprocess.run(cmd, cwd=ROOT, check=True, capture_output=True, text=True).stdout
    pairs = set()
    for line in out.splitlines():
        path, _, ids = line.partition(':')
        pairs.update((path, i) for i in ids.split())
    return pairs


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--base', help='fail only on dangling row IDs this commit did not cite')
    args = parser.parse_args()

    repo = Repo()
    found = []  # (doc, line, kind, target, path)
    for doc in repo.docs:
        with open(os.path.join(ROOT, doc), encoding='utf-8') as f:
            text = f.read()
        for n, kind, target in references(text):
            if kind == 'code' and doc.startswith('docs/rfcs/'):
                continue
            path = resolve_link(doc, target) if kind == 'link' else resolve_code(repo, target)
            if path is None:
                continue
            if path == '..' or path.startswith('../'):
                print(f'{doc}:{n}: link {target!r} points outside the repo')
                found.append(None)
                continue
            found.append((doc, n, kind, target, path))

    refs = [f for f in found if f]
    gone = set(repo.missing(sorted({f[4] for f in refs})))
    broken = [f for f in refs if f[4] in gone]
    for doc, n, kind, target, path in broken:
        what = f'link {target!r}' if kind == 'link' else f'path `{target}`'
        print(f'{doc}:{n}: {what} does not exist ({path})')
    failures = len(broken) + found.count(None)
    print(f'{len(repo.docs)} docs, {len(refs)} references checked, {failures} broken')
    if failures:
        print('Fix the path, or drop the reference if the thing is gone. A file in another '
              "repo is written as a link to it, not as a bare path that reads like one of ours.")

    now = dangling()
    base = args.base
    if base and subprocess.run(['git', 'rev-parse', '--verify', '-q', base + '^{commit}'],
                               cwd=ROOT, capture_output=True).returncode != 0:
        print(f'--base {base} is not a commit here', file=sys.stderr)
        return 2
    new = sorted(now - dangling(base)) if base else []
    for path, row in new:
        print(f'{path}: cites row {row}, which is no longer in docs/ROADMAP.md')
    print(f'dangling row IDs: {len(now)} citations, {len(new)} new'
          + ('' if base else ' (no --base, so none is counted as new)'))
    if new:
        print('A dropped row is cited by what replaced it, or the citation goes '
              '(docs/README.md, Roadmap rows).')
    return 1 if failures or new else 0


if __name__ == '__main__':
    sys.exit(main())
