#!/usr/bin/env bash
# flash.sh — flash the current esp-claw build to an ESP32-S3 board over USB-C
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/esp-claw/application/edge_agent/build"
DEFAULT_PORT="${PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)}"
FLASHER_ARGS="$BUILD_DIR/flasher_args.json"
ESPTOOL_VENV="$ROOT/.build_tools/esptool"
BEFORE_RESET="${BEFORE_RESET:-default-reset}"
# USB-Serial/JTAG ESP32-S3 boards can sample GPIO0 incorrectly after an RTS
# hard reset and fall back into ROM download mode. Watchdog reset avoids host
# modem-control line state after flashing while still starting the app.
AFTER_RESET="${AFTER_RESET:-watchdog-reset}"
WRITE_AFTER_RESET="$AFTER_RESET"
if [ "$AFTER_RESET" = "watchdog-reset" ]; then
    WRITE_AFTER_RESET="no-reset"
fi

log() { printf '\033[1;36m[flash]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[flash]\033[0m %s\n' "$*" >&2; exit 1; }

[ -f "$BUILD_DIR/edge_agent.bin" ] || die "no build at $BUILD_DIR — run \`./scripts/bootstrap.sh build\` first"
[ -f "$FLASHER_ARGS" ] || die "missing $FLASHER_ARGS — build output is incomplete"
[ -n "$DEFAULT_PORT" ] || die "no /dev/cu.usbmodem* found — plug in the ESP32-S3 board (hold BOOT if needed) or set PORT=..."
log "port=$DEFAULT_PORT"

if python3 -c "import esptool" 2>/dev/null; then
    ESPTOOL_PY=(python3 -m esptool)
else
    if [ ! -x "$ESPTOOL_VENV/bin/python" ]; then
        log "creating local esptool venv at $ESPTOOL_VENV"
        python3 -m venv "$ESPTOOL_VENV"
    fi
    if ! "$ESPTOOL_VENV/bin/python" -c "import esptool" 2>/dev/null; then
        log "installing esptool into local venv"
        "$ESPTOOL_VENV/bin/python" -m pip install --quiet --upgrade pip esptool
    fi
    ESPTOOL_PY=("$ESPTOOL_VENV/bin/python" -m esptool)
fi

cd "$BUILD_DIR"

if [ "${ERASE_NVS:-0}" = "1" ]; then
    log "ERASE_NVS=1 → erasing NVS config partition at 0x9000..0xefff"
    "${ESPTOOL_PY[@]}" --chip esp32s3 -p "$DEFAULT_PORT" -b 460800 \
        --before "$BEFORE_RESET" --after no-reset erase-region 0x9000 0x6000
fi

if [ "${STORAGE:-0}" = "1" ]; then
    log "STORAGE=1 → including FATFS image (will wipe saved Wi-Fi/LLM config)"
else
    log "preserving FATFS storage (Wi-Fi creds + LLM config survive). pass STORAGE=1 to wipe."
fi

flash_argv=()
while IFS= read -r arg; do
    flash_argv+=("$arg")
done < <(
    STORAGE="${STORAGE:-0}" python3 - <<'PY'
import json
import os
from pathlib import Path

args = json.loads(Path("flasher_args.json").read_text())
settings = args.get("flash_settings", {})
for key in ("flash_mode", "flash_size", "flash_freq"):
    value = settings.get(key)
    if value:
        print("--" + key.replace("_", "-"))
        print(value)

include_storage = os.environ.get("STORAGE") == "1"
for offset, filename in sorted(args["flash_files"].items(), key=lambda item: int(item[0], 16)):
    if Path(filename).name == "storage.bin" and not include_storage:
        continue
    print(offset)
    print(filename)
PY
)

log "flashing from build metadata in flasher_args.json"
"${ESPTOOL_PY[@]}" --chip esp32s3 -p "$DEFAULT_PORT" -b 460800 \
    --before "$BEFORE_RESET" --after "$WRITE_AFTER_RESET" write-flash \
    "${flash_argv[@]}"

if [ "$AFTER_RESET" = "watchdog-reset" ]; then
    log "starting app with watchdog reset"
    if ! "${ESPTOOL_PY[@]}" --chip esp32s3 -p "$DEFAULT_PORT" -b 460800 \
        --before no-reset --after watchdog-reset flash-id >/dev/null; then
        log "watchdog reset command returned nonzero after USB re-enumeration; flash was already verified"
    fi
fi

log "✓ flashed with after-reset=$AFTER_RESET. Watch USB serial for board/IP/AP state before assuming the old address is still valid."
log "   to watch boot without toggling reset lines:  ./scripts/usb-monitor.py --port $DEFAULT_PORT"
