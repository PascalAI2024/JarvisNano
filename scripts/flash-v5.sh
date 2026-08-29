#!/usr/bin/env bash
# Build and safely flash JarvisNano v5; 1.75C is the default board target.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build"
PORT="${PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -n1 || true)}"
BAUD="${BAUD:-460800}"
ESPTOOL_VENV="$ROOT/.build_tools/esptool"

log() { printf '\033[1;36m[flash-v5]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[flash-v5]\033[0m %s\n' "$*" >&2; exit 1; }

if [ "${NO_BUILD:-0}" != "1" ]; then
    "$ROOT/scripts/build-v5.sh"
fi

test -n "$PORT" || die "no /dev/cu.usbmodem* found; attach the board or set PORT"
test -f "$BUILD_DIR/flasher_args.json" || die "missing build/flasher_args.json"
test -f "$BUILD_DIR/jarvisrobot_v5.bin" || die "missing v5 app image"

python3 - "$BUILD_DIR/flasher_args.json" <<'PY'
import json
import pathlib
import sys

manifest = json.loads(pathlib.Path(sys.argv[1]).read_text())
mode = manifest.get("flash_settings", {}).get("flash_mode")
if mode != "dio":
    raise SystemExit(f"flash-v5: refusing image with flash_mode={mode!r}; DIO is mandatory")
PY

if python3 - 2>/dev/null <<'PY'
import sys
import esptool
major = int(getattr(esptool, "__version__", "0").split(".")[0] or 0)
sys.exit(0 if major >= 5 else 1)
PY
then
    ESPTOOL=(python3 -m esptool)
elif command -v esptool >/dev/null 2>&1; then
    ESPTOOL=(esptool)
elif command -v esptool.py >/dev/null 2>&1; then
    ESPTOOL=(esptool.py)
else
    if [ ! -x "$ESPTOOL_VENV/bin/python" ]; then
        log "creating local esptool environment"
        python3 -m venv "$ESPTOOL_VENV"
    fi
    if ! "$ESPTOOL_VENV/bin/python" -c 'import esptool' 2>/dev/null; then
        "$ESPTOOL_VENV/bin/python" -m pip install --quiet --upgrade pip esptool
    fi
    ESPTOOL=("$ESPTOOL_VENV/bin/python" -m esptool)
fi

cd "$BUILD_DIR"

if [ "${ERASE_NVS:-0}" = "1" ]; then
    log "erasing NVS at 0x9000..0xefff"
    "${ESPTOOL[@]}" --chip esp32s3 -p "$PORT" -b 115200 \
        --before default-reset --after no-reset erase-region 0x9000 0x6000
fi

flash_argv=()
while IFS= read -r arg; do
    flash_argv+=("$arg")
done < <(python3 - <<'PY'
import json
from pathlib import Path

manifest = json.loads(Path("flasher_args.json").read_text())
settings = manifest["flash_settings"]
for key in ("flash_mode", "flash_size", "flash_freq"):
    print("--" + key.replace("_", "-"))
    print(settings[key])
for offset, filename in sorted(
    manifest["flash_files"].items(), key=lambda item: int(item[0], 16)
):
    print(offset)
    print(filename)
PY
)

log "port=$PORT baud=$BAUD mode=DIO (NVS preserved)"
"${ESPTOOL[@]}" --chip esp32s3 -p "$PORT" -b "$BAUD" \
    --connect-attempts 25 --before default-reset --after no-reset \
    write-flash "${flash_argv[@]}"

log "starting application with watchdog reset"
if ! "${ESPTOOL[@]}" --chip esp32s3 -p "$PORT" -b 115200 \
    --before no-reset --after watchdog-reset flash-id >/dev/null; then
    log "reset command raced USB re-enumeration; flash itself was already verified"
fi

log "flash complete; monitor with: ./scripts/usb-monitor.py --port $PORT"
