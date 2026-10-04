#!/usr/bin/env bash
# Generate the Vita LiveArea asset set from the APK's own artwork.
#
#   icon0.png    128x128   home-screen bubble
#   bg.png       840x500   LiveArea background
#   startup.png  280x158   gate image
#
# Requires ImageMagick. Source art is read from a private APK extraction.
set -euo pipefail

ROOT="${1:-$(pwd)}"
APK_ROOT="${2:-$ROOT/../runtime/apk}"
OUT="${3:-$ROOT/sce_sys}"
LOGO="$APK_ROOT/res/drawable-hdpi/logo.png"
LAUNCH="$APK_ROOT/res/mipmap-xxhdpi/ic_launcher.png"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

[ -f "$LOGO" ]   || { echo "missing $LOGO"   >&2; exit 1; }
[ -f "$LAUNCH" ] || { echo "missing $LAUNCH" >&2; exit 1; }

# The Vita package installer rejects the entire VPK with error 0x8010113D if any
# sce_sys PNG is not 8-bit indexed, so everything here is written through PNG8.
# Floyd-Steinberg keeps the 256-colour limit from banding the space gradient.
png8() {  # png8 <in> <out>
  convert "$1" -dither FloydSteinberg -colors 256 -strip "PNG8:$2"
}

mkdir -p "$OUT/livearea/contents"

# ---------------------------------------------------------------- starfield --
# Sparse white points, two passes at different densities so the field has both
# faint background stars and a few bright ones.
# NB: -separate leaves -channel active, so +channel must reset it before
# -threshold or only that one channel gets thresholded and the rest comes
# through as full-intensity noise.
stars() {  # stars <w> <h> <out>
  local w=$1 h=$2 out=$3
  convert -size "${w}x${h}" xc:black +noise Random -channel G -separate +channel \
          -threshold 99.5% -blur 0x0.4 -evaluate multiply 0.5 "$TMP/s1.png"
  convert -size "${w}x${h}" xc:black +noise Random -channel R -separate +channel \
          -threshold 99.94% -blur 0x0.8 -evaluate multiply 0.85 "$TMP/s2.png"
  convert "$TMP/s1.png" "$TMP/s2.png" -compose screen -composite "$out"
}

# --------------------------------------------------------------------- bg.png
# Deep space gradient, warm nebula glow, starfield, logo, vignette.
#
# NB: radial-gradient only reaches its end colour at the *corner* radius, so the
# edge midpoints stay lit and the layer screens on as a visible rectangle. Every
# glow here is a drawn ellipse blurred into black instead, which falls off on
# all sides.
glow() {  # glow <w> <h> <rx> <ry> <colour> <blur> <mul> <out>
  convert -size "${1}x${2}" xc:black -fill "$5" \
          -draw "ellipse $(( $1 / 2 )),$(( $2 / 2 )) ${3},${4} 0,360" \
          -blur "0x${6}" -evaluate multiply "$7" "$8"
}

convert -size 840x500 gradient:'#0d1526'-'#010206' "$TMP/base.png"

glow 840 500 300 170 '#2a1e10' 90 0.9 "$TMP/neb.png"
convert "$TMP/base.png" "$TMP/neb.png" -compose screen -composite "$TMP/bg1.png"

stars 840 500 "$TMP/stars.png"
convert "$TMP/bg1.png" "$TMP/stars.png" -compose screen -composite "$TMP/bg2.png"

# Warm bloom behind where the logo lands, so it sits in the scene.
glow 620 360 175 95 '#7a5a1e' 60 0.55 "$TMP/bloom.png"
convert "$TMP/bg2.png" "$TMP/bloom.png" \
        -geometry +225+10 -compose screen -composite "$TMP/bg3.png"

# Logo, right of the gate column so the LiveArea frame never covers it.
convert "$LOGO" -resize 470x "$TMP/logo_bg.png"
convert "$TMP/bg3.png" "$TMP/logo_bg.png" \
        -gravity none -geometry +300+95 -compose over -composite "$TMP/bg4.png"

# Vignette + a thin horizon line for depth.
convert -size 840x500 radial-gradient:white-'#5a5a5a' -resize 840x500\! "$TMP/vig.png"
convert "$TMP/bg4.png" "$TMP/vig.png" -compose multiply -composite "$TMP/bg5.png"

convert "$TMP/bg5.png" \
        -fill '#c9a227' -stroke none \
        -font DejaVu-Sans -pointsize 15 \
        -gravity SouthEast -annotate +28+22 'PlayStation Vita port' \
        -alpha off "$TMP/bg_final.png"
png8 "$TMP/bg_final.png" "$OUT/livearea/contents/bg.png"

# ---------------------------------------------------------------- startup.png
# The gate. Same treatment, tighter crop, logo fills it.
convert -size 280x158 gradient:'#101a2e'-'#010206' "$TMP/g1.png"
stars 280 158 "$TMP/gstars.png"
convert "$TMP/g1.png" "$TMP/gstars.png" -compose screen -composite "$TMP/g2.png"
glow 280 158 78 44 '#7a5a1e' 26 0.6 "$TMP/gbloom.png"
convert "$TMP/g2.png" "$TMP/gbloom.png" -compose screen -composite "$TMP/g3.png"
convert "$LOGO" -resize 214x "$TMP/logo_g.png"
convert "$TMP/g3.png" "$TMP/logo_g.png" -gravity center -compose over -composite \
        -alpha off "$TMP/g_final.png"
png8 "$TMP/g_final.png" "$OUT/livearea/contents/startup.png"

# -------------------------------------------------------------------- icon0.png
# The bubble. The Android launcher icon is already composed for this size and
# carries its own gold rounded border with transparent corners -- masking it
# again just clips that border, so only rescale.
convert "$LAUNCH" -resize 128x128 "$TMP/i_final.png"
png8 "$TMP/i_final.png" "$OUT/icon0.png"

echo "wrote:"
identify -format '  %f  %wx%h  %b\n' \
  "$OUT/icon0.png" \
  "$OUT/livearea/contents/bg.png" \
  "$OUT/livearea/contents/startup.png"
