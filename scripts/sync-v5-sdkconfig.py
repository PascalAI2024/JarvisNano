#!/usr/bin/env python3
"""Synchronize esp_board_manager-owned symbols into the generated sdkconfig.

The board generator emits components/gen_bmgr_codes/board_manager.defaults only
after IDF has already created sdkconfig. Kconfig therefore preserves stale
"not set" values and every later action warns, despite the generated defaults
being correct. This small, idempotent bridge applies only CONFIG_ESP_BOARD_*
symbols; all project-owned settings remain sourced from sdkconfig.defaults.
"""

from __future__ import annotations

import argparse
import re
import os
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SDKCONFIG = ROOT / "sdkconfig"
BOARD_DEFAULTS = ROOT / "components" / "gen_bmgr_codes" / "board_manager.defaults"
ASSIGN_RE = re.compile(r"^(CONFIG_ESP_BOARD_[A-Z0-9_]+)=(.*)$")
UNSET_RE = re.compile(r"^# (CONFIG_ESP_BOARD_[A-Z0-9_]+) is not set$")


def desired_lines() -> dict[str, str]:
    if not BOARD_DEFAULTS.is_file():
        raise SystemExit(f"missing generated board defaults: {BOARD_DEFAULTS}")
    desired: dict[str, str] = {}
    for line in BOARD_DEFAULTS.read_text(encoding="utf-8").splitlines():
        match = ASSIGN_RE.match(line) or UNSET_RE.match(line)
        if match:
            desired[match.group(1)] = line
    if not desired:
        raise SystemExit("generated board defaults contain no CONFIG_ESP_BOARD_* symbols")
    return desired


def synchronized_text(current: str, desired: dict[str, str]) -> str:
    seen: set[str] = set()
    output: list[str] = []
    for line in current.splitlines():
        match = ASSIGN_RE.match(line) or UNSET_RE.match(line)
        if match:
            key = match.group(1)
            if key in desired:
                if key not in seen:
                    output.append(desired[key])
                    seen.add(key)
            else:
                # All CONFIG_ESP_BOARD_* selections are generator-owned. A
                # symbol that disappeared (for example, a deprecated device
                # type) must not remain enabled from an older board shape.
                output.append(f"# {key} is not set")
            continue
        output.append(line)
    missing = [desired[key] for key in sorted(desired) if key not in seen]
    if missing:
        output.extend(["", "# esp_board_manager generated selections"])
        output.extend(missing)
    return "\n".join(output) + "\n"



def validate_runtime_config(text: str) -> None:
    board = os.environ.get("BOARD_NAME", "")
    if board != "esp32s3_touch_amoled_1_75c":
        return
    required = (
        "CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y",
        'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_32MB.csv"',
        "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y",
        "CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=y",
        "CONFIG_ESP_WS_CLIENT_TX_LOCK_TIMEOUT_MS=20",
    )
    missing = [line for line in required if line not in text]
    if missing:
        joined = ", ".join(missing)
        raise SystemExit(
            "1.75C runtime sdkconfig is stale; run "
            f"RECONFIGURE=1 scripts/build-v5.sh (missing: {joined})"
        )

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    if not SDKCONFIG.is_file():
        raise SystemExit(f"missing sdkconfig: {SDKCONFIG}")
    current = SDKCONFIG.read_text(encoding="utf-8")
    updated = synchronized_text(current, desired_lines())
    validate_runtime_config(updated)
    if args.check:
        if current != updated:
            raise SystemExit("sdkconfig is not synchronized with generated board defaults")
        print("v5 sdkconfig: board-manager symbols synchronized")
        return 0
    if current != updated:
        SDKCONFIG.write_text(updated, encoding="utf-8")
        print("v5 sdkconfig: applied generated board-manager symbols")
    else:
        print("v5 sdkconfig: board-manager symbols already synchronized")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
