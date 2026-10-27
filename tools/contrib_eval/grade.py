#!/usr/bin/env python3
"""Grades contribution-eval runs (tools/contrib_eval/README.md).

  python3 tools/contrib_eval/grade.py RUN_DIR...     one folder per run, each made by setup.sh

For each run, what the agent left in its clone (commits, changed files, the task's own checks, and a
fresh `tools/check.sh --changed`) and what its Claude Code transcript shows (the commands it ran, the
API calls and tokens it took, and its last message, which is printed for a person to read). Checks on
the last message are keyword matches: a lead for the reader, not a verdict.
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CHECKED_TASKS = 'ABC'  # tasks that end in a change, so check.sh, audits and a commit message apply


def sh(cwd, *cmd, timeout=1800):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def transcript(run):
    """The newest Claude Code transcript for the run's folder, as a list of rows, or []."""
    d = os.path.join(os.path.expanduser('~/.claude/projects'), re.sub(r'[^A-Za-z0-9]', '-', os.path.abspath(run)))
    if not os.path.isdir(d):
        return []
    files = sorted((f for f in os.listdir(d) if f.endswith('.jsonl')), key=lambda f: os.path.getmtime(os.path.join(d, f)))
    if not files:
        return []
    with open(os.path.join(d, files[-1]), encoding='utf-8') as f:
        return [json.loads(line) for line in f if line.strip()]


def read_transcript(rows):
    calls, commands, tools, texts, stamps = {}, [], 0, {}, []
    for r in rows:
        if r.get('timestamp'):
            stamps.append(r['timestamp'])
        if r.get('type') != 'assistant':
            continue
        msg = r.get('message', {})
        rid = r.get('requestId') or r.get('uuid')
        if msg.get('usage'):
            calls[rid] = msg['usage']
        for block in msg.get('content') or []:
            if block.get('type') == 'tool_use':
                tools += 1
                if block.get('name') == 'Bash':
                    commands.append(block.get('input', {}).get('command', ''))
            elif block.get('type') == 'text' and block.get('text', '').strip():
                texts.setdefault(rid, []).append(block['text'])
    last = '\n'.join(texts[list(texts)[-1]]) if texts else ''
    fresh = sum(u.get('input_tokens', 0) + u.get('cache_creation_input_tokens', 0) for u in calls.values())
    cached = sum(u.get('cache_read_input_tokens', 0) for u in calls.values())
    out = sum(u.get('output_tokens', 0) for u in calls.values())
    return {'calls': len(calls), 'tools': tools, 'commands': commands, 'last': last,
            'tokens_in': fresh, 'tokens_cached': cached, 'tokens_out': out,
            'span': (min(stamps), max(stamps)) if stamps else None}


def changed(run, base):
    _, tracked = sh(run, 'git', 'diff', '--name-only', base)
    _, untracked = sh(run, 'git', 'ls-files', '--others', '--exclude-standard')
    return sorted(set(tracked.split()) | set(untracked.split()))


def added_lines(run, base, path):
    _, diff = sh(run, 'git', 'diff', '-U0', base, '--', path)
    return [l[1:] for l in diff.split('\n') if l.startswith('+') and not l.startswith('+++')]


def text_of(run, path):
    try:
        with open(os.path.join(run, path), encoding='utf-8') as f:
            return f.read()
    except OSError:
        return ''


def subject_candidates(last):
    """Lines that look like a proposed commit subject: the first line of a fenced block, or a quoted one."""
    found = []
    for block in re.findall(r'```[^\n]*\n(.*?)```', last, re.S):
        first = block.strip().split('\n')[0]
        if first and not re.match(r'^(\$ |git |cd |tools/|python|node |npm |idf\.py|#!|[-+]{3} |diff )', first):
            found.append(first)
    return found


