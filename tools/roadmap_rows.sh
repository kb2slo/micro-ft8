#!/usr/bin/env bash
# ROADMAP.md hygiene (docs/README.md "Doc types" > "Roadmap rows").
#
#   tools/roadmap_rows.sh                  rows over the size limit, median length, long-ok count
#   tools/roadmap_rows.sh new              print a fresh random row ID never seen in git history
#   tools/roadmap_rows.sh health [--at REV]
#                                          backlog health and drift budget, now or as of REV
#   tools/roadmap_rows.sh dangling [--at REV]
#                                          docs citing a row ID that is no longer a row (nor Declined)
#
# The constants below are the only place these numbers live. Each grooming retro
# re-reads the output and decides whether any of them should move.
set -euo pipefail

LIMIT=1000              # row size, characters, padding collapsed -- every section, Blocked included
BUDGET_READ_KB=40       # ROADMAP.md size; the session-start read cost (also a trigger for row c9yz)
BUDGET_OVERSIZE_PCT=15  # rows over LIMIT, % of all rows
BUDGET_NO_DONE_PCT=20   # Backlog rows with no Done-when, % of Backlog
BUDGET_NOW_ROWS=3       # rows in Now (theme count is judged at grooming, not here)
BUDGET_OWED=30          # owed field checks in TEST_PLAN Pending (standing row swsu)
FLOW_DAYS=14            # window for rows added vs removed

ROADMAP="docs/ROADMAP.md"
TEST_PLAN="docs/TEST_PLAN.md"

cd "$(git rev-parse --show-toplevel)"

# Table rows of a ROADMAP.md text on stdin:
#   "<section> <length> <id> <long-ok 0|1> <done-when 0|1> <blocked-on 0|1>".
rows() {
  awk -F'|' '
    /^## / { sec = $0; sub(/^## /, "", sec); gsub(/ /, "_", sec) }
    /^\| *[A-Za-z0-9]+ *\|/ && !/^\| *(ID|---)/ {
      s = $0; gsub(/  +/, " ", s)
      id = $2; gsub(/ /, "", id)
      print sec, length(s), id, (index(s, "`long-ok`") ? 1 : 0), (tolower(s) ~ /done[- ]when/ ? 1 : 0), (index(s, "Blocked on:") ? 1 : 0)
    }'
}

new_id() {
  # 4 chars from a lowercase alphabet without look-alikes (0/o, 1/l/i), led by
  # a letter. Uppercase B/I IDs predate this scheme and cannot collide.
  local letters="abcdefghjkmnpqrstuvwxyz" alnum="abcdefghjkmnpqrstuvwxyz23456789"
  local seen id i
  seen="$( (git log -p --all -- "$ROADMAP"; cat "$ROADMAP") | grep -oE '\b[a-z][a-z2-9]{3}\b' | sort -u)"
  for _ in $(seq 1 100); do
    id="${letters:RANDOM%${#letters}:1}"
    for i in 1 2 3; do id+="${alnum:RANDOM%${#alnum}:1}"; done
    if ! grep -qx "$id" <<<"$seen"; then echo "$id"; return; fi
  done
  echo "no free ID after 100 tries" >&2; exit 1
}

report() {
  local data total over longok median
  data="$(rows <"$ROADMAP" | awk '{ print $2, $3, $4 }' | sort -n)"
  total="$(wc -l <<<"$data" | tr -d ' ')"
  over="$(awk -v L="$LIMIT" '$1 > L' <<<"$data" | sort -rn)"
  longok="$(awk '$3 == 1' <<<"$data" | wc -l | tr -d ' ')"
  median="$(awk '{ a[NR] = $1 } END { print a[int((NR + 1) / 2)] }' <<<"$data")"
  echo "limit ${LIMIT} chars · ${total} rows · median ${median} · long-ok ${longok}"
  if [[ -n "$over" ]]; then
    echo "over the limit ($(wc -l <<<"$over" | tr -d ' ')):"
    awk '{ printf "  %5d  %s%s\n", $1, $2, ($3 ? "  (long-ok)" : "") }' <<<"$over"
  fi
}

# Row IDs that were once rows in ROADMAP.md (as of REV) but are not rows now,
# in any section -- Declined lines count as rows, so a declined ID is not dead.
# Only real ID shapes: B12 / I28a / D7, or an opaque 4-char ID.
dead_ids() {
  local rev="$1" text="$2"
  local id_re='^([BID][0-9]+[a-z]?|[a-z][a-z2-9]{3})$'
  comm -23 \
    <(git log -p "$rev" -- "$ROADMAP" | awk -F'|' '/^[-+ ]\| *[A-Za-z0-9]+ *\|/ && !/^[-+ ]\| *(ID|---)/ { id = $2; gsub(/ /, "", id); print id }' \
        | grep -E "$id_re" | sort -u) \
    <(rows <<<"$text" | awk '{ print $3 }' | sort -u)
}

