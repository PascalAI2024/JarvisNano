#!/usr/bin/env bash
# smoke-build.sh — post-build sanity checks for the generated ESP-Claw image
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/esp-claw/application/edge_agent/build"
APP_BIN="$BUILD_DIR/edge_agent.bin"
STORAGE_BIN="$BUILD_DIR/storage.bin"
REPORT="$ROOT/.build_logs/smoke-build.txt"

log() { printf '\033[1;36m[smoke]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[smoke]\033[0m %s\n' "$*" >&2; exit 1; }

mkdir -p "$ROOT/.build_logs"

[ -f "$APP_BIN" ] || die "missing firmware image: $APP_BIN"
[ -f "$STORAGE_BIN" ] || die "missing FATFS image: $STORAGE_BIN"
[ -d "$ROOT/esp-claw/.git" ] || die "missing esp-claw checkout"

app_size="$(wc -c < "$APP_BIN" | tr -d ' ')"
storage_size="$(wc -c < "$STORAGE_BIN" | tr -d ' ')"
esp_claw_commit="$(git -C "$ROOT/esp-claw" rev-parse HEAD)"

grep -a -q "Native status LED on gpio" "$APP_BIN" ||
    die "edge_agent.bin does not contain native status LED log string"
grep -a -q "boot_status_led" "$STORAGE_BIN" ||
    die "storage.bin does not contain router rules"
grep -a -q "status_led.lua" "$STORAGE_BIN" ||
    die "storage.bin does not contain status_led.lua"
jq . "$ROOT/firmware/router_rules/router_rules.json" >/dev/null ||
    die "router_rules.json is not valid JSON"

{
    printf 'JarvisNano firmware smoke check\n'
    printf 'esp-claw commit: %s\n' "$esp_claw_commit"
    printf 'edge_agent.bin: %s bytes\n' "$app_size"
    printf 'storage.bin: %s bytes\n' "$storage_size"
    printf 'status LED string: present\n'
    printf 'router rules: present\n'
    printf 'status_led.lua: present\n'
} > "$REPORT"

log "✓ smoke checks passed"
log "report → $REPORT"
