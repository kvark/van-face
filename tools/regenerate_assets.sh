#!/usr/bin/env bash
# Regenerate every PNG frame under resources/images/ from the Vangers shop
# videos. Produces three sets so each Pebble platform gets the right rendering:
#
#   large/  200×150, gamma 1.0           — emery, gabbro (color, full size)
#   small/  144×108, gamma 1.0           — basalt, chalk (color, small screen)
#   bw/     144×108, gamma 0.4 + dither  — aplite, diorite, flint (b/w)
#
# The vehicle→video mapping below was discovered from
# /x/Work/VangersData/actint/mech_prm.inc — actint's MECHxx_ID numbering
# isn't the same as the m1..m14 slugs in the m3d filenames. Variant suffix
# picks the escave faction colour: '' = blue (Fee), 2 = green (Zeex),
# 3 = orange (Khox). m1..m5 are green, m6..m10 orange, m11..m14 blue —
# the watchface routes them by time of day.
#
# Usage:  ./regenerate_assets.sh /path/to/VangersData
#         (defaults to /x/Work/VangersData if no arg)

set -euo pipefail
DATA_ROOT="${1:-/x/Work/VangersData}"
VIDEO_DIR="$DATA_ROOT/resource/video"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)/resources/images"
TOOL="python3 $SCRIPT_DIR/build_mech_frames.py"

if [ ! -d "$VIDEO_DIR" ]; then
  echo "Vangers video dir not found: $VIDEO_DIR" >&2
  echo "Pass the VangersData root as the first arg." >&2
  exit 1
fi

# m{slug}={mechNN base} - the integer that the .avi files use.
declare -A VBASE=(
  [m1]=mech06 [m2]=mech01 [m3]=mech07  [m4]=mech00  [m5]=mech04
  [m6]=mech02 [m7]=mech03 [m8]=mech05  [m9]=mech09  [m10]=mech10
  [m11]=mech11 [m12]=mech12 [m13]=mech13 [m14]=mech14
)
# Variant suffix per slug. '' (base) is blue, '2' green, '3' orange.
declare -A VSUFFIX=(
  [m1]=2  [m2]=2  [m3]=2  [m4]=2  [m5]=2       # green  (Zeex)
  [m6]=3  [m7]=3  [m8]=3  [m9]=3  [m10]=3      # orange (Khox)
  [m11]= [m12]= [m13]= [m14]=                  # blue   (Fee, base)
)
VEHICLES=(m1 m2 m3 m4 m5 m6 m7 m8 m9 m10 m11 m12 m13 m14)

mkdir -p "$OUT_ROOT/large" "$OUT_ROOT/small" "$OUT_ROOT/bw"
rm -f "$OUT_ROOT"/large/*.png "$OUT_ROOT"/small/*.png "$OUT_ROOT"/bw/*.png

for veh in "${VEHICLES[@]}"; do
  src="$VIDEO_DIR/${VBASE[$veh]}${VSUFFIX[$veh]}.avi"
  if [ ! -f "$src" ]; then
    echo "missing video: $src" >&2
    exit 1
  fi
  echo "[$veh] $src"
  $TOOL "$src" "$OUT_ROOT/large" --frames 6 --width 200 --height 150 --prefix "$veh" >/dev/null
  $TOOL "$src" "$OUT_ROOT/small" --frames 6 --width 144 --height 108 --prefix "$veh" >/dev/null
  $TOOL "$src" "$OUT_ROOT/bw"    --frames 6 --width 144 --height 108 --prefix "$veh" \
        --gamma 0.4 --dither >/dev/null
done

echo
echo "Done. Sizes:"
du -sh "$OUT_ROOT"/{large,small,bw}
