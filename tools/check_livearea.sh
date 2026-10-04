#!/usr/bin/env bash
# Validate the LiveArea assets against what the Vita package installer accepts.
#
# The installer rejects the whole VPK with error 0x8010113D if any sce_sys PNG
# is not 8-bit indexed (PNG colour type 3), or is not exactly the right size.
# That failure happens at install time, not at render time, so it is worth
# checking before packing rather than after flashing to a console.
set -uo pipefail

ROOT="${1:-$(dirname "$0")/..}"
SCE="$ROOT/sce_sys"
fail=0

file_size() {
  stat -c%s "$1" 2>/dev/null || stat -f%z "$1"
}

check() {  # check <file> <w> <h>
  local f=$1 w=$2 h=$3 geom ctype depth sig
  local w0 w1 w2 w3 h0 h1 h2 h3
  if [ ! -f "$f" ]; then
    echo "FAIL  $(basename "$f"): missing"; fail=1; return
  fi

  sig=$(od -An -tx1 -N8 "$f" | tr -d ' \n')
  if [ "$sig" != "89504e470d0a1a0a" ]; then
    echo "FAIL  $(basename "$f"): not a PNG"; fail=1; return
  fi

  # PNG stores width, height, bit depth and colour type at fixed offsets in its
  # mandatory first IHDR chunk. Read those bytes directly so this check works in
  # the VitaSDK container without requiring ImageMagick.
  read -r w0 w1 w2 w3 h0 h1 h2 h3 depth ctype < <(od -An -tu1 -j16 -N10 "$f")
  geom="$((w0 * 16777216 + w1 * 65536 + w2 * 256 + w3))x$((h0 * 16777216 + h1 * 65536 + h2 * 256 + h3))"

  if [ "$geom" != "${w}x${h}" ]; then
    echo "FAIL  $(basename "$f"): size $geom, need ${w}x${h}"; fail=1; return
  fi
  case "$ctype" in
    3*) ;;
    *)  echo "FAIL  $(basename "$f"): colour type '$ctype', need 3 (indexed)"; fail=1; return ;;
  esac
  if [ "$depth" != "8" ]; then
    echo "FAIL  $(basename "$f"): bit depth $depth, need 8"; fail=1; return
  fi
  echo "ok    $(basename "$f")  $geom  indexed/8-bit  $(file_size "$f") bytes"
}

check "$SCE/icon0.png"                      128 128
check "$SCE/livearea/contents/bg.png"       840 500
check "$SCE/livearea/contents/startup.png"  280 158

[ $fail -eq 0 ] && echo "LiveArea assets OK" || echo "LiveArea assets WILL FAIL TO INSTALL (0x8010113D)"
exit $fail
