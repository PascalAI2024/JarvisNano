#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile production companion helpers against the actual display renderer.

Only external main ports/globals are faked. test_shell.c includes jr_display.c
and its established IDF fakes. Extracted main helpers are unchanged source;
this proves their composition, not execution of the infinite voice loop.
Scratch faults must compile and fail positive runtime checks. No device access.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


DECLARATIONS = (
    "static bool companion_phase_busy(",
    "static void companion_attention(",
    "static bool companion_update(",
    "static void companion_boot_seed(",
    "static jr_face_t phase_to_face(",
)

# Each mutation is applied only to a generated helper or scratch renderer copy.
# Compile errors, extraction errors and zero-check runs are never detections.
MUTATIONS = {
    "foreground-phase-idle": ("helpers",
        "phase != JR_ST_IDLE && phase != JR_ST_LISTENING", "false"),
    "active-speech-idle": ("helpers",
        "phase == JR_ST_LISTENING && speech", "false && speech"),
    "foreground-caption-hidden": ("helpers",
        "jr_display_caption_foreground() ||", "false ||"),
    "stale-usb-accepted": ("helpers",
        "battery->age_ms <= 2000U", "battery->age_ms <= 9000U"),
    "night-undimmed": ("helpers",
        "view.night ? 8U : 22U", "view.night ? 22U : 22U"),
    "attention-retains-glance": ("helpers",
        "s_glance_until_ms = 0U;\n    s_companion_activity_ms = now;",
        "s_companion_activity_ms = now;"),
    "idle-listening-denied": ("renderer",
        "(face == JR_FACE_LISTENING && mode != JR_DISPLAY_REST_DREAM)",
        "false"),
    "voice-busy-refusal-bypassed": ("renderer",
        "mode != JR_DISPLAY_REST_NONE && !(env & JR_DISPLAY_ENV_VOICE_BUSY)",
        "mode != JR_DISPLAY_REST_NONE"),
    "authoritative-usb-refusal-bypassed": ("renderer",
        "(usb && (env & JR_DISPLAY_ENV_USB))", "(env & JR_DISPLAY_ENV_USB)"),
    "caption-prefix-ambient": ("renderer",
        'strcmp(text, quiet[i]) == 0',
        'strncmp(text, quiet[i], strlen(quiet[i])) == 0'),
    "charging-range-unbounded": ("renderer",
        "percent <= 100U && *p == '%'", "*p == '%'"),
    "initial-cap-ignored": ("renderer",
        "const uint8_t initial = __atomic_load_n(&s_brightness_want, __ATOMIC_ACQUIRE);",
        "const uint8_t initial = 100U;"),
}


def extract_function(source, declaration):
    if source.count(declaration) != 1:
        raise AssertionError("production declaration missing or ambiguous: " + declaration)
    start = source.index(declaration)
    opening = source.index("{", start)
    depth = 0
    for token in re.finditer(
        r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',
        source[opening:], re.DOTALL,
    ):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start:opening + token.end()]
    raise AssertionError("unterminated production declaration: " + declaration)


def snapshot(root):
    paths = (
        "main/main.c", "components/jr_display/src/jr_display.c",
        "components/jr_display/src/companion.c", "components/jr_core/src/mood.c",
        "components/jr_display/tests/test_shell.c",
        "components/jr_display/tests/probe/stubs/idf_stubs.inc",
        "main/tests/test_companion_integration.c.in",
        "main/tests/test_companion_integration.py",
    )
    raw = {path: (root / path).read_bytes() for path in paths}
    hashes = {path: hashlib.sha256(value).hexdigest() for path, value in raw.items()}
    return raw, hashes


