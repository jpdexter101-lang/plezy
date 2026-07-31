#!/bin/bash
# Builds libmpv 0.41.0 into its own prefix, reusing the existing prefix's
# ffmpeg/libplacebo/shaderc.
#
# The point is a controlled comparison. Our runner drives libmpv 0.40's GL
# renderer through the render API, and its BT.2390 collapses the top of the range
# on a 1000-nit source. Standalone mpv 0.41 does not, but that observation came
# from a Vulkan/libplacebo build, so it confounded the mpv version with the GPU
# backend. Rebuilding *only* mpv, with byte-identical meson options from
# linux/packaging/build-libmpv.sh - GL enabled, Vulkan and Wayland disabled -
# leaves the version as the single difference.
#
# Reusing the dependencies matters as much as matching the options: pulling a
# newer ffmpeg or libplacebo in alongside would put the answer back out of reach.
set -euo pipefail

VERSION=0.41.0
PREFIX_OLD="$HOME/plezy-dev/libmpv-prefix"
PREFIX_NEW="$HOME/plezy-dev/libmpv41-prefix"
WORK=/tmp/mpv41

rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK"

curl -sL -o mpv.tar.gz "https://github.com/mpv-player/mpv/archive/refs/tags/v${VERSION}.tar.gz"
echo "SHA256=$(sha256sum mpv.tar.gz | cut -d' ' -f1)"
tar xzf mpv.tar.gz
cd "mpv-${VERSION}"

export PKG_CONFIG_PATH="$PREFIX_OLD/lib/pkgconfig"

meson setup build \
  --prefix="$PREFIX_NEW" \
  -Dlibmpv=true \
  -Dcplayer=false \
  -Dbuild-date=false \
  -Dlua=enabled \
  -Djavascript=enabled \
  -Dcplugins=disabled \
  -Dmanpage-build=disabled \
  -Djack=disabled \
  -Dvulkan=disabled \
  -Dd3d11=disabled \
  -Dgl=enabled \
  -Dvaapi=enabled \
  -Dvdpau=enabled \
  -Dalsa=enabled \
  -Dpulse=enabled \
  -Dpipewire=enabled \
  -Dwayland=disabled \
  -Dx11=enabled 2>&1 | tail -14

ninja -C build -j6 2>&1 | tail -4
ninja -C build install 2>&1 | tail -2

echo "installed:"
ls -la "$PREFIX_NEW/lib/"libmpv.so* | tail -3
strings "$PREFIX_NEW/lib/libmpv.so.2" | grep -oE '^mpv v?0\.[0-9]+\.[0-9]+' | sort -u | head -2
echo MPV41_BUILD_DONE
