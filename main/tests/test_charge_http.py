#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the actual paired charge GET handler and its emitted JSON offline.

Extract the production static handler, compile it with real charge.c and tiny
HTTP/cache stubs, then compare JSON with independent expected values. Generated
source/binaries use a temporary directory unless --output-dir retains evidence.
Mutations change only the extracted scratch handler, never production source.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile


class Checks:
    def __init__(self) -> None:
        self.count = 0

    def equal(self, actual, expected, context: str) -> None:
        self.count += 1
        if type(actual) is not type(expected) or actual != expected:
            raise AssertionError(f"{context}: got {actual!r}; expected {expected!r}")


ADDRESSES = [0x00, 0x01, 0x15, 0x16, 0x18, 0x50, 0x58, 0x59,
             0x5A, 0x5B, 0x61, 0x62, 0x63, 0x64, 0x65, 0x67]
RAW = [0x2B, 0x2A, 5, 3, 2, 0xAA, 0xBB, 0xCC,
       0xDD, 0xEE, 3, 12, 0x12, 3, 1, 0xD5]
# Independent datasheet-derived expectations, rather than re-running decoder.
FIELDS = {
    "usb_present": (0, 1), "battery_present": (0, 1),
    "thermal_regulation": (0, 1), "current_limited": (0, 1),
    "direction": (1, 1), "charge_phase": (1, 2), "vindpm_active": (1, 1),
    "vindpm_millivolts": (2, 4280), "input_limit_milliamps": (3, 1000),
    "charge_enabled": (4, 1), "precharge_milliamps": (10, 75),
    "constant_current_milliamps": (11, 600),
    "termination_enabled": (12, 1), "termination_milliamps": (12, 50),
    "cv_millivolts": (13, 4200), "thermal_threshold_c": (14, 80),
    "precharge_timeout_enabled": (15, 1),
    "precharge_timeout_minutes": (15, 50), "charge_timeout_enabled": (15, 1),
    "charge_timeout_hours": (15, 8), "timeout_slows_when_limited": (15, 1),
}
RESERVED_NULLS = {"direction", "charge_phase", "input_limit_milliamps",
                  "precharge_milliamps", "constant_current_milliamps",
                  "termination_milliamps", "cv_millivolts"}
RESERVED_RAW = {1: 0x67, 3: 6, 10: 9, 11: 22, 12: 0x19, 13: 0}


def extract_handler(source: str) -> str:
    start = source.index("static esp_err_t power_charge_handler(")
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
    raise AssertionError("unterminated production power_charge_handler")


def validate_registration(source: str) -> None:
    source = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                    lambda token: "" if token.group().startswith("/") else token.group(),
                    source, flags=re.DOTALL)
    registrations = re.findall(
        r'\{\s*\.uri\s*=\s*"/api/power/charge"\s*,\s*'
        r'\.method\s*=\s*(\w+)\s*,\s*\.handler\s*=\s*(\w+)\s*\}', source)
    if registrations != [("HTTP_GET", "power_charge_handler")]:
        raise AssertionError(f"charge route registration changed: {registrations!r}")


def mutate(handler: str, mutation: str | None) -> str:
    if mutation is None:
        return handler
    if mutation == "auth-removal":
        before = "if (!agent_require_auth(req)) { return ESP_OK; }"
        after = "(void)agent_require_auth(req);"
    elif mutation == "current-on-failure":
        before = "jr_power_charge_decode(&sample, &decoded);"
        after = (before + "\n    if (!sample.regs[JR_POWER_CHARGE_CC].available) {\n"
                 "        decoded.constant_current_milliamps.available = true;\n"
                 "        decoded.constant_current_milliamps.value = 600;\n    }")
    else:
        before = "if (err != ESP_OK) { return err; }"
        after = "(void)err; /* deliberately continue after failed send */"
    count = handler.count(before)
    expected = 4 if mutation == "send-failure-continuation" else 1
    if count != expected:
        raise AssertionError(f"mutation anchor {mutation}: {count}, expected {expected}")
    return handler.replace(before, after)