def run(root, output, mutation, raw, hashes):
    helpers = "\n\n".join(extract_function(raw["main/main.c"].decode("utf-8").replace("\r\n", "\n"), decl)
                            for decl in DECLARATIONS)
    renderer = raw["components/jr_display/src/jr_display.c"].decode("utf-8")
    if mutation:
        target, before, after = MUTATIONS[mutation]
        text = helpers if target == "helpers" else renderer
        if text.count(before) != 1:
            raise AssertionError("mutation target missing or ambiguous: " + mutation)
        text = text.replace(before, after)
        if target == "helpers":
            helpers = text
        else:
            renderer = text
    template = raw["main/tests/test_companion_integration.c.in"].decode("utf-8")
    if template.count("\n__PRODUCTION_HELPERS__\n") != 1:
        raise AssertionError("one production insertion required")
    output.mkdir(parents=True, exist_ok=True)
    generated = output / "test_companion_integration.c"
    generated.write_text(template.replace("\n__PRODUCTION_HELPERS__\n",
                                         "\n" + helpers + "\n"), encoding="utf-8")
    # The first include directory selects this exact snapshot through the
    # existing test_shell.c include pattern, including for positive runs.
    (output / "jr_display.c").write_text(renderer, encoding="utf-8")
    compiler = shutil.which("cc") or shutil.which("gcc")
    if compiler is None:
        raise RuntimeError("no cc/gcc; use the host-tests Docker compiler environment")
    binary = output / "jr_companion_integration_tests"
    command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-function", "-D_GNU_SOURCE", str(generated),
               str(root / "components/jr_display/src/hud_render.c"),
               str(root / "components/jr_display/src/companion.c"),
               str(root / "components/jr_core/src/mood.c"), "-I", str(output)]
    for include in (
        "components/jr_display/include", "components/jr_display/src",
        "components/jr_display/tests", "components/jr_ports/include",
        "components/jr_display/tests/probe/stubs", "components/jr_core/include",
        "components/jr_power/include",
    ):
        command += ["-I", str(root / include)]
    command += ["-o", str(binary)]
    compiled = subprocess.run(command, capture_output=True, text=True)
    (output / "compile.txt").write_text(compiled.stdout + compiled.stderr, encoding="utf-8")
    if compiled.returncode:
        sys.stderr.write(compiled.stdout + compiled.stderr)
        raise RuntimeError("compile failed; not a mutation detection")
    completed = subprocess.run([str(binary)], capture_output=True, text=True)
    (output / "result.txt").write_text(completed.stdout + completed.stderr, encoding="utf-8")
    count = re.search(r"companion integration tests: ([0-9]+) checks, ([0-9]+) failures",
                      completed.stdout)
    if not count or int(count.group(1)) == 0:
        raise AssertionError("runtime must report positive integration checks")
    result = {"mutation": mutation, "compiled": True, "checks": int(count.group(1)),
              "failures": int(count.group(2)), "exit_code": completed.returncode,
              "source_sha256": hashes}
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    sys.stdout.write(completed.stdout)
    sys.stderr.write(completed.stderr)
    return result


def verify(root, output, mutation, all_mutations):
    raw, hashes = snapshot(root)
    if not all_mutations:
        return run(root, output, mutation, raw, hashes)["exit_code"]
    positive = run(root, output / "positive", None, raw, hashes)
    if positive["exit_code"] or positive["failures"]:
        raise AssertionError("positive integration run failed")
    results = [positive]
    for name in MUTATIONS:
        result = run(root, output / name, name, raw, hashes)
        if result["exit_code"] == 0 or result["failures"] == 0:
            raise AssertionError("compiled runtime mutation survived: " + name)
        results.append(result)
    if snapshot(root)[1] != hashes:
        raise AssertionError("production sources changed during proof; rerun against a stable snapshot")
    (output / "summary.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print("companion integration mutation proof passed (%d/%d compiled runtime faults)" %
          (len(MUTATIONS), len(MUTATIONS)))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--mutation", choices=MUTATIONS)
    group.add_argument("--verify-mutations", action="store_true")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    if args.output_dir:
        return verify(root, args.output_dir.resolve(), args.mutation, args.verify_mutations)
    with tempfile.TemporaryDirectory(prefix="jr-companion-integration-") as scratch:
        return verify(root, Path(scratch), args.mutation, args.verify_mutations)


if __name__ == "__main__":
    sys.exit(main())
