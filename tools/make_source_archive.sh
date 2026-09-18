#!/usr/bin/env bash
# Build a history-free source tarball of HEAD, including the ft8_lib submodule
# tree as plain files (no .git). Used by release-main for the public mirror.
#
# Usage: tools/make_source_archive.sh <outdir> <short-sha>
# Writes: <outdir>/microft8-source-<short-sha>.tar.gz
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTDIR="${1:?outdir}"
SHORT="${2:?short-sha}"
PREFIX="microft8-${SHORT}"
OUT="${OUTDIR}/microft8-source-${SHORT}.tar.gz"
VENDOR_REL="components/ft8_lib/vendor"

cd "$ROOT"
mkdir -p "$OUTDIR"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Omit editor/agent stubs — not part of the firmware tree operators build.
git archive --format=tar --prefix="${PREFIX}/" HEAD \
  ':(exclude).cursor' \
  ':(exclude)AGENTS.md' \
  ':(exclude)CLAUDE.md' \
  | tar -x -C "$tmp"

if [[ ! -e "$VENDOR_REL/.git" && ! -d "$VENDOR_REL" ]]; then
  echo "error: $VENDOR_REL missing — checkout with submodules before archiving" >&2
  exit 1
fi

# Empty directory from the superproject archive; replace with the pinned tree.
rm -rf "${tmp}/${PREFIX}/${VENDOR_REL}"
mkdir -p "${tmp}/${PREFIX}/$(dirname "$VENDOR_REL")"
git -C "$VENDOR_REL" archive --format=tar --prefix="${PREFIX}/${VENDOR_REL}/" HEAD \
  | tar -x -C "$tmp"

if [[ ! -f "${tmp}/${PREFIX}/${VENDOR_REL}/CMakeLists.txt" \
   && ! -f "${tmp}/${PREFIX}/${VENDOR_REL}/README.md" \
   && ! -d "${tmp}/${PREFIX}/${VENDOR_REL}/ft8" ]]; then
  echo "error: ft8_lib vendor tree looks empty after archive" >&2
  exit 1
fi

tar -czf "$OUT" -C "$tmp" "$PREFIX"
sha256sum "$OUT" | tee "${OUT}.sha256"
echo "Wrote $OUT"