def invoke(binary: Path, output: Path, scenario: str, fail_at: int = 0):
    completed = subprocess.run([str(binary), scenario, str(fail_at)],
                               check=True, capture_output=True, text=True)
    stem = f"{scenario}-{fail_at}"
    (output / f"{stem}.txt").write_text(completed.stdout + completed.stderr,
                                       encoding="utf-8")
    metadata, _, body = completed.stdout.partition("\n")
    return json.loads(metadata), body.strip()


def check_response(checks: Checks, metadata, body: str, scenario: str):
    prefix = scenario + ": "
    for name, expected in {"result": 0, "auth_calls": 1, "cache_calls": 1,
                           "chunks": 41, "errors": 0, "denied": False,
                           "terminated": True, "type": "application/json",
                           "cache_control": "no-store"}.items():
        checks.equal(metadata[name], expected, prefix + name)
    checks.equal(metadata["max_chunk"] < 384, True, prefix + "bounded chunk")
    doc = json.loads(body)
    checks.equal(set(doc), {"read_error", "complete", "sample_seq", "age_ms",
                           "span_ms", "started_ms", "completed_ms",
                           "atomic_snapshot", "measured_current", "registers",
                           "decoded"}, prefix + "top-level keys")
    unavailable = scenario in ("never", "busy")
    next_batch = scenario == "all-failed" or scenario.startswith("failure-")
    failed = int(scenario[8:]) if scenario.startswith("failure-") else None
    checks.equal(doc["read_error"], "ESP_ERR_INVALID_STATE" if scenario == "never"
                 else "ESP_ERR_TIMEOUT" if scenario == "busy" else "ESP_OK",
                 prefix + "read_error")
    checks.equal(doc["complete"], not unavailable, prefix + "complete")
    checks.equal(doc["sample_seq"], 0 if unavailable else 43 if next_batch else 42,
                 prefix + "sample_seq")
    checks.equal(doc["age_ms"], 0xFFFFFFFF if unavailable else
                 11400 if scenario == "stale" else
                 8500 if scenario == "mixed-age" else 100, prefix + "age_ms")
    checks.equal(doc["span_ms"], 0 if unavailable else 1600, prefix + "span_ms")
    offset = (1 << 64) - 1 - 5000 if scenario == "large-clock" else 0
    checks.equal(doc["started_ms"], 0 if unavailable else
                 3000 if next_batch else offset + 1000, prefix + "started_ms")
    checks.equal(doc["completed_ms"], 0 if unavailable else
                 4600 if next_batch else offset + 2600, prefix + "completed_ms")
    checks.equal(doc["atomic_snapshot"], False, prefix + "non-atomic label")
    checks.equal(doc["measured_current"], False, prefix + "current label")
    checks.equal(len(doc["registers"]), 16, prefix + "fixed register count")
    for i, register in enumerate(doc["registers"]):
        attempted = not unavailable
        bus_failed = scenario == "all-failed" or failed == i
        read_ok = attempted and not bus_failed
        fresh = scenario != "stale" and not (scenario == "mixed-age" and i == 0)
        available = read_ok and fresh
        age = 0xFFFFFFFF if unavailable else (
            13000 - i * 100 if scenario == "stale" else
            10100 - i * 100 if scenario == "mixed-age" else 1700 - i * 100)
        success_age = (age if not bus_failed else 3700 - i * 100)
        raw = 0 if not read_ok else RESERVED_RAW.get(i, RAW[i]) if scenario == "reserved" else RAW[i]
        expected = {"address": ADDRESSES[i], "raw": raw,
                    "attempted": attempted, "read_ok": read_ok,
                    "available": available, "has_success": attempted,
                    "error": 263 if bus_failed else 0, "attempt_age_ms": age,
                    "last_success_age_ms": success_age}
        checks.equal(set(register), set(expected), prefix + f"reg {i} keys")
        for name, value in expected.items():
            checks.equal(register[name], value, prefix + f"reg {i} {name}")
    checks.equal(set(doc["decoded"]), set(FIELDS), prefix + "decoded keys")
    for name, (index, good_value) in FIELDS.items():
        missing = (unavailable or scenario in ("stale", "all-failed") or
                   failed == index or (scenario == "mixed-age" and index == 0) or
                   (scenario == "reserved" and name in RESERVED_NULLS))
        expected = None if missing else good_value
        if scenario == "reserved" and name == "vindpm_active":
            expected = 0
        checks.equal(doc["decoded"][name], expected, prefix + name)


