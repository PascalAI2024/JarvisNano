#!/usr/bin/env python3
"""Generate the exact license bundle for the resolved ESP-IDF components."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / "dependencies.lock"
MANAGED = ROOT / "managed_components"


def resolved_versions(text: str) -> dict[str, str]:
    versions: dict[str, str] = {}
    current: str | None = None
    for line in text.splitlines():
        match = re.match(r"^  ([^ ].*):$", line)
        if match:
            current = match.group(1)
            continue
        version = re.match(r"^    version: ['\"]?([^'\"]+)['\"]?$", line)
        if current and version:
            versions[current] = version.group(1)
    return versions


def license_files(component_dir: Path) -> list[Path]:
    pattern = re.compile(
        r"^(?:license|licence|copying|notice)(?:\.[A-Za-z0-9_-]+)?$",
        re.IGNORECASE,
    )
    return sorted(
        path for path in component_dir.iterdir()
        if path.is_file() and pattern.fullmatch(path.name)
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / ".build_logs" / "THIRD_PARTY_NOTICES.txt",
    )
    args = parser.parse_args()

    if not LOCK.is_file() or not MANAGED.is_dir():
        raise SystemExit("run ./scripts/build-v5.sh before generating notices")

    versions = resolved_versions(LOCK.read_text(encoding="utf-8"))
    fallback_manifest_path = ROOT / "third_party" / "licenses" / "manifest.json"
    fallback_manifest = json.loads(
        fallback_manifest_path.read_text(encoding="utf-8")
    )
    sections: list[str] = []
    missing: list[str] = []
    for name, version in sorted(versions.items()):
        if name == "idf":
            continue
        component_dir = MANAGED / name.replace("/", "__")
        files = license_files(component_dir) if component_dir.is_dir() else []
        upstream_url = None
        if not files:
            key = f"{name}@{version}"
            entry = fallback_manifest.get(key)
            if isinstance(entry, dict):
                fallback_path = ROOT / str(entry.get("path", ""))
                if fallback_path.is_file():
                    expected_digest = str(entry.get("sha256", ""))
                    actual_digest = hashlib.sha256(
                        fallback_path.read_bytes()
                    ).hexdigest()
                    if actual_digest != expected_digest:
                        raise SystemExit(
                            f"vendored license hash drift for {key}: "
                            f"{actual_digest} != {expected_digest}"
                        )
                    files = [fallback_path]
                    upstream_url = str(entry.get("upstream_url", ""))
        if not files:
            missing.append(f"{name}@{version}")
            continue
        for license_path in files:
            body = license_path.read_text(
                encoding="utf-8", errors="replace"
            ).rstrip()
            if len(body) < 100 or not re.search(
                r"copyright|permission|apache|license", body, re.IGNORECASE
            ):
                raise SystemExit(f"invalid license text: {license_path}")
            digest = hashlib.sha256(body.encode("utf-8")).hexdigest()
            provenance = (
                f"UPSTREAM: {upstream_url}\\n" if upstream_url else ""
            )
            sections.append(
                f"COMPONENT: {name}\\nVERSION: {version}\\n"
                f"SOURCE: {license_path.relative_to(ROOT)}\\n{provenance}"
                f"SHA256: {digest}\\n\\n{body}"
            )

    if missing:
        raise SystemExit("missing component license text: " + ", ".join(missing))

    header = (
        "JarvisNano third-party notices\n"
        "Generated from dependencies.lock and managed_components after the final build.\n"
        "ESP-IDF itself is Apache-2.0; include the matching ESP-IDF release notices "
        "with binary distributions.\n\n"
    )
    output = args.output if args.output.is_absolute() else ROOT / args.output
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(header + "\n\n".join(sections) + "\n", encoding="utf-8")
    print(f"wrote {output} ({len(sections)} license sections)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
