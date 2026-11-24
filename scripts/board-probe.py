#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Ask the device what its idle hardware really does: GET /api/diag/board.

  JARVIS_DEVICE_HOST=<ip> scripts/board-probe.py              # one pass, ~1 s
  scripts/board-probe.py --gyro                               # gyro on for <1 s
  scripts/board-probe.py --watch pwr                          # press PWR within 3 s
  scripts/board-probe.py --watch touch                        # two fingers on the glass for 4 s
  scripts/board-probe.py --json                               # the raw object

Host, pairing token and the control header come from jarvisctl.py, so the
rules are the same as every other desk command. The address and the token are
never printed. Exit 0 when the route answered, 1 when it did not, 2 on usage.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jarvisctl  # noqa: E402


def fetch(watch: str | None, gyro: bool) -> dict:
    query = []
    if watch:
        query.append("watch=" + watch)
    if gyro:
        query.append("gyro=1")
    path = "/api/diag/board" + ("?" + "&".join(query) if query else "")
    # The route holds the connection for up to ~5 s; leave room.
    return json.loads(jarvisctl.api(path, timeout=20))


def cell(block: dict, key: str, unit: str = "") -> str:
    return f"{block[key]}{unit}" if key in block else "-"


def rows(doc: dict) -> list[tuple[str, str, str]]:
    out: list[tuple[str, str, str]] = []

    def add(name: str, block: dict, detail: str) -> None:
        out.append((name, "ok" if block.get("ok") else "FAIL",
                    detail if block.get("ok") else block.get("why", "?")))

    te = doc["te"]
    add("te", te, (f"{te.get('edges')} edges in {te.get('window_ms')} ms = "
                   f"{te.get('hz')} Hz, period {te.get('period_us_min')}-"
                   f"{te.get('period_us_max')} us") if te.get("ok") else "")
    if te.get("ok") and te.get("edges") == 0:
        out.append(("", "", te["note"]))

    pk = doc["pwr_key"]
    if pk.get("ok") and pk.get("watched"):
        d = (f"now {pk['level']}; {pk['transitions']} changes in {pk['watch_ms']} ms, "
             f"high {pk['high_ms']} ms (min {pk['level_min']} max {pk['level_max']})")
    else:
        d = f"now {pk.get('level')} (add --watch pwr and press the key)"
    add("pwr_key", pk, d)

    im = doc["imu"]
    d = ""
    if im.get("ok"):
        d = (f"who 0x{im['who_am_i']:02x} rev 0x{im['revision']:02x} "
             f"ctrl1/2/3/7 {im['ctrl1']:#04x}/{im['ctrl2']:#04x}/"
             f"{im['ctrl3']:#04x}/{im['ctrl7']:#04x} temp {cell(im, 'temp_c', ' C')}"
             f"{' WoM' if im.get('wom') else ''}")
    add("imu", im, d)
    g = im.get("gyro")
    if g:
        if g.get("ran"):
            out.append(("", "", "gyro dps min " + str(g["min_dps"]) + " max " +
                        str(g["max_dps"]) + " mean " + str(g["mean_dps"])))
            out.append(("", "", f"gyro {g['samples']} samples, restored={g['restored']} "
                        f"ctrl3 {g['ctrl3_after']:#04x} ctrl7 {g['ctrl7_after']:#04x}"))
        else:
            out.append(("", "", "gyro not run: " + g.get("why", "?")))

    t = doc["touch"]
    if t.get("ok") and t.get("watched"):
        d = (f"{t['reads']} raw reads in {t['watch_ms']} ms, max fingers "
             f"{t['max_fingers']}")
        if t.get("seen"):
            d += f", ack 0x{t['ack']:02x}, raw {t['raw']}"
    else:
        d = "driver reads 1 finger, SensorLib reads 2 (add --watch touch)"
    add("touch", t, d)

    p = doc["pmic"]
    d = ""
    if p.get("ok"):
        parts = [f"die {cell(p, 'die_c', ' C')}", f"vbus {cell(p, 'vbus_mv', ' mV')}",
                 f"vsys {cell(p, 'vsys_mv', ' mV')}", f"vbat {cell(p, 'vbat_mv', ' mV')}",
                 f"ts {cell(p, 'ts_mv', ' mV')}",
                 f"charge reg 0x62 = {p['charge_cc_raw']:#04x}"
                 + (f" ({p['charge_cc_ma']} mA)" if "charge_cc_ma" in p else ""),
                 f"adc {p['adc_before']:#04x}->{p['adc_during']:#04x}->"
                 f"{p['adc_after']:#04x} restored={p['adc_restored']}"]
        d = ", ".join(parts)
    add("pmic", p, d)

    s = doc["spi"]
    add("spi", s, f"{s.get('hz', 0) / 1e6:g} MHz ({s.get('source')})")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", help="device address (default $JARVIS_DEVICE_HOST)")
    ap.add_argument("--watch", choices=("pwr", "touch"),
                    help="hold the request open for 3 s (pwr) or 4 s (touch)")
    ap.add_argument("--gyro", action="store_true",
                    help="switch the gyro on for under a second and sample it")
    ap.add_argument("--json", action="store_true", help="print the raw object")
    args = ap.parse_args()
    try:
        doc = fetch(args.watch, args.gyro)
    except urllib.error.HTTPError as e:
        print(f"board-probe: device answered HTTP {e.code}", file=sys.stderr)
        return 1
    except (urllib.error.URLError, OSError, ValueError) as e:
        print(f"board-probe: no usable answer ({type(e).__name__})", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(doc, indent=2))
        return 0
    width = max(len(r[0]) for r in rows(doc))
    for name, state, detail in rows(doc):
        print(f"{name:<{width}}  {state:<4}  {detail}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
