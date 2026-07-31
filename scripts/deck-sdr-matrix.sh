#!/bin/bash
# usage: deck-sdr-matrix.sh <clip-filename> <tag>
#
# Runs the same HDR10 clip through every renderer that matters and captures each
# one on the SDR panel. The interesting comparison is not just ours-vs-theirs but
# *which side* tone-maps: mpv, KWin, or Kodi.
#
# mpv is run twice on purpose. Standalone mpv defaults to vo=gpu-next
# (libplacebo), while our runner drives libmpv's render API, which is the legacy
# `gpu` renderer. Testing both separates "our integration looks different" from
# "the legacy tone-mapper looks different", which are entirely different bugs.
set -uo pipefail
clip=$1; tag=$2
cd "$HOME/plezy-dev" || exit 1
media="$HOME/plezy-dev/media/$clip"
[ -f "$media" ] || { echo "no such clip: $media"; exit 1; }

run() { echo; bash sdr-compare.sh "$1" "$2" "$3" -- "${@:4}"; }

run "$tag-plezy-off"     plezy 14 bash ./deck-plezy-leg.sh "$clip" 0
run "$tag-plezy-comp"    plezy 14 bash ./deck-plezy-leg.sh "$clip" 1 compositor
run "$tag-plezy-player"  plezy 14 bash ./deck-plezy-leg.sh "$clip" 1 player
run "$tag-mpv-gpunext"   mpv   12 flatpak run --user --filesystem=host io.mpv.Mpv \
      --fullscreen --no-config --loop-file=inf --really-quiet --vo=gpu-next "$media"
run "$tag-mpv-gpu"       mpv   12 flatpak run --user --filesystem=host io.mpv.Mpv \
      --fullscreen --no-config --loop-file=inf --really-quiet --vo=gpu "$media"
run "$tag-kodi"          kodi  32 flatpak run --user --filesystem=host tv.kodi.Kodi "$media"

echo
echo "=== captures for $tag ==="
ls -la "$HOME/plezy-dev/sdr/$tag-"*.png 2>/dev/null | awk '{printf "  %8d  %s\n", $5, $9}'
