#!/bin/sh
# One-command cross build & packaging of v06x for TrimUI Brick Pro.
#
#   ./make.sh           build the cross image if missing, then build+package
#   ./make.sh rebuild   force rebuilding the cross image first
#
# Result: platform/trimui/release/ - a tree mirroring the SD card layout;
# copy its CONTENTS to the root of the SD card (/mnt/SDCARD).
set -e
cd "$(dirname "$0")"

IMAGE=v06x-trimui-cross

# snap-installed docker usually needs sudo; a group-member docker does not
if docker info >/dev/null 2>&1; then
    DOCKER=docker
else
    DOCKER="sudo docker"
fi

if [ "$1" = "rebuild" ] || ! $DOCKER image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "== Building cross image (one-time, ~10-20 min) =="
    $DOCKER build -f Dockerfile.aarch64 -t "$IMAGE" .
fi

echo "== Building v06x inside the container =="
$DOCKER run --rm -v "$(cd ../.. && pwd):/builder" -w /builder/platform/trimui \
    "$IMAGE" ./build-trimui.sh

# the container writes as root; hand the tree back to the calling user
if [ -d release ] && [ "$(stat -c '%U' release)" != "$(id -un)" ]; then
    sudo chown -R "$(id -u):$(id -g)" build release
fi

echo
echo "Done. Copy the CONTENTS of release/ to the SD card root (/mnt/SDCARD)."
