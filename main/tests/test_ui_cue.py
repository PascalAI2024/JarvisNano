#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run main.c's interface-cue wiring (ui_cue, ui_level_cue) against the real
jr_cue.c gate, with the audio lane stubbed.

The helpers are extracted from main.c, not retyped, so the test fails if the
production wiring stops carrying the touch source into the gate. Pass
--mutation to see the test catch a wiring that ignores it.
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
    source = (root / "main/main.c").read_text(encoding="utf-8")
    helpers = "\n\n".join(extract_function(source, sig) for sig in (
        "static bool owner_speaker_muted(",
        "static void ui_cue(",
        "static void ui_level_cue("))
    if mutation == "ignore-source":
        before = "physical ? JR_CUE_SOURCE_PHYSICAL\n                              : JR_CUE_SOURCE_SYNTHETIC"
        # main.c may be CRLF; compare after normalising.
        norm = helpers.replace("\r\n", "\n")
        assert norm.count(before) == 1, "ui_cue no longer passes the source"
        helpers = norm.replace(before, "physical ? JR_CUE_SOURCE_PHYSICAL : JR_CUE_SOURCE_PHYSICAL")
    template = (root / "main/tests/test_ui_cue.c.in").read_text(encoding="utf-8")
    assert template.count("\n__PRODUCTION_HELPERS__\n") == 1
    output.mkdir(parents=True, exist_ok=True)
    generated = output / "test_ui_cue.c"
    binary = output / "jr_ui_cue_tests"
    generated.write_text(template.replace("\n__PRODUCTION_HELPERS__\n",
                                          "\n" + helpers + "\n"),
                         encoding="utf-8")
    compiler = shutil.which("cc") or shutil.which("gcc")
    if compiler is None:
        raise RuntimeError("no cc/gcc; use scripts/host-tests.sh Docker fallback")
    command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-function", "-D_GNU_SOURCE", str(generated),
               str(root / "components/jr_core/src/cue.c"),
               "-I", str(root / "components/jr_core/include"),
               "-I", str(root / "components/jr_ports/include"),
               "-o", str(binary), "-lm"]
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
    parser.add_argument("--mutation", choices=("ignore-source",))
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    if args.output_dir is not None:
        return run(root, args.output_dir.resolve(), args.mutation)
    with tempfile.TemporaryDirectory(prefix="jr-ui-cue-tests-") as scratch:
        return run(root, Path(scratch), args.mutation)


if __name__ == "__main__":
    sys.exit(main())
