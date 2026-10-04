#!/bin/bash
# snapshot.sh -- keep the last N builds so a bad change is one copy away from undone.
#
# Stores each build under backups/ as a PAIR:
#   KOTOR2-<stamp>.vpk   the installable build
#   src-<stamp>.tar.gz   loader/ + CMakeLists.txt as they were when it was built
# The .vpk alone lets you revert on the Vita; the source snapshot is what lets
# development continue from that point, so they are kept and pruned together.
#
# Runs automatically after every build (CMakeLists POST_BUILD) and never fails
# the build -- a backup problem must not block getting a .vpk out.

set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VPK="$ROOT/build/KOTOR2.vpk"
ELF="$ROOT/build/KOTOR2"
DIR="$ROOT/backups"
KEEP="${KEEP_BUILDS:-4}"

[ -f "$VPK" ] || { echo "snapshot: no $VPK yet, skipping"; exit 0; }
mkdir -p "$DIR" || exit 0

# Name by the stamp baked into the ELF so a backup always matches its log header.
STAMP="$(strings -a "$ELF" 2>/dev/null | sed -n 's/^=== BUILD \(.*\) ===$/\1/p' | head -1)"
[ -n "$STAMP" ] || STAMP="$(date '+%b %d %Y %H:%M:%S')"
# __DATE__ space-pads a single-digit day ("Aug  1"); zero-pad it so names sort.
SLUG="$(echo "$STAMP" | sed 's/  \([0-9]\) / 0\1 /' | tr ' :' '--')"

if [ -f "$DIR/KOTOR2-$SLUG.vpk" ]; then exit 0; fi   # same stamp, already saved

cp "$VPK" "$DIR/KOTOR2-$SLUG.vpk" 2>/dev/null || exit 0
tar -czf "$DIR/src-$SLUG.tar.gz" -C "$ROOT" loader CMakeLists.txt 2>/dev/null

# Prune oldest-first, by mtime, keeping the newest $KEEP of each kind.
prune() {
  local pat="$1" n
  n=$(ls -1t "$DIR"/$pat 2>/dev/null | wc -l)
  if [ "$n" -gt "$KEEP" ]; then
    ls -1t "$DIR"/$pat 2>/dev/null | tail -n +$((KEEP + 1)) | while read -r f; do
      echo "snapshot: pruning $(basename "$f")"
      rm -f "$f"
    done
  fi
}
prune 'KOTOR2-*.vpk'
prune 'src-*.tar.gz'

echo "snapshot: saved KOTOR2-$SLUG.vpk (keeping $KEEP)"
ls -1t "$DIR"/KOTOR2-*.vpk 2>/dev/null | sed 's/^/snapshot:   /'
exit 0
