#!/usr/bin/env python3
"""Scan tracked and untracked public-candidate files for secrets and identifiers."""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXCLUDED = {
    "components/jr_memory/src/jr_memory_guard.c",
    "components/jr_memory/tests/test_guard.c",
    "scripts/check_secrets.py",
}

PATTERNS = {
    "OpenAI-style key": r"sk-[A-Za-z0-9_-]{32,}",
    "Anthropic key": r"sk-ant-[A-Za-z0-9_-]{32,}",
    "OpenAI project key": r"sk-proj-[A-Za-z0-9_-]{32,}",
    "Slack token": r"xox[baprs]-[A-Za-z0-9-]{20,}",
    "GitHub token": r"(?:ghp_|gho_)[A-Za-z0-9]{36}",
    "GitHub fine-grained token": r"github_pat_[A-Za-z0-9_]{82}",
    "AWS access key": r"AKIA[0-9A-Z]{16}",
    "Google API key": r"AIza[0-9A-Za-z_-]{35}",
    "JWT": r"eyJ[A-Za-z0-9_-]{30,}\.[A-Za-z0-9_-]{30,}\.[A-Za-z0-9_-]{20,}",
    "Telegram bot token": r"[0-9]{8,12}:AA[A-Za-z0-9_-]{30,}",
    "private key marker": r"-----BEGIN (?:(?:RSA|EC|OPENSSH|DSA|PGP) )?PRIVATE KEY-----",
    "private IPv4 address": r"\b(?:10(?:\.[0-9]{1,3}){3}|172\.(?:1[6-9]|2[0-9]|3[0-1])(?:\.[0-9]{1,3}){2}|192\.168(?:\.[0-9]{1,3}){2})\b",
    "MAC address": r"\b(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}\b",
    "device AP suffix": r"\besp-claw-[0-9A-Fa-f]{6}\b",
    "macOS user path": r"/Users/[^\s`\"'<>]+",
    "macOS temp path": r"/var/folders/[^\s`\"'<>]+",
}
COMPILED = {name: re.compile(pattern) for name, pattern in PATTERNS.items()}
ASCII_RUN = re.compile(rb"[\x20-\x7e]{4,}")


def candidate_files() -> list[Path]:
    result = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        check=True,
    )
    paths = []
    for raw in result.stdout.split(b"\0"):
        if not raw:
            continue
        relative = raw.decode("utf-8", "surrogateescape")
        path = ROOT / relative
        if relative not in EXCLUDED and path.is_file():
            paths.append(path)
    return sorted(paths)


def searchable_text(data: bytes) -> tuple[str, bool]:
    try:
        text = data.decode("utf-8")
        if "\0" not in text:
            return text, True
    except UnicodeDecodeError:
        pass
    return "\n".join(run.decode("ascii") for run in ASCII_RUN.findall(data)), False


def main() -> int:
    findings: list[str] = []
    for path in candidate_files():
        relative = path.relative_to(ROOT).as_posix()
        text, has_lines = searchable_text(path.read_bytes())
        for label, pattern in COMPILED.items():
            match = pattern.search(text)
            if match is None:
                continue
            if has_lines:
                line = text.count("\n", 0, match.start()) + 1
                findings.append(f"{relative}:{line}: {label}")
            else:
                findings.append(f"{relative}: binary/metadata: {label}")
    if findings:
        print("Potential secret or public identifier material found:", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    print("No obvious secrets or public identifiers found in tracked or untracked files.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
