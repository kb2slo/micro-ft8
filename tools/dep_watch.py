#!/usr/bin/env python3
"""Compare every pinned dependency with its newest release (docs/maintainer/upstream.md, Dependencies).

  python3 tools/dep_watch.py

Reads the component-manager pins in every tracked idf_component.yml of ours, the ESP-IDF
version CI builds with (tools/check.sh), and the git submodules, then asks the component
registry, GitHub tags or the submodule's remote what is newest. Prints one row per pin and,
for each pin with a newer release, where its changelog is. Read-only; needs the network and no
credentials. A pin it cannot reach prints "unreachable" and the rest still run.

A vendored copy (override_path) is compared by the version its manifest names, so a vendored
library falls behind visibly like any other.
"""
import json
import pathlib
import re
import subprocess
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
REGISTRY = 'https://components.espressif.com/api/components/'
TIMEOUT = 20


def git(*args):
    return subprocess.run(['git', *args], cwd=ROOT, check=True, capture_output=True,
                          text=True).stdout


def fetch_json(url):
    with urllib.request.urlopen(url, timeout=TIMEOUT) as r:
        return json.load(r)


def vkey(v):
    """Sort key for a release version; non-numeric parts sort first."""
    return tuple(int(p) if p.isdigit() else -1 for p in re.split(r'[.\-+]', v.lstrip('v')))


def manifests():
    """Tracked idf_component.yml files that pin our dependencies (not vendored copies)."""
    for path in git('ls-files', '*idf_component.yml').split():
        if '/managed_components/' in path or path.startswith('components/espressif__'):
            continue
        yield path


def pins(path):
    """(name, version, vendored) for each namespaced dependency in one manifest.

    The manifests here use two shapes: `ns/name: "1.2.3"` and a block with `version:` and an
    optional `override_path:` under `ns/name:`. A YAML library would be one more thing to install
    for a script whose input is this small.
    """
    current = None
    out = {}
    for raw in (ROOT / path).read_text().splitlines():
        line = raw.split('#', 1)[0].rstrip()
        m = re.match(r'^  ([\w.-]+/[\w.-]+):\s*(.*)$', line)
        if m:
            current = m.group(1)
            value = m.group(2).strip().strip('"\'')
            out[current] = [value or None, False]
            continue
        m = re.match(r'^    (version|override_path):\s*(.+)$', line)
        if m and current:
            if m.group(1) == 'version':
                out[current][0] = m.group(2).strip().strip('"\'')
            else:
                out[current][1] = True
            continue
        if re.match(r'^  \S', line):
            current = None
    return [(name, ver, vendored) for name, (ver, vendored) in out.items()]


def registry_row(name, ours):
    try:
        data = fetch_json(REGISTRY + name)
    except Exception:
        return 'unreachable', None
    versions = [v['version'] for v in data.get('versions', []) if not v.get('yanked_at')]
    if not versions:
        return 'unknown', None
    newest = max(versions, key=vkey)
    behind = sum(1 for v in versions if vkey(v) > vkey(ours or '0'))
    return newest, behind


def idf_row():
    text = (ROOT / 'tools/check.sh').read_text()
    ours = re.search(r'^IDF_VERSION="(v[\d.]+)"', text, re.M).group(1)
    try:
        tags = git('ls-remote', '--tags', '--refs', 'https://github.com/espressif/esp-idf.git', 'v*')
    except subprocess.CalledProcessError:
        return ours, 'unreachable', None, None
    releases = [t.rsplit('/', 1)[1] for t in tags.split('\n') if re.search(r'/v\d+\.\d+(\.\d+)?$', t)]
    series = ours.rsplit('.', 1)[0]
    newest_series = max((t for t in releases if t.startswith(series + '.') or t == series), key=vkey)
    newest = max(releases, key=vkey)
    return ours, newest_series, newest, sum(1 for t in releases if vkey(t) > vkey(ours))


def submodule_rows():
    rows = []
    for line in git('submodule', 'status').splitlines():
        sha, path = line[1:].split()[:2]
        url = git('config', '-f', '.gitmodules', f'submodule.{path}.url').strip()
        try:
            head = git('ls-remote', url, 'HEAD').split()[0]
        except (subprocess.CalledProcessError, IndexError):
            rows.append((path, sha[:10], 'unreachable'))
            continue
        rows.append((path, sha[:10], 'current' if head == sha else f'remote HEAD {head[:10]}'))
    return rows


def main():
    print(f'{"dependency":38} {"ours":10} {"newest":10} behind  where')
    stale = []
    for path in manifests():
        for name, ours, vendored in pins(path):
            newest, behind = registry_row(name, ours)
            tag = ' (vendored)' if vendored else ''
            print(f'{name:38} {ours or "-":10} {newest:10} {behind if behind is not None else "-":>6}  {path}{tag}')
            if behind:
                stale.append(f'https://components.espressif.com/components/{name}/versions/{newest}/changelog')
    ours, series, newest, behind = idf_row()
    print(f'{"ESP-IDF":38} {ours:10} {series:10} {behind if behind is not None else "-":>6}  '
          f'tools/check.sh (newest release {newest}; a bump is its own decision)')
    for path, sha, state in submodule_rows():
        print(f'{"submodule " + path:38} {sha:10} {state}')
    if stale:
        print('\nChangelogs to read (Fixed entries since our pin):')
        for url in dict.fromkeys(stale):
            print('  ' + url)
    return 0


if __name__ == '__main__':
    sys.exit(main())