# "<file>: <id> <id> ..." for every doc citing a dead ID. TEST_PLAN.md is left
# out (row IDs label what a check covers there), and so is rfcs/rejected/ (frozen).
dangling() {
  local rev="" worktree=1
  if [[ "${1:-}" == "--at" ]]; then rev="$2"; worktree=0; fi
  local text re
  if (( worktree )); then text="$(cat "$ROADMAP")"; else text="$(git show "$rev:$ROADMAP")"; fi
  re="$(dead_ids "${rev:-HEAD}" "$text" | paste -sd'|' -)"
  [[ -z "$re" ]] && return 0
  # In the worktree, a doc not yet added counts too (tools/check_docs.py reads it before its commit).
  local untracked=""
  (( worktree )) && untracked="--untracked"
  git grep ${untracked:+"$untracked"} -owE "($re)" ${rev:+"$rev"} -- 'docs/*.md' ':!docs/TEST_PLAN.md' ':!docs/rfcs/rejected/*' \
    | sed "s|^${rev:+$rev:}||" \
    | awk -F: '!seen[$1 FS $2]++ { ids[$1] = ids[$1] " " $2 } END { for (f in ids) print f ":" ids[f] }' | sort
}

pct() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%d", (b ? 100 * a / b : 0) }'; }

# IDs added / removed by ROADMAP.md diffs in git log args "$@".
flow() {
  git log -p "$@" -- "$ROADMAP" | awk '
    /^[-+]\| *[A-Za-z0-9]+ *\|/ && !/^[-+]\| *(ID|---)/ {
      split(substr($0, 2), x, "|"); id = x[2]; gsub(/ /, "", id)
      if (substr($0, 1, 1) == "+") add[id] = 1; else del[id] = 1
    }
    END { for (k in add) if (!(k in del)) a++; for (k in del) if (!(k in add)) d++; print a + 0, d + 0 }'
}

