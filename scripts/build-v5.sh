#!/usr/bin/env bash
# Build the plain-ESP-IDF JarvisRobot v5 image reproducibly in the pinned image.
#
# Environment flags:
#   FAST=1         Skip pip install, gen-bmgr-config, and double reconfigure when
#                  build/build.ninja exists and boards/ + sdkconfig.defaults hash
#                  matches .build_logs/.cfg_hash (incremental build shortcut).
#   RECONFIGURE=1  Force a full reconfigure (disables FAST=1).
set -euo pipefail
# Git Bash on Windows rewrites "/project" into "C:/Program Files/Git/project"
# before docker sees it; this stops that and is inert everywhere else.
export MSYS_NO_PATHCONV=1

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${IDF_DOCKER_IMAGE:-espressif/idf:v5.5.4}"
# The release build has one target. Original-1.75 sources remain as a hardware
# reference, but its 16 MB partition/flash contract is not release-gated.
BOARD="${BOARD_NAME:-esp32s3_touch_amoled_1_75c}"
RECONFIGURE="${RECONFIGURE:-0}"
FAST="${FAST:-0}"

log() { printf '\033[1;36m[build-v5]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[build-v5]\033[0m %s\n' "$*" >&2; exit 1; }
[ "$BOARD" = "esp32s3_touch_amoled_1_75c" ] ||
    die "unsupported release target '$BOARD'; build-v5 currently supports only esp32s3_touch_amoled_1_75c"

command -v docker >/dev/null 2>&1 || die "Docker is required for the pinned ESP-IDF build"
docker info >/dev/null 2>&1 || die "Docker is not running"

sha256_stream() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum | awk '{print $1}'
    else
        shasum -a 256 | awk '{print $1}'
    fi
}

compute_cfg_hash() {
    {
        printf 'board=%s\nimage=%s\n' "$BOARD" "$IMAGE"
        for f in "$ROOT"/sdkconfig.defaults "$ROOT"/sdkconfig.defaults.*; do
            [ -f "$f" ] || continue
            printf 'FILE:%s\n' "${f#"$ROOT"/}"
            cat "$f"
            printf '\n'
        done
        if [ -d "$ROOT/boards" ]; then
            while IFS= read -r f; do
                [ -f "$f" ] || continue
                printf 'FILE:%s\n' "${f#"$ROOT"/}"
                cat "$f"
                printf '\n'
            done < <(find "$ROOT/boards" -type f | LC_ALL=C sort)
        fi
    } | sha256_stream
}

CFG_HASH_FILE="$ROOT/.build_logs/.cfg_hash"
CUR_CFG_HASH="$(compute_cfg_hash)"
USE_FAST=0
if [ "$FAST" = "1" ] && [ "$RECONFIGURE" != "1" ] &&
   [ -f "$ROOT/build/build.ninja" ] && [ -f "$ROOT/sdkconfig" ] &&
   [ -f "$CFG_HASH_FILE" ] && [ "$(cat "$CFG_HASH_FILE" 2>/dev/null || true)" = "$CUR_CFG_HASH" ]; then
    USE_FAST=1
fi

if [ "$USE_FAST" = "1" ]; then
    log "image=$IMAGE board=$BOARD mode=FAST (incremental; config hash unchanged)"
    docker run --rm \
        -e "BOARD_NAME=$BOARD" \
        -v "$ROOT:/project" \
        -w /project \
        "$IMAGE" \
        bash -lc '
set -euo pipefail
idf.py build
'
else
    if [ "$FAST" = "1" ]; then
        log "image=$IMAGE board=$BOARD mode=FULL (FAST requested but build.ninja or config hash changed)"
    else
        log "image=$IMAGE board=$BOARD mode=FULL"
    fi
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
    mkdir -p "$ROOT/.build_logs"
    printf '%s\n' "$CUR_CFG_HASH" > "$CFG_HASH_FILE"
fi

BIN="$ROOT/build/jarvisrobot_v5.bin"
ELF="$ROOT/build/jarvisrobot_v5.elf"
test -f "$BIN" || die "build completed without jarvisrobot_v5.bin"

