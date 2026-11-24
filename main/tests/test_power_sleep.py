#!/usr/bin/env python3
"""Compile the actual sleep coordinator with fault-injecting host peripherals."""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


MUTATIONS = {
    "timer-error-bypass": ("if (timer_err != ESP_OK) {", "if (false) {"),
    "panel-error-bypass": ("if (!jr_display_panel_is_off()) {", "if (false) {"),
    "timer-cleanup-removal": (
        "const esp_err_t err = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);",
        "const esp_err_t err = ESP_OK;"),
    "usb-guard-removal": (
        "power.age_ms > SLEEP_POWER_MAX_AGE_MS || power.usb_present)",
        "power.age_ms > SLEEP_POWER_MAX_AGE_MS)"),
    "final-usb-check-removal": (
        "if (!sleep_usb_absence_known()) {\n        xSemaphoreGive(s_brain_lock);",
        "if (!sleep_usb_absence_known() && false) {\n        xSemaphoreGive(s_brain_lock);"),
    "privacy-on-refusal": (
        "atomic_store(&s_sleep_force, false);\n}\n\nvoid enter_deep_sleep",
        "atomic_store(&s_voice_privacy_paused, false);\n"
        "    atomic_store(&s_sleep_force, false);\n}\n\nvoid enter_deep_sleep"),
    "panel-recovery-removal": (
        "jr_display_panel_on_request();", "/* recovery request removed */"),
    "active-lift-wake": (
        "if (rtc_gpio_get_level(SLEEP_WAKE_IMU_GPIO) == 0) {",
        "if (rtc_gpio_get_level(SLEEP_WAKE_IMU_GPIO) != 0) {"),
    "rtc-early-commit": (
        "void enter_deep_sleep(const char *why, uint32_t timer_s)\n{",
        "void enter_deep_sleep(const char *why, uint32_t timer_s)\n{\n"
        "    s_rtc_sleep_magic = SLEEP_RTC_MAGIC;\n    s_rtc_sleeps++;"),
    "ota-ownership-removal": ("if (updating || leased) {", "if (leased) {"),
    "lease-ownership-removal": ("if (updating || leased) {", "if (updating) {"),
    "initial-ownership-removal": (
        "if (sleep_owner_active()) {\n        atomic_store(&s_sleep_force, false);",
        "if (sleep_owner_active() && false) {\n        atomic_store(&s_sleep_force, false);"),
    "late-ownership-removal": (
        "if (sleep_owner_active()) {\n        xSemaphoreGive(s_brain_lock);",
        "if (sleep_owner_active() && false) {\n        xSemaphoreGive(s_brain_lock);"),
    "admission-lock-bypass": (
        "xSemaphoreTake(s_brain_lock, pdMS_TO_TICKS(100)) != pdTRUE",
        "pdTRUE != pdTRUE"),
    "release-before-irreversible-work": (
        "uint8_t armed = SLEEP_ARMED_TIMER;",
        "xSemaphoreGive(s_brain_lock);\n    uint8_t armed = SLEEP_ARMED_TIMER;"),
    "usb-refusal-lock-leak": (
        "if (!sleep_usb_absence_known()) {\n        xSemaphoreGive(s_brain_lock);",
        "if (!sleep_usb_absence_known()) {\n        /* lock leaked */"),
    "audio-refusal-lock-leak": (
        "esp_err_to_name(audio_err));\n        xSemaphoreGive(s_brain_lock);",
        "esp_err_to_name(audio_err));\n        /* lock leaked */"),
}


def run(root: Path, output: Path, mutation: str | None) -> int:
    source = (root / "main/power.c").read_text(encoding="utf-8")
    digest = hashlib.sha256(source.encode()).hexdigest()
    if mutation:
        old, new = MUTATIONS[mutation]
        if source.count(old) != 1:
            raise AssertionError(f"mutation anchor drift: {mutation}")
        source = source.replace(old, new, 1)
    template = (root / "main/tests/test_power_sleep.c.in").read_text(encoding="utf-8")
    if template.count("__PRODUCTION_SOURCE__") != 1:
        raise AssertionError("production source marker drift")
    compiler = shutil.which("cc") or shutil.which("gcc")
    if not compiler:
        raise RuntimeError("no cc/gcc; use scripts/host-tests.sh Docker fallback")
    output.mkdir(parents=True, exist_ok=True)
    generated, binary = output / "test_power_sleep.c", output / "jr_power_sleep_tests"
    generated.write_text(template.replace("__PRODUCTION_SOURCE__", source), encoding="utf-8")
    subprocess.run([compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-I", str(root / "host/fakes"), str(generated), "-o", str(binary)],
                   check=True)
    print(f"compiled actual power.c; source_sha256={digest}; mutation={mutation or 'none'}",
          flush=True)
    result = subprocess.run([str(binary)], text=True, capture_output=True)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    (output / "result.txt").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode == 0:
        import re
        count = re.search(r"passed \(([0-9]+) checks; ([0-9]+) scenarios\)", result.stdout)
        assert count and int(count.group(1)) > 0 and int(count.group(2)) > 0, "no positive checks"
        print("sleep coordinator passed (%s checks)" % count.group(1))
    return result.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, help="retain scratch source/binary/output")
    parser.add_argument("--mutation", choices=tuple(MUTATIONS))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    try:
        if args.output_dir:
            return run(root, args.output_dir.resolve(), args.mutation)
        with tempfile.TemporaryDirectory(prefix="jr-power-sleep-") as scratch:
            return run(root, Path(scratch), args.mutation)
    except AssertionError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