def run(root: Path, output: Path, mutation: str | None) -> int:
    source = (root / "main/http_routes.c").read_text(encoding="utf-8")
    validate_registration(source)
    handler = extract_handler(source)
    digest = hashlib.sha256(handler.encode()).hexdigest()
    handler = mutate(handler, mutation)
    template = (root / "main/tests/test_charge_http.c.in").read_text(encoding="utf-8")
    marker = "\n__PRODUCTION_HANDLER__\n"
    if template.count(marker) != 1:
        raise AssertionError("invalid template handler marker")
    output.mkdir(parents=True, exist_ok=True)
    generated = output / "test_charge_http.c"
    binary = output / "jr_charge_http_tests"
    generated.write_text(template.replace(marker, "\n" + handler + "\n"),
                         encoding="utf-8")
    compiler = shutil.which("cc") or shutil.which("gcc")
    if compiler is None:
        raise RuntimeError("no cc/gcc; use scripts/host-tests.sh Docker fallback")
    subprocess.run([compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    str(generated), str(root / "components/jr_power/src/charge.c"),
                    "-I", str(root / "components/jr_power/include"),
                    "-o", str(binary)], check=True)
    checks = Checks()
    meta, body = invoke(binary, output, "unauthorized")
    checks.equal(meta, {"result": 0, "auth_calls": 1, "cache_calls": 0,
                       "chunks": 0, "max_chunk": 0, "errors": 0,
                       "denied": True, "terminated": False, "type": "",
                       "cache_control": ""}, "unauthorized touches no cache or chunks")
    checks.equal(json.loads(body), {"error": "unauthorized"}, "auth response retained")
    scenarios = ["fresh", "never", "busy", "all-failed", "stale", "mixed-age",
                 "reserved", "large-clock"] + [f"failure-{i}" for i in range(16)]
    for scenario in scenarios:
        meta, body = invoke(binary, output, scenario)
        check_response(checks, meta, body, scenario)
    # Fail every actual chunk, including array/object boundaries and terminator.
    for chunk in range(1, 42):
        meta, _ = invoke(binary, output, "fresh", chunk)
        checks.equal(meta["result"], -1, f"send failure {chunk} propagates")
        checks.equal(meta["chunks"], chunk, f"send failure {chunk} stops immediately")
        checks.equal(meta["terminated"], chunk == 41, f"send failure {chunk} terminal")
        checks.equal(meta["cache_calls"], 1, f"send failure {chunk} no reread")
        checks.equal(meta["auth_calls"], 1, f"send failure {chunk} paired auth")
        checks.equal(meta["errors"], 0, f"send failure {chunk} no second response")
    result = (f"charge HTTP tests passed ({checks.count} checks); "
              "25 JSON scenarios, 41 failed-send positions; paired/auth/cache/"
              "reserved/failure/stale/64-bit/chunk-stop; "
              f"handler_sha256={digest}\n")
    (output / "result.txt").write_text(result, encoding="utf-8")
    print(result, end="")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path,
                        help="retain scratch source, binary and response evidence")
    parser.add_argument("--mutation", choices=("auth-removal", "current-on-failure",
                                               "send-failure-continuation"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    try:
        if args.output_dir is not None:
            return run(root, args.output_dir.resolve(), args.mutation)
        with tempfile.TemporaryDirectory(prefix="jr-charge-http-tests-") as scratch:
            return run(root, Path(scratch), args.mutation)
    except AssertionError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