# Pick a real Python 3 interpreter on the host (avoiding Windows Store python3 stub).
PY=""
for cand in python3 python; do
    if command -v "$cand" >/dev/null 2>&1 &&
       "$cand" -c "import sys; sys.exit(0 if sys.version_info[0] >= 3 else 1)" >/dev/null 2>&1; then
        PY="$cand"
        break
    fi
done

GIT_SHORT="$( (cd "$ROOT" && git rev-parse --short HEAD 2>/dev/null) || echo nogit )"

if [ -n "$PY" ]; then
    (cd "$ROOT" && "$PY" - "." "build/jarvisrobot_v5.bin" "build/jarvisrobot_v5.elf" "$GIT_SHORT") <<'PYEOF'
import datetime as dt
import hashlib
import json
import os
import shutil
import sys

root, bin_path, elf_path, git_short = sys.argv[1:5]
ota_slot = 4194304  # 4 MiB OTA slot
bin_bytes = os.path.getsize(bin_path)
bin_kib = bin_bytes / 1024.0
pct = (100.0 * bin_bytes) / ota_slot
free_kib = (ota_slot - bin_bytes) / 1024.0

print(
    f"\033[1;36m[build-v5]\033[0m binary: {bin_bytes:,} B ({bin_kib:.1f} KiB) "
    f"- {pct:.1f}% of 4096 KiB OTA slot ({free_kib:.1f} KiB free headroom)"
)

if os.path.isfile(elf_path):
    h = hashlib.sha256()
    with open(elf_path, "rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    sha16 = h.hexdigest()[:16]
    elf_dir = os.path.join(root, ".build_logs", "elfs")
    os.makedirs(elf_dir, exist_ok=True)
    archive_name = f"{sha16}-{git_short}.elf"
    archive_path = os.path.join(elf_dir, archive_name)
    shutil.copy2(elf_path, archive_path)
    os.utime(archive_path, None)

    # Prune oldest archived ELFs beyond 20 files.
    all_elfs = [
        os.path.join(elf_dir, f)
        for f in os.listdir(elf_dir)
        if f.endswith(".elf") and os.path.isfile(os.path.join(elf_dir, f))
    ]
    all_elfs.sort(key=lambda p: os.path.getmtime(p), reverse=True)
    for stale in all_elfs[20:]:
        try:
            os.unlink(stale)
        except OSError:
            pass
    kept = all_elfs[:20]

    idx_path = os.path.join(elf_dir, "index.json")
    prev_map = {}
    if os.path.isfile(idx_path):
        try:
            with open(idx_path, encoding="utf-8") as fh:
                raw = json.loads(fh.read())
            for item in (raw if isinstance(raw, list) else raw.get("elfs", [])):
                if isinstance(item, dict) and item.get("file"):
                    prev_map[item["file"]] = item
        except Exception:
            pass

    now_iso = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    prev_map[archive_name] = {
        "sha16": sha16,
        "file": archive_name,
        "git": git_short,
        "built_at": now_iso,
        "elf_bytes": os.path.getsize(archive_path),
        "bin_bytes": bin_bytes,
    }
    kept_names = [os.path.basename(p) for p in kept]
    entries = []
    for fname in kept_names:
        if fname in prev_map:
            entries.append(prev_map[fname])
        else:
            stem = fname[:-4]
            parts = stem.split("-", 1)
            entries.append({
                "sha16": parts[0],
                "file": fname,
                "git": parts[1] if len(parts) > 1 else "unknown",
                "elf_bytes": os.path.getsize(os.path.join(elf_dir, fname)),
            })
    with open(idx_path, "w", encoding="utf-8") as fh:
        json.dump({"elfs": entries}, fh, indent=2)
        fh.write("\n")
    print(
        f"\033[1;36m[build-v5]\033[0m archived ELF: .build_logs/elfs/{archive_name} "
        f"(sha16={sha16}, {len(kept)}/20 kept)"
    )
PYEOF
else
    BIN_BYTES="$(wc -c < "$BIN" | tr -d ' ')"
    log "binary: ${BIN_BYTES} B (4194304 B OTA slot)"
fi

log "build complete: $BIN"
