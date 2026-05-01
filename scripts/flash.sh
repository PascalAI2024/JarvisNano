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
log "flashing…"
python3 -m esptool --chip esp32s3 -p "$DEFAULT_PORT" -b 460800 \
    --before default_reset --after hard_reset write_flash \
    --flash_mode dio --flash_size 8MB --flash_freq 80m \
    0x0      bootloader/bootloader.bin \
    0x8000   partition_table/partition-table.bin \
    0xf000   ota_data_initial.bin \
    0x20000  edge_agent.bin \
    0x620000 storage.bin

log "✓ flashed. The device will boot, then broadcast SSID esp-claw-XXXXXX (open) at 192.168.4.1"
log "   to watch boot:  screen $DEFAULT_PORT 115200    (Ctrl-A K to quit)"
