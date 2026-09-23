#!/bin/sh
# CrossMix-OS launcher for VECTOR-06C (v06x) on TrimUI Brick Pro.
# Everything is logged to v06x.log next to this script so startup failures
# (missing libs, SDL init errors, bad ROM path) can be diagnosed on device.

cd "$(dirname "$0")" || exit 1
LOG="$PWD/v06x.log"
{
    echo "== $(date) launch =="
    echo "PWD=$PWD"
    echo "ROM=$1"
} >> "$LOG"

# POSIX dot works under bash/dash/ash alike (bare `source` is a bashism).
. /mnt/SDCARD/System/usr/trimui/scripts/common_launcher.sh >> "$LOG" 2>&1

# Performance governor; exact values to be tuned after profiling on device.
cpufreq.sh performance 7 7 >> "$LOG" 2>&1

# Bundled Boost shared libraries live next to the binary.
export LD_LIBRARY_PATH="$PWD/lib:$LD_LIBRARY_PATH"

# Optional per-device overrides for debugging, e.g. a file "debug.env" with
#   SDL_VIDEODRIVER=dummy
#   SDL_AUDIODRIVER=dummy
[ -f "$PWD/debug.env" ] && . "$PWD/debug.env" >> "$LOG" 2>&1

ROM="$1"
case "$ROM" in
    *.fdd|*.FDD)
        ./v06x --fdd "$ROM" >> "$LOG" 2>&1
        ;;
    *)
        ./v06x --rom "$ROM" >> "$LOG" 2>&1
        ;;
esac
echo "v06x exit code: $?" >> "$LOG"
