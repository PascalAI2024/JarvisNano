#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Execute actual OTA admission/restoration helpers with controlled lock ports.

Route assertions prove source wiring only, not runtime HTTP/flash execution.
Mutation detections require successful compilation and positive runtime checks.
No device/network access. Use the host-test compiler environment when needed.
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


ADMIT = "static esp_err_t ota_admit_upload("
RESTORE = "static void ota_restore_control_state("
CLOCK = "    *started_ms = (uint32_t)(esp_timer_get_time() / 1000);\n"
LOCK = "s_brain_lock == NULL ||\n        xSemaphoreTake(s_brain_lock, pdMS_TO_TICKS(100)) != pdTRUE"
ACTIVE = "    atomic_store(&s_ota_active, false);\n"
MUTATIONS = {
    "admission-lock-bypassed": (ADMIT, LOCK, "false"),
    "admission-active-ignored": (ADMIT, "if (atomic_load(&s_ota_active))",
                                  "if (atomic_load(&s_ota_active) && false)"),
    "admission-clock-outside-lock": (ADMIT, CLOCK, "@MOVE_CLOCK@"),
    "admission-active-after-unlock": (ADMIT,
        "    atomic_store(&s_ota_active, true);\n", "@MOVE_ACTIVE@"),
    "admission-lease-after-unlock": (ADMIT,
        "    atomic_store(&s_operator_lease_until_ms, *started_ms + 180000U);\n",
        "@MOVE_LEASE@"),
    "restore-active-cleared-first": (RESTORE, "uint32_t previous_lease_until_ms)\n{\n",
                                    "uint32_t previous_lease_until_ms)\n{\n" + ACTIVE),
    "restore-active-before-lease": (RESTORE, ACTIVE, "@MOVE_RESTORE@"),
    "owner-cleared-lease-recreated": (RESTORE,
        "if (atomic_load(&s_operator_lease_until_ms) != 0U)", "if (true)"),
    "expired-lease-revived": (RESTORE,
        "(int32_t)(previous_lease_until_ms - now) > 0",
        "previous_lease_until_ms != now"),
    "wrap-lease-comparison-broken": (RESTORE,
        "(int32_t)(previous_lease_until_ms - now) > 0",
        "previous_lease_until_ms > now"),
    "prior-privacy-ignored": (RESTORE,
        "was_privacy_paused || atomic_load(&s_voice_privacy_paused)",
        "(was_privacy_paused && false) || atomic_load(&s_voice_privacy_paused)"),
    "current-privacy-ignored": (RESTORE,
        "was_privacy_paused || atomic_load(&s_voice_privacy_paused)",
        "was_privacy_paused || (atomic_load(&s_voice_privacy_paused) && false)"),
    "restore-lock-failure-clears-veto": (RESTORE,
        "        ESP_LOGW(TAG,", "        atomic_store(&s_ota_active, false);\n        ESP_LOGW(TAG,"),
}


def block_end(source, opening):
    depth = 0
    for token in re.finditer(
        r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',
        source[opening:], re.DOTALL,
    ):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return opening + token.end()
    raise AssertionError("unterminated production block")


def function(source, declaration):
    if source.count(declaration) != 1:
        raise AssertionError("missing or ambiguous declaration: " + declaration)
    start = source.index(declaration)
    opening = source.index("{", start)
    return source[start:block_end(source, opening)]


def route_wiring(source):
    """Fail closed on route drift. These are structural assertions only."""
    checks = 0

    def require(condition, explanation):
        nonlocal checks
        checks += 1
        if not condition:
            raise AssertionError("route wiring: " + explanation)

    for declaration, started in (
        ("static esp_err_t ota_upload_handler(", "upload_started_ms"),
        ("static esp_err_t ota_assets_handler(", "started_ms"),
    ):
        route = function(source, declaration)
        call = re.search(r"const\s+esp_err_t\s+admission\s*=\s*ota_admit_upload\(\s*&" +
                         started + r",\s*&previous_lease_until_ms,\s*&was_privacy_paused\s*\);", route)
        require(call is not None, declaration + " correct admission outputs")
        require(len(re.findall(r"\bota_admit_upload\s*\(", route)) == 1,
                declaration + " exactly one admission")
        guard = re.search(r"if\s*\(admission\s*!=\s*ESP_OK\)\s*{", route)
        require(guard is not None and guard.start() > call.end(),
                declaration + " checked admission result")
        end = block_end(route, route.index("{", guard.start()))
        require(bool(re.search(r"return\s+ESP_OK\s*;\s*}\s*$", route[guard.end():end])),
                declaration + " unavailable admission returns")
        flash = list(re.finditer(
            r"\b(?:esp_ota_(?:begin|write|end|abort|set_boot_partition)|"
            r"esp_partition_(?:erase_range|write))\s*\(", route))
        require(bool(flash), declaration + " actual flash operation exists")
        require(all(item.start() >= end for item in flash),
                declaration + " admission and rejection precede all flash operations")
        require(not re.search(r"atomic_store(?:_explicit)?\s*\(\s*&s_ota_active\s*,", route),
                declaration + " active owned exclusively by helpers")
        restore_calls = re.findall(
            r"ota_restore_control_state\(\s*was_privacy_paused,\s*previous_lease_until_ms\s*\)", route)
        require(len(restore_calls) == (2 if started == "upload_started_ms" else 1),
                declaration + " every existing error cleanup uses original captured state")
    return checks


