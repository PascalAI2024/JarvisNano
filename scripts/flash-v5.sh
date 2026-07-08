#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# flash-v5.sh — flash JarvisRobot v5 to the Waveshare AMOLED-1.75.
#
# DIO flash mode is MANDATORY: the CO5300 QSPI panel and the flash controller
# interact, and the default QIO mode has caused boot failures with this firmware
# (hardware.md non-negotiable #4). DIO is enforced in TWO places so it cannot be
# flashed wrong:
#   1. sdkconfig.defaults   -> CONFIG_ESPTOOLPY_FLASHMODE_DIO=y (build header)
#   2. this script          -> preflight asserts the build's flash_args says dio
#
# Port is auto-detected as the first /dev/cu.usbmodem* (override with PORT=...).
#
# Usage:
#   scripts/flash-v5.sh                 # build (if needed) + flash + monitor
#   PORT=/dev/cu.usbmodem1101 scripts/flash-v5.sh
#   BAUD=921600 scripts/flash-v5.sh
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
BAUD="${BAUD:-460800}"

# --- port auto-detect ------------------------------------------------------
PORT="${PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -n1 || true)}"
if [ -z "${PORT}" ]; then
  echo "flash-v5: no /dev/cu.usbmodem* found. Plug in the board (or hold BOOT" >&2
  echo "          and replug for download mode), or set PORT=... explicitly." >&2
  exit 1
fi

command -v idf.py >/dev/null 2>&1 || {
  echo "flash-v5: idf.py not on PATH — run '. \$IDF_PATH/export.sh' first." >&2
  exit 1
}

# --- ensure a build exists -------------------------------------------------
if [ ! -f "${BUILD_DIR}/flash_args" ]; then
  echo "flash-v5: no build found — running idf.py build"
  ( cd "${PROJECT_DIR}" && idf.py set-target esp32s3 build )
fi

# --- DIO preflight: refuse to flash a QIO image ---------------------------
if grep -q -- '--flash_mode qio' "${BUILD_DIR}/flash_args" 2>/dev/null; then
  echo "flash-v5: REFUSING to flash — build/flash_args is QIO, not DIO." >&2
  echo "          Set CONFIG_ESPTOOLPY_FLASHMODE_DIO=y and rebuild." >&2
  exit 1
fi

echo "flash-v5: port=${PORT} baud=${BAUD} mode=DIO"
cd "${PROJECT_DIR}"
exec idf.py -p "${PORT}" -b "${BAUD}" flash monitor
