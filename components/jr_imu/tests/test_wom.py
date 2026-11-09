#!/usr/bin/env python3
"""Exercise the production WoM command and arming sequence at 100 Hz ticks."""
import argparse
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


MUTATIONS = {
    "zero-tick-poll": ("vTaskDelay(IMU_CMD_GAP_TICKS);", "vTaskDelay(pdMS_TO_TICKS(2));"),
    "timeout-success": ("} while (esp_timer_get_time() < deadline);\n    return ESP_ERR_TIMEOUT;",
                        "} while (esp_timer_get_time() < deadline);\n    return ESP_OK;"),
    "ack-error-ignore": ("if (ack_err != ESP_OK) {", "if (false) {"),
}


def function(source, name):
    match = re.search(r"^(?:static )?esp_err_t " + name + r"\([^;]*?\)\n\{", source, re.M)
    if not match:
        raise AssertionError("missing production function " + name)
    start = match.start()
    pos = source.index("{", match.start())
    depth = 1
    for end in range(pos + 1, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if not depth:
            return source[start:end + 1]
    raise AssertionError("unclosed production function " + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--mutation", choices=MUTATIONS)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    source = (args.source or root / "components/jr_imu/src/jr_imu.c").read_text()
    print("production jr_imu.c sha256=" + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    if args.mutation:
        old, new = MUTATIONS[args.mutation]
        assert source.count(old) == 1, "mutation anchor drift"
        source = source.replace(old, new)
    constants = "\n".join(re.findall(r"^#define (?:QMI8658_|IMU_CMD_)\w+[^\n]*", source, re.M))
    functions = []
    if "static esp_err_t imu_wait_cmd_done(" in source:
        functions.append(function(source, "imu_wait_cmd_done"))
    functions += [function(source, "imu_ctrl9"), function(source, "jr_imu_arm_wake_on_motion")]
    template = Path(__file__).with_suffix(".c.in").read_text()
    generated = template.replace("__PRODUCTION_CONSTANTS__", constants).replace(
        "__PRODUCTION_FUNCTIONS__", "\n\n".join(functions))
    compiler = shutil.which("cc") or shutil.which("gcc")
    if not compiler:
        raise RuntimeError("no C compiler; use scripts/host-tests.sh Docker fallback")
    with tempfile.TemporaryDirectory(prefix="jr-imu-wom-") as scratch:
        path = Path(scratch)
        (path / "test.c").write_text(generated)
        binary = path / "test"
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(path / "test.c"), "-o", str(binary)], check=True)
        return subprocess.run([str(binary)]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
