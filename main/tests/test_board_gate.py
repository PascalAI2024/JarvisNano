#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run device_tools.c's board_maybe_poll against stubs.

With no gateway key (jr_tools_is_configured() false) the device must never
submit the board poll. The function is extracted from device_tools.c, not
retyped. --mutation drop-gate deletes the key check to show the test notices.
"""
from pathlib import Path
import argparse
import re
import shutil
import subprocess
import sys
import tempfile


def extract_function(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    tokens = re.finditer(
        r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',
        source[opening:], re.DOTALL)
    depth = 0
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start:opening + token.end()]
    raise AssertionError(f"unterminated production helper: {signature}")


def run(root: Path, output: Path, mutation: str | None) -> int:
    source = (root / "main/device_tools.c").read_text(encoding="utf-8")
    helper = extract_function(source, "void board_maybe_poll(").replace("\r\n", "\n")
    if mutation == "drop-gate":
        before = "!atomic_load(&s_tool_diag.worker_ready) ||\n        !jr_tools_is_configured()"
        assert helper.count(before) == 1, "the gate moved; update the mutation"
        helper = helper.replace(before, "!atomic_load(&s_tool_diag.worker_ready)")
    elif mutation == "fixed-cadence":
        before = "now - s_board_last_poll_ms < device_gateway_poll_ms()"
        assert helper.count(before) == 1, "the cadence moved; update the mutation"
        helper = helper.replace(before, "now - s_board_last_poll_ms < BOARD_POLL_MS")
    else:
        assert "device_gateway_poll_ms()" in helper, \
            "board_maybe_poll no longer asks the gateway's health for its cadence"
        assert "!jr_tools_is_configured()" in helper, \
            "board_maybe_poll no longer checks for a gateway key"
    template = (root / "main/tests/test_board_gate.c.in").read_text(encoding="utf-8")
    assert template.count("\n__PRODUCTION_HELPERS__\n") == 1
    output.mkdir(parents=True, exist_ok=True)
    generated = output / "test_board_gate.c"
    binary = output / "jr_board_gate_tests"
    generated.write_text(template.replace("\n__PRODUCTION_HELPERS__\n",
                                          "\n" + helper + "\n"),
                         encoding="utf-8")
    compiler = shutil.which("cc") or shutil.which("gcc")
    if compiler is None:
        raise RuntimeError("no cc/gcc; use scripts/host-tests.sh Docker fallback")
    command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-function", "-D_GNU_SOURCE", str(generated),
               "-I", str(root / "components/jr_core/include"),
               "-I", str(root / "components/jr_ports/include"),
               "-o", str(binary)]
    subprocess.run(command, check=True)
    completed = subprocess.run([str(binary)], check=False,
                               capture_output=True, text=True)
    (output / "result.txt").write_text(completed.stdout + completed.stderr,
                                      encoding="utf-8")
    sys.stdout.write(completed.stdout)
    sys.stderr.write(completed.stderr)
    return completed.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation", choices=("drop-gate", "fixed-cadence"))
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    if args.output_dir is not None:
        return run(root, args.output_dir.resolve(), args.mutation)
    with tempfile.TemporaryDirectory(prefix="jr-board-gate-tests-") as scratch:
        return run(root, Path(scratch), args.mutation)


if __name__ == "__main__":
    sys.exit(main())
