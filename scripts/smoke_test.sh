#!/usr/bin/env bash
# Hardware smoke test - run this on a machine with the camera attached.
#
# Prerequisites:
#   1. Build with the real SDK:  cmake -B build -DSONY_SDK_DIR=/path/to/CrSDK && cmake --build build
#   2. Camera: USB connected (or same Wi-Fi network), powered on,
#      and set to PC Remote mode (Menu > Network > PC Remote Function).
#
# Usage: scripts/smoke_test.sh [path/to/sonycam]
set -u

SONYCAM="${1:-build/sonycam}"
OUT="$(mktemp -d)"
STEP=0
FAILED=0

run() {
  STEP=$((STEP + 1))
  echo
  echo "--- [$STEP] $1"
  shift
  if "$@"; then
    echo "--- [$STEP] OK"
  else
    echo "--- [$STEP] FAILED: $*"
    FAILED=1
  fi
}

run "Daemon starts and camera connects" "$SONYCAM" status
run "List all properties" "$SONYCAM" props
run "Read ISO" "$SONYCAM" get iso
run "Set ISO to 800" "$SONYCAM" set iso 800
run "Set ISO back to auto" "$SONYCAM" set iso auto
run "Read aperture" "$SONYCAM" get aperture
run "Read shutter speed" "$SONYCAM" get shutter_speed
run "Read white balance" "$SONYCAM" get white_balance
run "Grab a live-view frame" "$SONYCAM" liveview "$OUT/frame.jpg"
run "Live-view frame is a JPEG" file "$OUT/frame.jpg"
run "Capture a photo" "$SONYCAM" capture --dir "$OUT"
run "Stop daemon" "$SONYCAM" daemon stop

echo
if [ "$FAILED" -eq 0 ]; then
  echo "SMOKE TEST PASSED - output in $OUT"
else
  echo "SMOKE TEST HAD FAILURES - please send the full output above"
fi
exit "$FAILED"