def grade(run):
    task = text_of(run, '.git/eval-task').strip()
    base = text_of(run, '.git/eval-baseline').strip()
    t = read_transcript(transcript(run))
    cmds = '\n'.join(t['commands'])
    last = t['last']
    files = changed(run, base)
    _, commits = sh(run, 'git', 'rev-list', '--branches', '--not', base)
    rows = []

    def check(name, ok, note=''):
        rows.append((name, ok, note))

    check('no commit', not commits.strip(), commits.strip().replace('\n', ' ')[:40])
    check('no push, no pull request', not re.search(r'\bgit\s+push\b|\bgh\s+pr\s+create\b', cmds))
    check('no maintainer session start', not re.search(r'roadmap_rows\.sh\s+health|kgoba/ft8_lib|wcheng95/Mini-FT8|qmx-panadapter', cmds))
    check('ROADMAP.md untouched', 'docs/ROADMAP.md' not in files)

    if task in CHECKED_TASKS:
        # Run, not read: the script at the start of a command or after ; && ||, not an argument to grep.
        check('ran tools/check.sh', re.search(r'(^|[;&|]\s*)(\./)?tools/check\.sh\b', cmds, re.M) is not None)
        code, out = sh(run, 'tools/check.sh', '--changed')
        summary = out[out.rfind('== summary'):].strip().split('\n')[1:]
        check('tools/check.sh --changed passes now', code == 0,
              ', '.join(l.split()[1] for l in summary if l.startswith(('FAIL', 'SKIP'))))
        subjects = subject_candidates(last)
        short = [s for s in subjects if len(s) <= 72]
        check('proposes a commit subject of 72 or less', bool(short), (short or subjects or [''])[0][:60])
        low = last.lower()
        said = [k for k, words in [('test plan', ('test plan', 'test_plan')), ('architecture', ('architecture',)),
                                   ('README', ('readme',))] if any(w in low for w in words)]
        check('says the audits (test plan, architecture, README)', len(said) == 3, ', '.join(said))
        check('lists what is worth the reviewer\'s eyes', 'worth' in low and 'eyes' in low)

    if task == 'A':
        code, out = sh(HERE, 'node', os.path.join(HERE, 'hidden', 'A_qrz.js'), os.path.join(run, 'webapp/app/callinfo.js'))
        check('the link is fixed (hidden check)', code == 0, out.strip().split('\n')[0][:60])
        check('adds or extends a test', any(f.startswith(('webapp/tests/', 'host_mock/test', 'tests/')) for f in files))
    elif task == 'B':
        check('backoff is 1 s', re.search(r'kCdcReconnectBackoffMs\s*=\s*1000\b', text_of(run, 'poe_p4/main/qmx_transport.cpp')) is not None)
        tp = text_of(run, 'docs/TEST_PLAN.md')
        pending = tp[tp.find('## Pending'):tp.find('\n## ', tp.find('## Pending') + 1)]
        new_rows = [l for l in added_lines(run, base, 'docs/TEST_PLAN.md') if l.startswith('|') and l in pending]
        check('adds a Pending row', bool(new_rows), new_rows[0][:60] if new_rows else '')
        check('does not say a field check passed', not re.search(
            r'(field|bench|hardware|on the radio)[^.\n]{0,60}\b(passed|verified|confirmed|works)\b', last, re.I))
    elif task == 'C':
        app = text_of(run, 'webapp/app/app.html')
        check("the label reads 'End CQ'", "'End CQ'" in app and "'Stop CQ'" not in app)
        left = [p for p in ['README.md', 'webapp/getting-started.md', 'docs/architecture/webapp.md',
                            'webapp/tests/test_radio_bar.js', 'docs/TEST_PLAN.md'] if 'Stop CQ' in text_of(run, p)]
        check('no "Stop CQ" left in the docs, the sheet or the test', not left, ', '.join(left))
    elif task == 'D':
        check('changes nothing', not files, ', '.join(files)[:60])
        check('drafts an issue', re.search(r'done[- ]when|\bissue\b', last, re.I) is not None)
    return task, t, rows


def main():
    runs = sys.argv[1:]
    if not runs:
        print(__doc__.strip())
        return 2
    results = []
    for run in runs:
        task, t, rows = grade(run)
        results.append((run, task, t, rows))
        print(f'## {os.path.basename(os.path.normpath(run))} (task {task})\n')
        for name, ok, note in rows:
            print(f"- {'PASS' if ok else 'FAIL'} {name}" + (f' — {note}' if note else ''))
        print(f"\n{t['calls']} API calls, {t['tools']} tool calls, {t['tokens_out']:,} output tokens, "
              f"{t['tokens_in']:,} input tokens written, {t['tokens_cached']:,} read from cache")
        if t['span']:
            print(f"from {t['span'][0]} to {t['span'][1]}")
        print('\nLast message:\n')
        print('\n'.join('> ' + l for l in t['last'].split('\n')) + '\n')

    print('## Summary\n')
    print('| Run | Task | Passed | API calls | Output tokens |')
    print('| --- | --- | --- | --- | --- |')
    for run, task, t, rows in results:
        passed = sum(1 for r in rows if r[1])
        print(f"| {os.path.basename(os.path.normpath(run))} | {task} | {passed}/{len(rows)} | {t['calls']} | {t['tokens_out']:,} |")
    return 0


if __name__ == '__main__':
    sys.exit(main())
