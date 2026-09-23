#!/bin/sh
# Build and package v06x for TrimUI Brick Pro / CrossMix-OS (AArch64).
#
# This script expects the cross toolchain (aarch64-linux-gnu-*) and the sysroot
# built by Dockerfile.aarch64 to be present. Typical usage:
#
#   cd platform/trimui
#   docker build -f Dockerfile.aarch64 -t v06x-trimui-cross .
#   docker run --rm -v "$(cd ../.. && pwd):/builder" -w /builder/platform/trimui \
#       v06x-trimui-cross ./build-trimui.sh
#
# It can also run directly on a host that has the toolchain + sysroot installed.
set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
BUILD_DIR="$SCRIPT_DIR/build"
RELEASE_DIR="$SCRIPT_DIR/release"
EMU_DIR="$RELEASE_DIR/Emus/VECTOR06C"
ROM_DIR="$RELEASE_DIR/Roms/VECTOR06C"
SYSROOT=${SYSROOT:-/opt/cross/aarch64-crossmix}
CROSS_STRIP=${CROSS_STRIP:-aarch64-linux-gnu-strip}

echo "== Configuring (TRIMUI=ON) =="
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake \
    -DCMAKE_TOOLCHAIN_FILE="$SCRIPT_DIR/trimui-aarch64.toolchain.cmake" \
    -DTRIMUI=ON \
    -DTRIMUI_SYSROOT="$SYSROOT" \
    -DSDL2_INCLUDE_DIR="$SYSROOT/include/SDL2" \
    -DSDL2_LIBRARY="$SYSROOT/lib/libSDL2.so" \
    -DCMAKE_BUILD_TYPE=Release \
    "$PROJECT_ROOT"

echo "== Building =="
make -j"$(nproc 2>/dev/null || echo 2)"

echo "== Stripping =="
"$CROSS_STRIP" v06x

echo "== Packaging into $RELEASE_DIR (mirrors the SD card layout) =="
rm -rf "$RELEASE_DIR"
mkdir -p "$EMU_DIR/lib" "$ROM_DIR"
cp v06x "$EMU_DIR/"
cp "$SCRIPT_DIR/package/launch.sh" "$EMU_DIR/"
cp "$SCRIPT_DIR/package/config.json" "$EMU_DIR/"
chmod +x "$EMU_DIR/launch.sh" "$EMU_DIR/v06x"

# Menu icon and background referenced by config.json.
mkdir -p "$RELEASE_DIR/Icons/Default/Emus" "$RELEASE_DIR/Backgrounds/Default"
cp "$SCRIPT_DIR/assets/VECTOR06C-icon.png" "$RELEASE_DIR/Icons/Default/Emus/VECTOR06C.png"
cp "$SCRIPT_DIR/assets/VECTOR06C-background.png" "$RELEASE_DIR/Backgrounds/Default/VECTOR06C.png"

# Bundle the Boost shared libraries the binary links against. Copy only the
# real versioned file, never the .so symlink: the SD card is FAT/exFAT and
# cannot store symlinks, and the real filename matches the ELF SONAME.
for lib in thread system filesystem program_options chrono; do
    real=$(find "$SYSROOT/lib" -maxdepth 1 -type f -name "libboost_${lib}.so.*" | head -n 1)
    if [ -n "$real" ]; then
        cp "$real" "$EMU_DIR/lib/"
    else
        echo "WARNING: libboost_${lib} not found in $SYSROOT/lib"
    fi
done

echo
echo "== Done. Release contents =="
ls -lR "$RELEASE_DIR"
echo
echo "Copy the CONTENTS of $RELEASE_DIR to the root of the SD card"
echo "(/mnt/SDCARD), so the device ends up with:"
echo "  /mnt/SDCARD/Emus/VECTOR06C/{v06x,launch.sh,config.json,lib/}"
echo "  /mnt/SDCARD/Roms/VECTOR06C/   <- put your .rom/.fdd files here"
echo "  /mnt/SDCARD/Icons/Default/Emus/VECTOR06C.png"
echo "  /mnt/SDCARD/Backgrounds/Default/VECTOR06C.png"
echo
echo "Verify on the Brick Pro:"
echo "  cd /mnt/SDCARD/Emus/VECTOR06C && file v06x && ldd ./v06x && readelf -d ./v06x"
