#!/bin/bash
# usage: deck-plezy-leg.sh <media-filename> <hdr:0|1> [tonemap:compositor|player]
#
# Launches the harness build inside the "plezy" distrobox, which is where libmpv
# lives: it was built against glibc 2.44 and links /usr/lib/libmujs.so by
# absolute path, neither of which the SteamOS host provides. The container shares
# the host PID namespace and Wayland socket, so host-side instrumentation still
# sees the process and the window.
set -uo pipefail
media=$1; hdr=$2; tonemap=${3:-}

env_lines="export PLEZY_HARNESS_MEDIA=\$HOME/plezy-dev/media/$media; export PLEZY_HARNESS_SECONDS=40;"
[ "$hdr" = "1" ] && env_lines="$env_lines export PLEZY_HARNESS_HDR=1;"
[ -n "$tonemap" ] && env_lines="$env_lines export PLEZY_HARNESS_TONEMAP=$tonemap;"

exec distrobox-enter -n plezy -- bash -c "
  export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 GDK_BACKEND=wayland
  export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
  export LD_LIBRARY_PATH=\$HOME/plezy-dev/libmpv-prefix/lib
  $env_lines
  cd \$HOME/plezy-dev/plezy/build/linux/x64/release/bundle
  exec ./plezy
"
