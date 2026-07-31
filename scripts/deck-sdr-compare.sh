#!/bin/bash
# usage: deck-sdr-compare.sh <label> <window-class-substring> <settle-seconds> -- <launch cmd...>
#
# Captures how one player renders HDR10 content on this Deck's *SDR* internal
# panel. eDP-1 reports "HDR: incapable", so every leg necessarily ends in a
# tone-map to SDR - by mpv, by KWin, or by Kodi - and the composited result is
# an ordinary sRGB image. That is the whole reason this comparison can be made
# digitally: a screenshot of an SDR output is a faithful record of what the eye
# sees, so no camera and no human judgement are needed. (On an HDR output a
# screenshot cannot represent the signal, which is why the external-display legs
# still need photographs.)
#
# The test clips are static PQ luminance charts with neutral chroma, so any
# frame during playback carries the same known code values and the capture does
# not have to hit an exact timestamp.
set -uo pipefail

label=$1; class=$2; settle=$3; shift 3
[ "${1:-}" = "--" ] && shift

export XDG_RUNTIME_DIR=/run/user/1000
export WAYLAND_DISPLAY=wayland-0
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus

outdir="$HOME/plezy-dev/sdr"
mkdir -p "$outdir"
shot="$outdir/$label.png"
log="$outdir/$label.log"
rm -f "$shot"

# The panel is the only output, so refuse to measure a blanked screen: a
# screenshot of a DPMS-off panel is black and would read as "clips everything".
dpms=$(cat /sys/class/drm/card0-eDP-1/dpms 2>/dev/null)
[ "$dpms" = "On" ] || { echo "$label: ABORT - eDP-1 dpms is '$dpms', not On"; exit 1; }

kwin_script() {
  local path=$1
  for m in unloadScript loadScript; do
    gdbus call --session --dest org.kde.KWin --object-path /Scripting \
      --method org.kde.kwin.Scripting.$m "$path" >/dev/null 2>&1
  done
  gdbus call --session --dest org.kde.KWin --object-path /Scripting \
    --method org.kde.kwin.Scripting.start >/dev/null 2>&1
}

# Matched on a substring so the same helper drives plezy, mpv and kodi, whose
# resource classes are all different.
cat > /tmp/sdr-fs.js <<JS
for (const w of workspace.windowList()) {
  const c = (w.resourceClass || "").toLowerCase();
  if (c.includes("$class")) {
    w.minimized = false;
    workspace.activeWindow = w;
    w.fullScreen = true;
    if (!w.fullScreen && typeof workspace.slotWindowFullScreen === "function") {
      workspace.slotWindowFullScreen();
    }
    w.keepAbove = true;
    print("SDRGEO $label class=" + w.resourceClass +
          " geo=" + w.frameGeometry.width + "x" + w.frameGeometry.height +
          " fullscreen=" + w.fullScreen + " output=" + (w.output ? w.output.name : "?"));
  }
}
JS

echo "$label: launching: $*" | tee "$log"
setsid "$@" >>"$log" 2>&1 &
launch_pid=$!

sleep "$settle"
kwin_script /tmp/sdr-fs.js
sleep 3
# Second pass: KWin applies fullscreen asynchronously, so the first call's own
# read-back is stale. This one reports the geometry that is actually on screen.
kwin_script /tmp/sdr-fs.js
sleep 2

spectacle -b -n -f -o "$shot" >>"$log" 2>&1
for _ in $(seq 1 20); do [ -s "$shot" ] && break; sleep 0.5; done

journalctl --user -n 120 --no-pager 2>/dev/null | grep "SDRGEO $label" | tail -1 | sed 's/^.*SDRGEO/  geo:/'

if [ -s "$shot" ]; then
  echo "  shot: $(stat -c%s "$shot") bytes  $shot"
else
  echo "  shot: MISSING"
fi

kill -- -"$launch_pid" 2>/dev/null
pkill -f 'io.mpv.Mp[v]' 2>/dev/null
pkill -f 'tv.kodi.Kod[i]' 2>/dev/null
flatpak kill io.mpv.Mpv 2>/dev/null
flatpak kill tv.kodi.Kodi 2>/dev/null
sleep 2
exit 0