health() {
  local rev="HEAD" worktree=1
  if [[ "${1:-}" == "--at" ]]; then rev="$2"; worktree=0; fi
  local text tp now since
  if (( worktree )); then
    text="$(cat "$ROADMAP")"; tp="$(cat "$TEST_PLAN")"; now="$(date +%s)"
  else
    text="$(git show "$rev:$ROADMAP")"; tp="$(git show "$rev:$TEST_PLAN")"
    now="$(git show -s --format=%ct "$rev")"
  fi
  since="$(( now - FLOW_DAYS * 86400 ))"

  local data bytes total over longok now_rows bl no_done blocked declined no_blocked_on added removed owed
  data="$(rows <<<"$text")"
  bytes="$(printf '%s\n' "$text" | wc -c | tr -d ' ')"
  total="$(wc -l <<<"$data" | tr -d ' ')"
  over="$(awk -v L="$LIMIT" '$2 > L' <<<"$data" | wc -l | tr -d ' ')"
  longok="$(awk '$4 == 1' <<<"$data" | wc -l | tr -d ' ')"
  now_rows="$(awk '$1 == "Now"' <<<"$data" | wc -l | tr -d ' ')"
  bl="$(awk '$1 == "Backlog"' <<<"$data" | wc -l | tr -d ' ')"
  no_done="$(awk '$1 == "Backlog" && $5 == 0' <<<"$data" | wc -l | tr -d ' ')"
  blocked="$(awk '$1 == "Blocked"' <<<"$data" | wc -l | tr -d ' ')"
  declined="$(awk '$1 == "Declined"' <<<"$data" | wc -l | tr -d ' ')"
  no_blocked_on="$(awk '$1 == "Blocked" && $6 == 0 { print $3 }' <<<"$data" | tr '\n' ' ')"
  read -r added removed < <(flow --since="@$since" --until="@$now" "$rev")
  # Owed field checks: Pending table rows whose status cell does not say the row is closed. Bold is
  # stripped first -- a "**pass 2026-..**" left undropped once counted as owed and inflated this to 91.
  owed="$(awk -F'|' '/^## Pending/ { p = 1; next } /^## / { p = 0 }
    p && /^\| / && !/^\| *(From|---)/ { s = tolower($(NF - 1)); gsub(/\*/, "", s); gsub(/^ +/, "", s)
      if (s !~ /^(pass|answered|superseded|n\/a)/) n++ }
    END { print n + 0 }' <<<"$tp")"

  # Oldest Backlog row, by intake (first commit that added its row), not last edit.
  local oldest_id="" oldest_days=0 id t d
  for id in $(awk '$1 == "Backlog" { print $3 }' <<<"$data"); do
    t="$(git log --reverse --format=%ct -S"| $id " "$rev" -- "$ROADMAP" | awk 'NR == 1')"
    [[ -z "$t" ]] && continue
    d="$(( (now - t) / 86400 ))"
    if (( d > oldest_days )); then oldest_days="$d"; oldest_id="$id"; fi
  done

  local over_pct nodone_pct
  over_pct="$(pct "$over" "$total")"; nodone_pct="$(pct "$no_done" "$bl")"

  echo "health @ $( (( worktree )) && echo "worktree" || git rev-parse --short "$rev")"
  printf '  read cost        %d KB (~%dk tokens)                budget %d KB\n' "$(( bytes / 1024 ))" "$(( bytes / 4000 ))" "$BUDGET_READ_KB"
  printf '  oversize rows    %d / %d (%d%%), long-ok %d        budget %d%%\n' "$over" "$total" "$over_pct" "$longok" "$BUDGET_OVERSIZE_PCT"
  printf '  Now rows         %d                               budget %d\n' "$now_rows" "$BUDGET_NOW_ROWS"
  printf '  no Done-when     %d / %d Backlog (%d%%)            budget %d%%\n' "$no_done" "$bl" "$nodone_pct" "$BUDGET_NO_DONE_PCT"
  printf '  Blocked rows     %d%s · Declined %d\n' "$blocked" "${no_blocked_on:+, missing Blocked on: $no_blocked_on}" "$declined"
  printf '  flow %dd          +%d / -%d rows\n' "$FLOW_DAYS" "$added" "$removed"
  printf '  oldest Backlog   %s, %d days since intake\n' "${oldest_id:-none}" "$oldest_days"
  printf '  owed field checks %d (TEST_PLAN Pending)          budget %d\n' "$owed" "$BUDGET_OWED"
  # Reported, not budgeted: most dead citations sit in RFCs awaiting graduation (row k7gc).
  local dang
  dang="$(if (( worktree )); then dangling; else dangling --at "$rev"; fi)"
  printf '  dangling IDs     %d file(s) cite a dropped row ID    (tools/roadmap_rows.sh dangling)\n' "$(grep -c . <<<"$dang" || true)"

  local why=()
  (( bytes / 1024 > BUDGET_READ_KB )) && why+=("read cost $(( bytes / 1024 )) KB")
  (( over_pct > BUDGET_OVERSIZE_PCT )) && why+=("oversize ${over_pct}%")
  [[ -n "$no_blocked_on" ]] && why+=("Blocked rows missing Blocked on")
  (( now_rows > BUDGET_NOW_ROWS )) && why+=("Now ${now_rows} rows")
  (( nodone_pct > BUDGET_NO_DONE_PCT )) && why+=("no Done-when ${nodone_pct}%")
  (( owed > BUDGET_OWED )) && why+=("owed field checks ${owed}")
  # Inflow beating outflow only counts if the Backlog also grew since the last groom.
  local last_groom bl_then
  last_groom="$(git log -1 --format=%H --grep='^ROADMAP: groom' "$rev" -- "$ROADMAP" || true)"
  if (( added > removed )) && [[ -n "$last_groom" ]]; then
    bl_then="$(git show "$last_groom:$ROADMAP" | rows | awk '$1 == "Backlog"' | wc -l | tr -d ' ')"
    (( bl > bl_then )) && why+=("inflow > outflow, Backlog ${bl_then} -> ${bl}")
  fi
  # Carry-over from the last groom lives in ROADMAP.md's "## Next groom" section, not git.
  local carry
  carry="$(awk '/^## Next groom/ { p = 1; next } /^## / { p = 0 } p && /^- /' <<<"$text")"
  if [[ -n "$carry" ]]; then
    printf '  next groom       %d carried item(s):\n%s\n' "$(wc -l <<<"$carry" | tr -d ' ')" "$(sed 's/^/    /' <<<"$carry")"
  fi
  if (( ${#why[@]} )); then
    local IFS=';'; echo "  budget           OVER: ${why[*]} -- groom before feature work"
  else
    echo "  budget           OK"
  fi
}

case "${1:-}" in
  "") report ;;
  new) new_id ;;
  health) shift; health "$@" ;;
  dangling) shift; dangling "$@" ;;
  *) echo "usage: $0 [new | health [--at REV] | dangling [--at REV]]" >&2; exit 2 ;;
esac
