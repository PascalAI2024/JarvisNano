#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run production main.c shell policies against actual display behavior.

The C template reuses the display suite's actual jr_display.c and IDF stubs.
Static policies are extracted from main.c instead of reimplementing routing.
Dispatch validation fails closed if the input loop bypasses those policies.
Generated source and binaries are temporary unless --output-dir is supplied.
"""
from pathlib import Path
import argparse
import re
import shutil
import subprocess
import sys
import tempfile


def extract_function(source: str, name: str) -> str:
    start = source.index("static void " + name + "(")
    opening = source.index("{", start)
    # Ignore comments and literals while counting body braces. A future brace
    # inside a caption or comment must not silently truncate the tested body.
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
    raise AssertionError(f"unterminated production helper: {name}")


def validate_dispatch(source: str) -> None:
    swipe_start = source.index("bool style_changed")
    feedback_start = source.index("if (style_changed)", swipe_start)
    swipe_dispatch = source[swipe_start:feedback_start]
    swipe_dispatch = re.sub(r'/\*.*?\*/|//[^\n]*', '', swipe_dispatch,
                            flags=re.DOTALL)
    # The final non-shade/non-horizontal branch must contain only the tested
    # policy, followed by the existing shade-state publication.
    if not re.search(
        r'\}\s*else\s*\{\s*shell_vertical_navigation\(iev\.direction\);\s*\}'
        r'\s*s_ui_shade_open\s*=\s*jr_display_nav_overlay\(\)\s*=='
        r'\s*JR_DISPLAY_OVERLAY_SHADE\s*;\s*\Z', swipe_dispatch):
        raise AssertionError("production swipe dispatch bypasses tested policy")
    feedback_end = source.index('ESP_LOGI(TAG, "ui: nav overlay=%d"',
                                feedback_start)
    feedback = source[feedback_start:feedback_end]
    if not re.search(r'\}\s*else\s*\{\s*shell_caption_space\(\);\s*\}\s*\Z',
                     feedback):
        raise AssertionError("production swipe feedback bypasses tested policy")
    dismiss_dispatch = source[source.index("case JR_DISPLAY_ACT_DISMISS:"):
                              source.index("case JR_DISPLAY_ACT_FOCUS:")]
    if not re.fullmatch(r'case JR_DISPLAY_ACT_DISMISS:\s*shell_dismiss\(\);'
                        r'\s*break;\s*', dismiss_dispatch):
        raise AssertionError("production DISMISS dispatch bypasses tested policy")


def run(root: Path, output: Path, mutation: str | None) -> int:
    source = (root / "main/main.c").read_text(encoding="utf-8")
    validate_dispatch(source)
    helpers = "\n\n".join(extract_function(source, name) for name in (
        "shell_caption_space", "shell_vertical_navigation", "shell_dismiss"))
    if mutation == "detail-down-advances":
        before = "jr_display_nav_down();\n        } else {\n            jr_display_nav_next();"
        after = "jr_display_nav_next();\n        } else {\n            jr_display_nav_next();"
        assert helpers.count(before) == 1
        helpers = helpers.replace(before, after)
    elif mutation == "detail-dismiss-controls-caption":
        before = "if (overlay == JR_DISPLAY_OVERLAY_DETAIL) {\n            shell_caption_space();"
        after = ('if (overlay == JR_DISPLAY_OVERLAY_DETAIL) {\n'
                 '            jr_display_caption_set("CONTROLS - UP TO CLOSE");')
        assert helpers.count(before) == 1
        helpers = helpers.replace(before, after)
    template = (root / "main/tests/test_shell_input.c.in").read_text(
        encoding="utf-8")
    # One code insertion. The explanatory comment uses the same marker name.
    assert template.count("\n__PRODUCTION_HELPERS__\n") == 1
    output.mkdir(parents=True, exist_ok=True)
    generated = output / "test_shell_input.c"
    binary = output / "jr_main_shell_tests"
    generated.write_text(template.replace("\n__PRODUCTION_HELPERS__\n",
                                          "\n" + helpers + "\n"),
                         encoding="utf-8")
    compiler = shutil.which("cc") or shutil.which("gcc")
    if compiler is None:
        raise RuntimeError("no cc/gcc; use scripts/host-tests.sh Docker fallback")
    command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-function", "-D_GNU_SOURCE", str(generated),
               str(root / "components/jr_display/src/hud_render.c")]
    for include in (
        "components/jr_display/include", "components/jr_display/src",
        "components/jr_display/tests", "components/jr_ports/include",
        "components/jr_display/tests/probe/stubs",
    ):
        command += ["-I", str(root / include)]
    command += ["-o", str(binary)]
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
    parser.add_argument("--mutation", choices=(
        "detail-down-advances", "detail-dismiss-controls-caption"))
    parser.add_argument("--output-dir", type=Path,
                        help="retain generated source/binary/result as evidence")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    if args.output_dir is not None:
        return run(root, args.output_dir.resolve(), args.mutation)
    with tempfile.TemporaryDirectory(prefix="jr-main-shell-tests-") as scratch:
        return run(root, Path(scratch), args.mutation)


if __name__ == "__main__":
    sys.exit(main())
