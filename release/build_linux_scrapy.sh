#!/bin/bash
# Native Linux build mirroring build_windows.sh (no dav1d/libusb, shared deps).
#
# Build dependencies (Debian/Ubuntu):
#   sudo apt-get install build-essential nasm pkg-config \
#     libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
#     libxfixes-dev libxrender-dev libxss-dev libxtst-dev libxkbcommon-dev \
#     libxkbcommon-x11-dev libwayland-dev wayland-protocols libgl1-mesa-dev \
#     libegl1-mesa-dev libdrm-dev libgbm-dev libasound2-dev libpulse-dev \
#     libdbus-1-dev libudev-dev libv4l-dev zlib1g-dev
#   (meson + ninja + cmake are also required, e.g. via pip)
#
# Output: release/work/build-linux-native/dist (portable, libs bundled in lib/).
set -ex
cd "$(dirname ${BASH_SOURCE[0]})"
. build_common
cd .. # root project dir

LINUX_BUILD_DIR="$WORK_DIR/build-linux-native"

app/deps/adb_linux.sh
app/deps/sdl.sh linux native shared
app/deps/ffmpeg.sh linux native shared

DEPS_INSTALL_DIR="$PWD/app/deps/work/install/linux-native-shared"
ADB_INSTALL_DIR="$PWD/app/deps/work/install/adb-linux"

# Prefer the bundled deps (listed first), but still allow system-only libs such
# as zlib (used for XAPK extraction) to be found via the default search path.
unset PKG_CONFIG_PATH
export PKG_CONFIG_LIBDIR="$DEPS_INSTALL_DIR/lib/pkgconfig:$(pkg-config --variable pc_path pkg-config)"

rm -rf "$LINUX_BUILD_DIR"
meson setup "$LINUX_BUILD_DIR" \
    -Dc_args="-I$DEPS_INSTALL_DIR/include" \
    -Dc_link_args="-L$DEPS_INSTALL_DIR/lib -Wl,-rpath,\$ORIGIN/lib -Wl,-rpath,$DEPS_INSTALL_DIR/lib" \
    --buildtype=release \
    -Dcompile_server=false -Dusb=false \
    -Dportable=true
ninja -C "$LINUX_BUILD_DIR"

# Assemble a portable dist directory
mkdir -p "$LINUX_BUILD_DIR/dist/lib"
cp "$LINUX_BUILD_DIR"/app/scrapy "$LINUX_BUILD_DIR/dist/"
cp app/data/scrcpy.png "$LINUX_BUILD_DIR/dist/" 2>/dev/null || true
cp app/data/disconnected.png "$LINUX_BUILD_DIR/dist/" 2>/dev/null || true
cp -r "$ADB_INSTALL_DIR"/. "$LINUX_BUILD_DIR/dist/" 2>/dev/null || true
cp -P "$DEPS_INSTALL_DIR"/lib/*.so* "$LINUX_BUILD_DIR/dist/lib/" 2>/dev/null || true
echo "LINUX BUILD DONE: $LINUX_BUILD_DIR/dist"
