#!/usr/bin/env bash
# Post-build sanity checks for the supported JarvisNano 1.75C image.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build"
MANIFEST="$BUILD_DIR/flasher_args.json"
REPORT="$ROOT/.build_logs/smoke-build.txt"

log() { printf '\033[1;36m[smoke-v5]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[smoke-v5]\033[0m %s\n' "$*" >&2; exit 1; }

[ -f "$MANIFEST" ] || die "missing $MANIFEST — run ./scripts/build-v5.sh"
[ -f "$ROOT/partitions_32MB.csv" ] || die "missing canonical 32 MB partition table"

mkdir -p "$ROOT/.build_logs"
python3 - "$BUILD_DIR" "$MANIFEST" "$REPORT" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

build = Path(sys.argv[1])
manifest_path = Path(sys.argv[2])
report_path = Path(sys.argv[3])
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
settings = manifest.get("flash_settings", {})
expected = {"flash_mode": "dio", "flash_size": "32MB", "flash_freq": "80m"}
if settings != expected:
    raise SystemExit(f"unsafe flash settings: expected {expected}, got {settings}")

files = manifest.get("flash_files", {})
required = {
    "0x0": "bootloader/bootloader.bin",
    "0x8000": "partition_table/partition-table.bin",
    "0x10000": "ota_data_initial.bin",
    "0x20000": "jarvisrobot_v5.bin",
    "0x420000": "emote_assets.bin",
    "0xa00000": "srmodels/srmodels.bin",
}
if files != required:
    raise SystemExit(f"flash manifest drift: expected {required}, got {files}")

rows = []
for offset, relative in required.items():
    path = build / relative
    if not path.is_file() or path.stat().st_size == 0:
        raise SystemExit(f"missing or empty flash artifact: {path}")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    rows.append((offset, relative, path.stat().st_size, digest))

partitions = {}
for raw in (build.parent / "partitions_32MB.csv").read_text().splitlines():
    if not raw.strip() or raw.lstrip().startswith("#"):
        continue
    name, _kind, _subtype, offset, size = (
        field.strip() for field in raw.split(",")
    )
    partitions[name] = (int(offset, 0), int(size, 0))
expected_partitions = {
    "otadata": (0x10000, 0x2000),
    "ota_0": (0x20000, 0x400000),
    "emote_assets": (0x420000, 0x5E0000),
    "model": (0xA00000, 0x200000),
    "ota_1": (0xC00000, 0x400000),
}
for name, expected_partition in expected_partitions.items():
    if partitions.get(name) != expected_partition:
        raise SystemExit(
            f"partition drift for {name}: expected {expected_partition}, "
            f"got {partitions.get(name)}"
        )
if any(partitions[name][0] >= 0x1000000 for name in ("ota_0", "ota_1")):
    raise SystemExit("application slot begins outside the ESP32-S3 mapping window")

app_size = (build / "jarvisrobot_v5.bin").stat().st_size
slot_size = min(partitions["ota_0"][1], partitions["ota_1"][1])
if app_size > slot_size:
    raise SystemExit(
        f"application exceeds {slot_size}-byte OTA slot: {app_size} bytes"
    )

lines = [
    "JarvisNano v5 smoke check",
    "board: esp32s3_touch_amoled_1_75c",
    "flash: DIO 80 MHz, 32 MB",
]
lines.extend(f"{offset} {name} {size} bytes sha256={digest}" for offset, name, size, digest in rows)
report_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
PY

python3 "$ROOT/scripts/patch-v5-managed.py" --check
python3 "$ROOT/scripts/sync-v5-sdkconfig.py" --check
python3 "$ROOT/scripts/check-pins.py"
log "smoke checks passed"
log "report: $REPORT"
