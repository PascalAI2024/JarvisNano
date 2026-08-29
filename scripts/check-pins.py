#!/usr/bin/env python3
"""Fail when documented tool/component pins drift from executable sources."""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    pins: dict[str, str] = {}
    for line in (ROOT / "tools" / "idf-pins.txt").read_text().splitlines():
        match = re.match(r"^([A-Za-z0-9_./-]+)\s+==\s+([^\s#]+)", line)
        if match:
            pins[match.group(1)] = match.group(2).removeprefix("v")

    expected: dict[str, str] = {}
    manifest = (ROOT / "main" / "idf_component.yml").read_text()
    for name, version in re.findall(
        r'^  ([A-Za-z0-9_./-]+):\s+"==([^\"]+)"$', manifest, re.MULTILINE
    ):
        expected[name] = version

    build = (ROOT / "scripts" / "build-v5.sh").read_text()
    idf = re.search(r"espressif/idf:v([0-9.]+)", build)
    component_manager = re.search(r'idf-component-manager==([0-9.]+)', build)
    board_assist = re.search(r'esp-bmgr-assist==([0-9.]+)', build)
    if not (idf and component_manager and board_assist):
        raise SystemExit("could not resolve executable tool pins from build-v5.sh")
    expected.update({
        "esp-idf": idf.group(1),
        "idf-component-manager": component_manager.group(1),
        "esp-bmgr-assist": board_assist.group(1),
    })

    if pins != expected:
        missing = sorted(set(expected) - set(pins))
        extra = sorted(set(pins) - set(expected))
        changed = sorted(
            name for name in set(pins) & set(expected)
            if pins[name] != expected[name]
        )
        details = []
        if missing:
            details.append("missing=" + ",".join(missing))
        if extra:
            details.append("extra=" + ",".join(extra))
        if changed:
            details.append(
                "changed=" + ",".join(
                    f"{name}:{pins[name]}!={expected[name]}" for name in changed
                )
            )
        raise SystemExit("tool pin drift: " + " ".join(details))

    print(f"tool pins synchronized ({len(pins)} entries)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
