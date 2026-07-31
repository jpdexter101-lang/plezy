#!/bin/bash
# Measures BT.2390 against both libmpv versions, with everything else held fixed.
#
# The shipped code names mobius on the SDR fallback, and the harness cannot
# override it: the native side re-applies the whole output description on
# playback-restart, so a property set before open() is overwritten. So the
# operator is patched out of the source for the duration of this test, the
# checkout is restored afterwards, and nothing is committed.
#
# What it answers: whether the collapsed highlights are a libmpv 0.40 defect that
# upgrading fixes, or a property of the GL renderer's BT.2390 in both versions. If
# the latter, the good curve seen from standalone mpv came from its Vulkan
# libplacebo path, which this build excludes by construction (-Dvulkan=disabled),
# and no upgrade reaches it.
set -uo pipefail

SRC="$HOME/plezy-dev/plezy"
LINE='const char\* operator_name = (tone_map_here && !enabled) ? "mobius" : "auto";'
PATCHED='const char* operator_name = "auto";'

cd "$SRC"
trap 'git -C "$SRC" checkout -- linux/runner/mpv/mpv_player.cc' EXIT

before=$(grep -c 'operator_name = (tone_map_here' linux/runner/mpv/mpv_player.cc)
if [ "$before" != "1" ]; then
  echo "ABORT: expected exactly one operator selection, found $before"
  exit 1
fi
sed -i 's|const char\* operator_name = (tone_map_here \&\& !enabled) ? "mobius" : "auto";|const char* operator_name = "auto";|' \
  linux/runner/mpv/mpv_player.cc
grep -q 'operator_name = "auto";' linux/runner/mpv/mpv_player.cc || { echo "ABORT: patch did not apply"; exit 1; }
echo "patched operator to auto"

distrobox-enter -n plezy -- bash -lc "source \$HOME/plezy-dev/env.sh; cd $SRC; flutter build linux --release --target=lib/harness_main.dart 2>&1 | tail -2"

for ver in 41 40; do
  if [ "$ver" = "41" ]; then
    libs="\$HOME/plezy-dev/libmpv41-prefix/lib:\$HOME/plezy-dev/libmpv-prefix/lib"
  else
    libs="\$HOME/plezy-dev/libmpv-prefix/lib"
  fi
  cat > /tmp/leg-bt-$ver.sh <<EOF
#!/bin/bash
exec distrobox-enter -n plezy -- bash -c "
  export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 GDK_BACKEND=wayland
  export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
  export LD_LIBRARY_PATH=$libs
  export PLEZY_HARNESS_MEDIA=\\\$HOME/plezy-dev/media/cmp_ramp1000.mp4
  export PLEZY_HARNESS_SECONDS=30
  cd \\\$HOME/plezy-dev/plezy/build/linux/x64/release/bundle
  exec ./plezy
"
EOF
  chmod +x /tmp/leg-bt-$ver.sh
  bash "$HOME/plezy-dev/sdr-compare.sh" "bt-v$ver" plezy 16 -- bash "/tmp/leg-bt-$ver.sh" >/dev/null 2>&1
done

echo
for ver in 40 41; do
  echo "=== libmpv 0.$ver + BT.2390 (auto) ==="
  grep -oE 'output colour target.*' "$HOME/plezy-dev/sdr/bt-v$ver.log" | sort -u
  python3 "$HOME/plezy-dev/deck_sample_chart.py" "$HOME/plezy-dev/sdr/bt-v$ver.png" 1000 x 2>&1 | sed -n '9,13p'
  echo
done
echo BT2390_TEST_DONE