def mutate(helper, before, after):
    if helper.count(before) != 1:
        raise AssertionError("mutation target missing or ambiguous")
    if after == "@MOVE_CLOCK@":
        helper = helper.replace(before, "")
        opening = helper.index("{\n")
        return helper[:opening + 2] + before + helper[opening + 2:]
    if after in ("@MOVE_ACTIVE@", "@MOVE_LEASE@"):
        helper = helper.replace(before, "")
        give = "    xSemaphoreGive(s_brain_lock);\n    return ESP_OK;"
        if helper.count(give) != 1:
            raise AssertionError("admission release anchor drift")
        return helper.replace(give, "    xSemaphoreGive(s_brain_lock);\n" + before + "    return ESP_OK;")
    if after == "@MOVE_RESTORE@":
        helper = helper.replace(before, "")
        anchor = "    if (atomic_load(&s_operator_lease_until_ms) != 0U)"
        return helper.replace(anchor, before + anchor)
    return helper.replace(before, after)


def snapshot(root):
    paths = ("main/http_routes.c", "main/tests/test_ota_sleep_fence.py",
             "main/tests/test_ota_sleep_fence.c.in")
    raw = {path: (root / path).read_bytes() for path in paths}
    return raw, {path: hashlib.sha256(body).hexdigest() for path, body in raw.items()}


def run(root, out, mutation, raw, hashes, wiring_checks):
    source = raw["main/http_routes.c"].decode("utf-8").replace("\r\n", "\n")
    helpers = {decl: function(source, decl) for decl in (ADMIT, RESTORE)}
    if mutation:
        decl, before, after = MUTATIONS[mutation]
        helpers[decl] = mutate(helpers[decl], before, after)
    template = raw["main/tests/test_ota_sleep_fence.c.in"].decode("utf-8")
    if template.count("__PRODUCTION_HELPERS__") != 1:
        raise AssertionError("production insertion marker drift")
    out.mkdir(parents=True, exist_ok=True)
    generated = out / "test_ota_sleep_fence.c"
    generated.write_text(template.replace("__PRODUCTION_HELPERS__", "\n\n".join(helpers.values())), encoding="utf-8")
    compiler = shutil.which("cc") or shutil.which("gcc")
    if not compiler:
        raise RuntimeError("no cc/gcc; use host-test compiler environment")
    binary = out / "jr_ota_sleep_fence_tests"
    compiled = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                               str(generated), "-o", str(binary)], capture_output=True, text=True)
    (out / "compile.txt").write_text(compiled.stdout + compiled.stderr, encoding="utf-8")
    if compiled.returncode:
        sys.stderr.write(compiled.stdout + compiled.stderr)
        raise RuntimeError("compile failure is not a mutation detection")
    completed = subprocess.run([str(binary)], capture_output=True, text=True)
    (out / "result.txt").write_text(completed.stdout + completed.stderr, encoding="utf-8")
    match = re.search(r"OTA sleep fence (?:passed|FAILED) \(([0-9]+) checks(?:; ([0-9]+) failures)?\)", completed.stdout)
    if not match or int(match.group(1)) == 0:
        raise AssertionError("runtime must report positive checks")
    result = {"mutation": mutation, "compiled": True, "checks": int(match.group(1)),
              "failures": int(match.group(2) or 0), "exit_code": completed.returncode,
              "structural_route_checks": wiring_checks, "source_sha256": hashes}
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if mutation:
        print(f"OTA scratch {mutation}: {result['checks']} runtime checks, {result['failures']} failures, exit {result['exit_code']}")
    else:
        sys.stdout.write(completed.stdout)
        sys.stderr.write(completed.stderr)
    return result


def verify(root, out, mutation, all_mutations):
    raw, hashes = snapshot(root)
    wiring_checks = route_wiring(raw["main/http_routes.c"].decode("utf-8").replace("\r\n", "\n"))
    print(f"OTA route wiring: {wiring_checks} structural checks (not HTTP execution)")
    if not all_mutations:
        result = run(root, out, mutation, raw, hashes, wiring_checks)
        if snapshot(root)[1] != hashes:
            raise AssertionError("sources changed during proof; rerun")
        return result["exit_code"]
    positive = run(root, out / "positive", None, raw, hashes, wiring_checks)
    if positive["exit_code"] or positive["failures"]:
        raise AssertionError("positive OTA helper runtime failed")
    results = [positive]
    for name in MUTATIONS:
        result = run(root, out / name, name, raw, hashes, wiring_checks)
        if result["exit_code"] == 0 or not result["failures"]:
            raise AssertionError("compiled runtime mutation survived: " + name)
        results.append(result)
    if snapshot(root)[1] != hashes:
        raise AssertionError("sources changed during proof; rerun")
    (out / "summary.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print(f"OTA sleep fence mutation proof passed ({len(MUTATIONS)}/{len(MUTATIONS)} compiled runtime faults)")
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
    with tempfile.TemporaryDirectory(prefix="jr-ota-sleep-fence-") as temporary:
        return verify(root, Path(temporary), args.mutation, args.verify_mutations)


if __name__ == "__main__":
    sys.exit(main())
