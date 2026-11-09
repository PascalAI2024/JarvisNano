#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile actual companion orchestration, with fake cached ports.

Extracted functions are unchanged production source. Dispatch checks enforce
the early boot / regular tick / physical escape wiring. Mutants use temporary
source only; compile failures are not successful mutation detections.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def function(source, declaration):
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
    raise AssertionError("unterminated production function")


def dispatch(source):
    boot = source[source.index("void app_main(void)"):]
    assert boot.index('setenv("TZ"') < boot.index("companion_boot_seed();")
    assert boot.index("restore_brightness_cap();") < boot.index("companion_boot_seed();")
    assert boot.index("jr_display_set_brightness(s_brightness_cap)") < boot.index("jr_display_start(")
    assert boot.index("companion_boot_seed();") < boot.index("jr_display_start(")
    assert boot.index("jr_display_start(") < boot.index("jr_net_init()")
    assert boot.index("s_glance_until_ms = s_companion_activity_ms + GLANCE_MS;") < \
        boot.index("atomic_store(&s_voice_start_gate, true)")
    assert boot.index('jr_display_caption_set("MUTED - HOLD TO LISTEN")') < \
        boot.index("atomic_store(&s_voice_start_gate, true)")
    assert source.count("const bool companion_clock = companion_update(") == 1
    assert "peek || watch_space || dream_glass || companion_clock" in source
    lift = source[source.index("const bool lifted = mout.changed"):
                  source.index("prev_mood = (uint8_t)mout.mood")]
    assert "s_weather.valid" not in lift and "GLANCE_MS" in lift
    assert "jr_display_nav_set" not in lift
    wake = source[source.index("if (jr_wake_feed("):source.index("capture_complete:")]
    assert "s_glance_until_ms = 0U;" in wake and "JR_DISPLAY_REST_NONE" in wake
    tap = source[source.index("if (iev.kind == JR_INPUT_PRESS_DOWN)") - 150:
                 source.index("/* ANY physical contact is activity")]
    assert "companion_attention((uint32_t)now);" in tap
    assert source.count("companion_phase_busy(") == 3
    speech = source[source.index("s_app.vad_starts++;"):source.index("s_utt_start_ms =")]
    assert "jr_display_rest_voice_busy_set(true);" in speech
    assert "companion_attention((uint32_t)now);" in speech


MUTANTS = {
    "usb-stale": ("battery->age_ms <= 2000U", "battery->age_ms <= 9000U"),
    "operator-mode-only": ("operator_lease_active(now)", "false"),
    "probation-off": ("const bool probation = image_in_probation();", "const bool probation = false;"),
    "tap-window": ("quiet ? JR_MOOD_QUIET_MS : JR_MOOD_AMBIENT_MS", "quiet ? 0U : 0U"),
    "glance-no-expiry": ("(int32_t)(now - s_glance_until_ms) >= 0", "false"),
    "glance-with-timer": ("s_boot_wake_cause == ESP_SLEEP_WAKEUP_EXT1", "s_boot_wake_cause == ESP_SLEEP_WAKEUP_TIMER"),
    "glance-no-magic": ("s_rtc_sleep_magic == SLEEP_RTC_MAGIC", "true"),
    "night-undimmed": ("view.night ? 8U : 22U", "view.night ? 22U : 22U"),
    "boot-rim-unmuted": ("atomic_load(&s_voice_privacy_paused), 0.0f, 0.0f", "false, 0.0f, 0.0f"),
    "foreground-allowed": ("phase != JR_ST_IDLE && phase != JR_ST_LISTENING", "false"),
    "speech-allowed": ("phase == JR_ST_LISTENING && speech", "((void)speech, false)"),
    "voice-busy-hidden": ("jr_display_rest_voice_busy_set(busy);", "jr_display_rest_voice_busy_set(false);"),
    "attention-retains-glance": ("s_glance_until_ms = 0U;\n    s_companion_activity_ms = now;", "s_companion_activity_ms = now;"),
}


def run(root, out, mutation):
    source = (root / "main/main.c").read_text(encoding="utf-8")
    dispatch(source)
    helpers = "\n\n".join(function(source, declaration) for declaration in (
        "static bool companion_phase_busy(",
        "static void companion_attention(",
        "static bool companion_update(", "static void companion_boot_seed(",
    ))
    if mutation:
        before, after = MUTANTS[mutation]
        assert helpers.count(before) == 1
        helpers = helpers.replace(before, after)
    template = (root / "main/tests/test_companion_main.c.in").read_text(encoding="utf-8")
    out.mkdir(parents=True, exist_ok=True)
    generated = out / "test.c"
    generated.write_text(template.replace("__PRODUCTION_HELPERS__", helpers), encoding="utf-8")
    compiler = shutil.which("cc") or shutil.which("gcc")
    if compiler is None:
        raise RuntimeError("no compiler; use host-tests Docker fallback")
    binary = out / "test_companion_main"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", str(generated),
                    str(root / "components/jr_display/src/companion.c"),
                    str(root / "components/jr_core/src/mood.c"),
                    "-I", str(root / "components/jr_display/include"),
                    "-I", str(root / "components/jr_core/include"),
                    "-I", str(root / "components/jr_ports/include"),
                    "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    (out / "result.txt").write_text(result.stdout + result.stderr, encoding="utf-8")
    print(result.stdout + result.stderr, end="")
    if result.returncode == 0:
        count = re.search(r"passed \(([0-9]+) checks, 0 failures\)", result.stdout)
        if not count or int(count.group(1)) == 0:
            raise AssertionError("no positive checks")
        print("companion orchestration passed (%s checks)" % count.group(1))
    return result.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation", choices=MUTANTS)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    if args.output_dir:
        return run(root, args.output_dir.resolve(), args.mutation)
    with tempfile.TemporaryDirectory(prefix="jr-companion-main-") as scratch:
        return run(root, Path(scratch), args.mutation)


if __name__ == "__main__":
    sys.exit(main())
