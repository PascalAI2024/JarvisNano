#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile the production /api/diag/board JSON writer and check what it emits.

The writer (components/jr_probe/src/board_json.c) is plain C, so this builds
the real file, not a copy. Expected values are written out here by hand rather
than derived from the code under test. Mutations edit a scratch copy only.
"""
import argparse, json, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SRC = ROOT / "components/jr_probe/src/board_json.c"
HARNESS = ROOT / "components/jr_probe/tests/test_board_json.c"
INC = ROOT / "components/jr_probe/include"

MUTANTS = {
    "ts-half-lost": ("(unsigned)((x->ts_raw % 2u) * 5u)", "0u"),
    "temp-sign-lost": ('t < 0 ? "-" : ""', '""'),
    "ok-always-true": ("tf(ok));", '"true");'),
    "gyro-restored-forged": ("tf(m->restored)", '"true"'),
    "edges-note-dropped": ("if (te->edges == 0) {", "if (0) {"),
}


def build(src_text, out):
    out.mkdir(parents=True, exist_ok=True)
    c = out / "board_json.c"
    c.write_text(src_text)
    exe = out / "t"
    cc = shutil.which("cc") or shutil.which("gcc")
    subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-I", str(INC),
                    str(c), str(HARNESS), "-o", str(exe)], check=True)
    return exe


def run(exe, mode):
    p = subprocess.run([str(exe), mode], capture_output=True, text=True)
    first, _, body = p.stdout.partition("\n")
    return p.returncode, int(first), body


class Checks:
    n = 0

    def eq(self, a, b, what):
        self.n += 1
        if type(a) is not type(b) or a != b:
            raise AssertionError(f"{what}: got {a!r}, expected {b!r}")


def check(exe, c):
    rc, n, body = run(exe, "full")
    c.eq(rc, 0, "full rc")
    doc = json.loads(body)
    c.eq(n, len(body), "full length")
    c.eq(n < 2048, True, "fits the 2048-byte handler buffer")
    c.eq(doc["te"], {"ok": True, "window_ms": 500, "edges": 30, "hz": 60,
                     "period_us_min": 16600, "period_us_max": 16700,
                     "level_now": 1}, "te")
    c.eq(doc["pwr_key"], {"ok": True, "level": 0, "watched": True,
                          "watch_ms": 3000, "changed": True, "transitions": 2,
                          "high_ms": 640, "level_min": 0, "level_max": 1}, "pwr")
    imu = doc["imu"]
    c.eq(imu["who_am_i"], 5, "who")
    c.eq(imu["revision"], 124, "rev")
    c.eq((imu["ctrl1"], imu["ctrl2"], imu["ctrl3"], imu["ctrl7"]),
         (64, 38, 0, 1), "ctrl")
    c.eq(imu["temp_c"], -5.3, "temp -53 tenths")
    c.eq(imu["gyro"], {"ran": True, "samples": 20,
                       "min_dps": [-0.5, 0.25, -1.0], "max_dps": [0.75, 1.5, 2.0],
                       "mean_dps": [0.12, 0.5, 0.0], "restored": True,
                       "ctrl3_after": 0, "ctrl7_after": 1}, "gyro")
    t = doc["touch"]
    c.eq((t["max_fingers"], t["reads"], t["seen"], t["ack"]), (2, 17, True, 171), "touch")
    c.eq(t["raw"], "".join(f"{0xA0 + i:02x}" for i in range(15)), "touch raw hex")
    c.eq((t["driver_max_fingers"], t["sensorlib_max_fingers"]), (1, 2), "static caps")
    c.eq(doc["pmic"], {"ok": True, "adc_before": 1, "adc_during": 31, "adc_after": 1,
                       "adc_restored": True, "charge_cc_raw": 10, "charge_cc_ma": 400,
                       "die_c": 41.2, "vbus_mv": 5012, "vsys_mv": 4180,
                       "vbat_mv": 4090, "ts_raw": 1001, "ts_mv": 500.5}, "pmic")
    c.eq(doc["spi"], {"ok": True, "hz": 40000000, "source": "macro"}, "spi")

    rc, n, body = run(exe, "quiet")
    doc = json.loads(body)
    c.eq(doc["te"]["edges"], 0, "quiet edges")
    c.eq("no edges" in doc["te"]["note"], True, "zero edges says so")
    c.eq("changed" in doc["pwr_key"], False, "no watch, no change claim")
    c.eq("gyro" in doc["imu"], False, "no gyro key unless asked")
    c.eq(doc["imu"]["temp_c"], -0.5, "temp -5 tenths keeps its sign")
    c.eq(doc["pmic"]["die_c"], -0.7, "die -7 tenths keeps its sign")
    c.eq("raw" in doc["touch"], False, "no touch watch, no raw")

    rc, n, body = run(exe, "failed")
    doc = json.loads(body)
    for key, why in (("te", "isr add failed"), ("pwr_key", "gpio_set_direction failed"),
                     ("imu", "imu not present"), ("touch", "touch worker not running"),
                     ("pmic", "pmic not present")):
        c.eq(doc[key], {"ok": False, "why": why}, "failed " + key)
    c.eq(doc["spi"]["ok"], True, "spi survives failures")

    rc, n, body = run(exe, "refused")
    doc = json.loads(body)
    c.eq(doc["imu"]["gyro"], {"ran": False, "why": "refused: wake-on-motion armed"},
         "refused gyro")
    c.eq(doc["imu"]["wom"], True, "wom reported")

    rc, n, body = run(exe, "dirty")
    g = json.loads(body)["imu"]["gyro"]
    c.eq((g["restored"], g["ctrl3_after"]), (False, 86), "unrestored gyro is reported")

    rc, n, body = run(exe, "small")
    c.eq((rc, n), (0, -1), "overflow returns -1 and stays terminated")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mutation", choices=sorted(MUTANTS))
    ap.add_argument("--output-dir")
    a = ap.parse_args()
    text = SRC.read_text()
    if a.mutation:
        before, after = MUTANTS[a.mutation]
        assert before in text, "mutation target missing"
        text = text.replace(before, after)
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(a.output_dir) if a.output_dir else Path(tmp)
        exe = build(text, out)
        c = Checks()
        try:
            check(exe, c)
        except (AssertionError, json.JSONDecodeError, KeyError) as e:
            if a.mutation:
                print(f"mutation {a.mutation} caught: {e}")
                return 0
            print(f"FAIL: {e}")
            return 1
    if a.mutation:
        print(f"mutation {a.mutation} SURVIVED")
        return 1
    print(f"board json passed ({c.n} checks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
