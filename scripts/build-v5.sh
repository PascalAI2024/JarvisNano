#!/usr/bin/env bash
# Build the plain-ESP-IDF JarvisRobot v5 image reproducibly in the pinned image.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${IDF_DOCKER_IMAGE:-espressif/idf:v5.5.4}"
# Default is the 1.75C (the live device since 2026-08). The original 1.75 stays
# buildable with BOARD_NAME=esp32s3_touch_amoled_1_75 — the two differ in LCD/touch
# reset and MCLK pins, so a mismatched image black-screens (reference/board-175c.md).
BOARD="${BOARD_NAME:-esp32s3_touch_amoled_1_75c}"
RECONFIGURE="${RECONFIGURE:-0}"

log() { printf '\033[1;36m[build-v5]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[build-v5]\033[0m %s\n' "$*" >&2; exit 1; }

command -v docker >/dev/null 2>&1 || die "Docker is required for the pinned ESP-IDF build"
docker info >/dev/null 2>&1 || die "Docker is not running"

log "image=$IMAGE board=$BOARD"
docker run --rm \
    -e "BOARD_NAME=$BOARD" \
    -e "RECONFIGURE=$RECONFIGURE" \
    -v "$ROOT:/project" \
    -w /project \
    "$IMAGE" \
    bash -lc '
set -euo pipefail
python -m pip install --quiet "idf-component-manager==2.4.10" "esp-bmgr-assist==0.5.0"
if [ ! -f sdkconfig ] || [ "$RECONFIGURE" = "1" ]; then
    idf.py set-target esp32s3
fi
idf.py gen-bmgr-config -c ./boards -b "$BOARD_NAME"
# gen-bmgr-config removes sdkconfig so its board defaults cannot pollute the
# first configuration pass. Recreate the project config before the bridge
# synchronizes the generated CONFIG_ESP_BOARD_* symbols.
idf.py reconfigure
python3 scripts/sync-v5-sdkconfig.py
idf.py reconfigure
python3 scripts/patch-v5-managed.py
idf.py build
python3 scripts/patch-v5-managed.py --check
python3 scripts/sync-v5-sdkconfig.py --check
'

test -f "$ROOT/build/jarvisrobot_v5.bin" || die "build completed without jarvisrobot_v5.bin"
log "build complete: $ROOT/build/jarvisrobot_v5.bin"
