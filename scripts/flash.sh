#!/usr/bin/env bash
# flash.sh — flash esp-claw firmware to a XIAO ESP32-S3 Sense over USB-C
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/esp-claw/application/edge_agent/build"
DEFAULT_PORT="${PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)}"

log() { printf '\033[1;36m[flash]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[flash]\033[0m %s\n' "$*" >&2; exit 1; }

[ -f "$BUILD_DIR/edge_agent.bin" ] || die "no build at $BUILD_DIR — run \`./scripts/bootstrap.sh build\` first"
[ -n "$DEFAULT_PORT" ] || die "no /dev/cu.usbmodem* found — plug in the XIAO (hold BOOT if needed) or set PORT=..."
log "port=$DEFAULT_PORT"

if ! python3 -c "import esptool" 2>/dev/null; then
    log "installing esptool to user site"
    pip3 install --user --quiet esptool
fi

cd "$BUILD_DIR"

# By default we DO NOT reflash the FATFS storage partition (0x620000),
# because that's where saved Wi-Fi credentials, LLM config, etc. live.
# Reflashing it on every iteration wipes user provisioning and forces
# the wizard to be re-run. Pass STORAGE=1 to explicitly include it
# (e.g. on first install, or after a partition layout change).
flash_args=(
    0x0      bootloader/bootloader.bin
    0x8000   partition_table/partition-table.bin
    0xf000   ota_data_initial.bin
    0x20000  edge_agent.bin
)
if [ "${STORAGE:-0}" = "1" ]; then
    log "STORAGE=1 → including FATFS image (will wipe saved Wi-Fi/LLM config)"
    flash_args+=(0x620000 storage.bin)
else
    log "preserving FATFS storage (Wi-Fi creds + LLM config survive). pass STORAGE=1 to wipe."
fi

log "flashing…"
python3 -m esptool --chip esp32s3 -p "$DEFAULT_PORT" -b 460800 \
    --before default_reset --after hard_reset write_flash \
    --flash_mode dio --flash_size 8MB --flash_freq 80m \
    "${flash_args[@]}"

log "✓ flashed. The device will boot, then broadcast SSID esp-claw-XXXXXX (open) at 192.168.4.1"
log "   to watch boot:  screen $DEFAULT_PORT 115200    (Ctrl-A K to quit)"
