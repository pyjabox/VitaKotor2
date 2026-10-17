#!/bin/bash
# vita3k_shots.sh <out-dir>
#
# Screenshots for a Vita3K autotest run: run it next to tools/vita3k_autotest.sh
# with an autotest build. Each request "<n> <what> <phase>" the loader writes to
# ux0:data/kotor2/shot.txt (loader/autotest.c) captures the Vita3K game window
# to <out-dir>/<n>-<what>-<phase>.png. Stops when "[autotest] DONE" or a failure is
# logged, or after 15 minutes. Needs screen-recording permission for the
# terminal (window titles readable).
set -u
OUT="${1:?usage: vita3k_shots.sh <out-dir>}"
mkdir -p "$OUT"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG="$HOME/Library/Application Support/Vita3K/Vita3K/fs/ux0/data/kotor2/log.txt"
# The window lookup, compiled once (about 30 s) and kept: start this script a
# little before the autotest so the first shots are not late.
WINID_BIN="${WINID_BIN:-$HOME/Library/Caches/vitakotor2/vita3k_winid}"
if [ ! -x "$WINID_BIN" ] || [ "$HERE/vita3k_winid.swift" -nt "$WINID_BIN" ]; then
  mkdir -p "$(dirname "$WINID_BIN")"
  swiftc -O "$HERE/vita3k_winid.swift" -o "$WINID_BIN" || exit 2
fi
echo "ready"
END=$(( $(date +%s) + 900 ))
REQ="$(dirname "$LOG")/shot.txt"
rm -f "$REQ"
LAST=
# The autotest script empties the log at launch: wait for that (no DONE left
# from the previous run), then follow the requests.
while grep -a -q "\[autotest\] DONE\|\[autotest\].*-> fail" "$LOG" 2>/dev/null; do
  [ "$(date +%s)" -lt "$END" ] || exit 1
  sleep 0.5
done
while [ "$(date +%s)" -lt "$END" ]; do
  CUR=$(head -1 "$REQ" 2>/dev/null)
  if [ -n "$CUR" ] && [ "$CUR" != "$LAST" ]; then
    LAST=$CUR
    read -r n what phase <<< "$CUR"
    ID=$("$WINID_BIN")
    if [ -n "$ID" ]; then
      screencapture -x -o -l "$ID" "$OUT/$n-$what-$phase.png" && echo "shot $n $what $phase"
    else
      echo "shot $n $what $phase: no Vita3K window"
    fi
  fi
  grep -a -q "\[autotest\] DONE\|\[autotest\].*-> fail" "$LOG" 2>/dev/null && break
  sleep 0.1
done
