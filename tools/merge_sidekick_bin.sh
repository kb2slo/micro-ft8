#!/usr/bin/env bash
# Merge staged sidekick flash pieces into one 0x0 image for desk esptool.
# Offsets match components/sidekick_flasher (bootloader 0x0, table 0x8000,
# otadata 0xF000, app/ota_0 0x20000) and sidekick/partitions.csv.
#
# Usage: tools/merge_sidekick_bin.sh [stagedir] [outfile]
# Defaults: components/sidekick_flasher/target_firmware/{,sidekick_merged.bin}
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="${1:-$ROOT/components/sidekick_flasher/target_firmware}"
OUT="${2:-$DIR/sidekick_merged.bin}"

for f in bootloader.bin partition-table.bin ota_data_initial.bin sidekick.bin; do
  test -s "$DIR/$f" || {
    echo "error: missing or empty $DIR/$f — run tools/stage_sidekick_firmware.sh first" >&2
    exit 1
  }
done

# Prefer python -m esptool (ESP-IDF / pip); fall back to esptool.py on PATH.
if python3 -c "import esptool" 2>/dev/null; then
  ESPTOOL=(python3 -m esptool)
elif command -v esptool.py >/dev/null 2>&1; then
  ESPTOOL=(esptool.py)
else
  echo "error: esptool not found (pip install esptool)" >&2
  exit 1
fi

"${ESPTOOL[@]}" --chip esp32s3 merge_bin -o "$OUT" \
  0x0 "$DIR/bootloader.bin" \
  0x8000 "$DIR/partition-table.bin" \
  0xf000 "$DIR/ota_data_initial.bin" \
  0x20000 "$DIR/sidekick.bin"

echo "Merged $OUT ($(wc -c <"$OUT") bytes)"
