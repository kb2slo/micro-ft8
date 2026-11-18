#!/usr/bin/env bash
# A night of a practical worst case on the sidekick (README.md, Overnight): two app pages left open and reloaded
# every 5 minutes, a slow client for a minute every 10, an events read every 2 s and the heap every 5 s, for
# HOURS. Writes its logs into OUT (made if missing). Read-only: nothing is written to the station.
#   tools/sidekick_load/overnight.sh http://192.168.50.128 8 /tmp/overnight
set -u
HOST=${1:?sidekick URL}; HOURS=${2:?hours}; OUT=${3:?log directory}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
MIN=$(python3 -c "print(int(float('$HOURS') * 60))")
SECS=$((MIN * 60))
echo "$(date -u +%FT%TZ) start: $HOURS h on $HOST" >> "$OUT/run.log"
python3 "$HERE/heap.py" "$HOST" "$MIN" "$OUT/heap.jsonl" &
python3 "$HERE/probe.py" "$HOST" "$SECS" 2 "$OUT/probe.log" "$OUT/log.txt" &
node "$HERE/load.js" "$HOST" "$MIN" 2 300 >> "$OUT/load.log" 2>&1 &
end=$(( $(date +%s) + SECS ))
while [ "$(date +%s)" -lt $((end - 60)) ]; do
  sleep 540
  [ "$(date +%s)" -lt $((end - 60)) ] || break
  python3 "$HERE/slow_client.py" "$HOST" 60 /app.html 1024 0.25 >> "$OUT/slow.log" 2>&1
done
wait
echo "$(date -u +%FT%TZ) done" >> "$OUT/run.log"
