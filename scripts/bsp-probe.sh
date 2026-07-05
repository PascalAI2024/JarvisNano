#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROBE_DIR="$ROOT/experiments/waveshare_bsp_probe"
IDF_IMAGE="${IDF_IMAGE:-espressif/idf:v5.5.4}"
BSP_PROBE_JOBS="${BSP_PROBE_JOBS:-1}"

usage() {
    cat <<'USAGE'
Usage:
  scripts/bsp-probe.sh build
  scripts/bsp-probe.sh flash -p /dev/cu.usbmodemXXXX
  scripts/bsp-probe.sh monitor -p /dev/cu.usbmodemXXXX
  scripts/bsp-probe.sh erase -p /dev/cu.usbmodemXXXX

The build uses espressif/idf:v5.5.4 in Docker. Flash/monitor use local
esptool/pyserial because macOS serial passthrough through Docker is unreliable.
USAGE
}

die() {
    printf '[bsp-probe] %s\n' "$*" >&2
    exit 1
}

parse_port() {
    local port=""
    while (($#)); do
        case "$1" in
            -p|--port)
                [[ $# -ge 2 ]] || die "missing value for $1"
                port="$2"
                shift 2
                ;;
            *)
                die "unknown option for serial command: $1"
                ;;
        esac
    done

    [[ -n "$port" ]] || die "serial command requires -p /dev/cu.usbmodemXXXX"
    printf '%s\n' "$port"
}

python_tools() {
    local venv="${JARVISNANO_TOOL_VENV:-/tmp/jarvisnano-esptool}"

    if [[ ! -x "$venv/bin/python" ]]; then
        python3 -m venv "$venv"
    fi

    if ! "$venv/bin/python" - <<'PY' >/dev/null 2>&1
import esptool
import serial
PY
    then
        "$venv/bin/python" -m pip install --quiet --upgrade pip esptool pyserial
    fi

    printf '%s\n' "$venv/bin/python"
}

cmd="${1:-build}"
shift || true

case "$cmd" in
    build)
        docker run --rm \
            -v "$PROBE_DIR:/project" \
            -w /project \
            -e BSP_PROBE_JOBS="$BSP_PROBE_JOBS" \
            "$IDF_IMAGE" \
            bash -lc 'set -e; pip install --quiet "idf-component-manager==2.4.10"; rm -rf build sdkconfig sdkconfig.old; idf.py set-target esp32s3; ninja -C build -j "$BSP_PROBE_JOBS"'
        ;;
    flash)
        port="$(parse_port "$@")"
        tool_py="$(python_tools)"
        [[ -f "$PROBE_DIR/build/flasher_args.json" ]] || die "build first: scripts/bsp-probe.sh build"
        "$tool_py" - "$PROBE_DIR" "$port" <<'PY'
import json
import os
import subprocess
import sys

probe_dir, port = sys.argv[1], sys.argv[2]
with open(os.path.join(probe_dir, "build", "flasher_args.json"), encoding="utf-8") as fh:
    manifest = json.load(fh)

cmd = [
    sys.executable,
    "-m",
    "esptool",
    "--chip",
    manifest["extra_esptool_args"].get("chip", "esp32s3"),
    "-p",
    port,
    "-b",
    os.environ.get("ESPTOOL_BAUD", "460800"),
    "--before",
    "default-reset",
    "--after",
    "hard-reset",
    "write-flash",
]
cmd.extend(arg.replace("_", "-") if arg.startswith("--flash_") else arg
           for arg in manifest["write_flash_args"])
for offset, rel_path in sorted(manifest["flash_files"].items(), key=lambda item: int(item[0], 16)):
    cmd.extend([offset, os.path.join(probe_dir, "build", rel_path)])

subprocess.check_call(cmd)
PY
        ;;
    monitor)
        port="$(parse_port "$@")"
        tool_py="$(python_tools)"
        "$tool_py" - "$port" <<'PY'
import os
import serial
import sys
import time

port = sys.argv[1]
duration = float(os.environ.get("BSP_PROBE_MONITOR_SECONDS", "0"))
deadline = time.time() + duration if duration > 0 else None

ser = serial.Serial(port, 115200, timeout=0.1)
ser.dtr = False
ser.rts = True
time.sleep(0.2)
ser.rts = False
time.sleep(0.2)

try:
    while deadline is None or time.time() < deadline:
        data = ser.read(4096)
        if data:
            sys.stdout.write(data.decode("utf-8", errors="replace"))
            sys.stdout.flush()
except KeyboardInterrupt:
    pass
finally:
    ser.close()
PY
        ;;
    erase)
        port="$(parse_port "$@")"
        tool_py="$(python_tools)"
        "$tool_py" -m esptool --chip esp32s3 -p "$port" --before default-reset --after hard-reset erase-flash
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage
        die "unknown command: $cmd"
        ;;
esac
