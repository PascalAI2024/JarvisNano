#!/usr/bin/env python3
"""gesture-doctor — diagnose "I can see it but I can't tap it" on the round glass.

Every check here exists because it was needed on 2026-08-29, when a 3-option ask
was on screen and the owner could not select any of them. Reading the source
suggested the arc geometry was broken; it was not. The device answered the
question in seconds once the right things were asked, in this order:

  1. is the panel producing events at all?          -> /api/touch counters
  2. are they the RIGHT KIND of event?              -> the tap:swipe ratio
  3. is anything actually hittable right now?       -> sweep the arc band
  4. does the classifier have a hole in it?         -> read the thresholds

Check 2 is the one a human eye skips and the one that mattered: 44 swipes to 12
taps, while the owner believed they were tapping. Check 3 must sweep the RIGHT
RADIUS — an earlier probe at r=190 returned "nothing is hittable anywhere" and
was simply inside the band, which looks exactly like a real defect.

Read-only. Uses only unauthenticated GET endpoints, so it works without the
pairing token that gates /api/cockpit and /api/logs.

    JARVIS_DEVICE_HOST=<ip> python3 scripts/gesture-doctor.py
    JARVIS_DEVICE_HOST=<ip> python3 scripts/gesture-doctor.py --watch
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
import time
import urllib.error
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOUCH_SRC = os.path.join(REPO, "components", "jr_hal", "src", "input_touch.c")
HUD_SRC = os.path.join(REPO, "components", "jr_display", "src", "hud_render.c")

OK, WARN, BAD = "ok  ", "WARN", "FAIL"


def get(base: str, path: str, timeout: float = 8.0, retries: int = 3):
    """GET with retries — ESP32 wifi power-save drops single requests routinely.
    One failed curl means nothing on this device; three in a row means something."""
    last = None
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(base + path, timeout=timeout) as r:
                return json.loads(r.read().decode("utf-8"))
        except (urllib.error.URLError, urllib.error.HTTPError, OSError,
                json.JSONDecodeError) as exc:
            last = exc
            time.sleep(0.4 * (attempt + 1))
    return {"__error": str(last)}


def defines(path: str, names: list[str]) -> dict[str, int]:
    """Read #define ints straight from source, so the report always describes
    the tree in front of you rather than remembered values."""
    out: dict[str, int] = {}
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return out
    for name in names:
        m = re.search(rf"^#define\s+{re.escape(name)}\s+(\d+)", text, re.M)
        if m:
            out[name] = int(m.group(1))
    return out


def check_thresholds() -> list[str]:
    """The classifier tries swipe FIRST and falls back to tap. Any gap between
    the two thresholds is a band where a real contact emits NOTHING."""
    d = defines(TOUCH_SRC, [
        "TOUCH_TAP_SLOP_PX", "TOUCH_SWIPE_MIN_TRAVEL_PX",
        "TOUCH_LONG_PRESS_MS", "TOUCH_HOLD_SLOP_PX", "TOUCH_TOP_EDGE_MAX_Y",
    ])
    lines = []
    tap, swipe = d.get("TOUCH_TAP_SLOP_PX"), d.get("TOUCH_SWIPE_MIN_TRAVEL_PX")
    if tap is None or swipe is None:
        return [f"{WARN} could not read thresholds from {TOUCH_SRC}"]
    if tap < swipe:
        lines.append(
            f"{BAD} DEAD BAND: drift {tap + 1}..{swipe - 1} px emits NO event "
            f"(tap slop {tap} < swipe min {swipe}). A real contact vanishes in "
            f"silence. Set TOUCH_TAP_SLOP_PX == TOUCH_SWIPE_MIN_TRAVEL_PX.")
    else:
        lines.append(f"{OK} no dead band (tap slop {tap} >= swipe min {swipe}) "
                     f"— every contact classifies as tap or swipe")
    edge = d.get("TOUCH_TOP_EDGE_MAX_Y")
    if edge:
        R = 233.0
        h = float(edge)
        cap = R * R * math.acos((R - h) / R) - (R - h) * math.sqrt(max(2 * R * h - h * h, 0))
        pct = 100.0 * cap / (math.pi * R * R)
        tag = WARN if pct < 15 else OK
        lines.append(f"{tag} shade top-edge band start_y<{edge} = {pct:.1f}% of "
                     f"the ROUND glass (a cap, not a strip — area falls off fast)")
    return lines


def arc_band() -> tuple[int, int, int]:
    d = defines(HUD_SRC, ["OV_CENTER", "OV_R_CHOICE_IN", "OV_R_CHOICE_OUT",
                          "OV_CHOICE_HIT_SLOP_IN", "OV_CHOICE_HIT_SLOP_OUT"])
    c = d.get("OV_CENTER", 232)
    lo = d.get("OV_R_CHOICE_IN", 223) - d.get("OV_CHOICE_HIT_SLOP_IN", 8)
    hi = d.get("OV_R_CHOICE_OUT", 231) + d.get("OV_CHOICE_HIT_SLOP_OUT", 24)
    return c, lo, hi


def sweep_arcs(base: str, step: int = 10) -> list[str]:
    """Sweep the MIDDLE of the hit band. Probing the wrong radius returns -1
    everywhere and is indistinguishable from a genuine defect."""
    c, lo, hi = arc_band()
    r = (lo + hi) // 2
    hits: dict[int, list[int]] = {}
    for deg in range(0, 360, step):
        a = math.radians(deg)
        x, y = int(c + r * math.cos(a)), int(c + r * math.sin(a))
        res = get(base, f"/api/display/choices/hit?x={x}&y={y}", timeout=6, retries=1)
        idx = res.get("index", -1) if isinstance(res, dict) else -1
        if isinstance(idx, int) and idx >= 0:
            hits.setdefault(idx, []).append(deg)
    if not hits:
        return [f"{OK} no ask active (swept r={r}, band {lo}..{hi}) — nothing to hit, "
                f"which is correct when no question is on screen"]
    out = [f"{OK} ask ACTIVE — {len(hits)} choice(s) hittable at r={r}:"]
    for idx in sorted(hits):
        degs = hits[idx]
        out.append(f"       index {idx}: {len(degs) * step} deg of arc "
                   f"({degs[0]}..{degs[-1]} deg)")
    out.append("       => geometry and ask state are GOOD. If the owner still "
               "cannot select, the fault is upstream: the contact is not "
               "arriving as a tap. See the tap:swipe ratio above.")
    return out


def check_touch(base: str) -> list[str]:
    t = get(base, "/api/touch")
    if "__error" in t:
        return [f"{BAD} /api/touch unreachable: {t['__error']}"]
    ev, taps = t.get("events", 0), t.get("taps", 0)
    swipes, longs = t.get("swipes", 0), t.get("long_presses", 0)
    lines = [f"{OK} panel producing events: total={ev} taps={taps} "
             f"swipes={swipes} long={longs} shade_open={t.get('shade_open')}"]
    if ev == 0:
        lines.append(f"{WARN} zero events — touch the glass before trusting "
                     f"this report (counters reset on boot)")
    elif swipes > taps * 2 and swipes >= 6:
        lines.append(
            f"{BAD} SWIPE-HEAVY {swipes}:{taps}. Intended taps are being "
            f"classified as swipes — on the rim a press always rolls, and "
            f"swipe is tested BEFORE tap, so the stroke navigates instead of "
            f"selecting. This is the signature of 'I can see it but I can't "
            f"tap it'.")
    last = t.get("last") or {}
    if last:
        lines.append(f"       last: kind={last.get('kind')} at "
                     f"({last.get('x')},{last.get('y')}) "
                     f"delta=({last.get('dx')},{last.get('dy')}) "
                     f"{last.get('duration_ms')}ms")
    return lines


def check_display(base: str) -> list[str]:
    d = get(base, "/api/display")
    if "__error" in d:
        return [f"{BAD} /api/display unreachable: {d['__error']}"]
    faces = ["IDLE", "LISTENING", "THINKING", "SPEAKING", "ERROR"]
    face = d.get("applied_face")
    name = faces[face] if isinstance(face, int) and 0 <= face < len(faces) else face
    fps = d.get("actual_fps")
    lines = [f"{OK} display {d.get('init')} face={name} fps={fps} "
             f"flush_errors={d.get('flush_errors')}"]
    if isinstance(fps, int) and fps < 8:
        lines.append(f"{WARN} fps {fps} is low — the render task may be starved")
    if d.get("flush_errors"):
        lines.append(f"{BAD} panel flush errors present")
    return lines


def run(base: str) -> int:
    print(f"gesture-doctor -> {base}\n")
    sections = [
        ("reachability + display", lambda: check_display(base)),
        ("touch events (is the gesture the RIGHT KIND?)", lambda: check_touch(base)),
        ("classifier thresholds (source of truth: the tree)", check_thresholds),
        ("live arc hit sweep (is anything hittable NOW?)", lambda: sweep_arcs(base)),
    ]
    worst = 0
    for title, fn in sections:
        print(f"── {title}")
        for line in fn():
            print("  " + line)
            if line.startswith(BAD):
                worst = 2
            elif line.startswith(WARN):
                worst = max(worst, 1)
        print()
    print("verdict:", ["healthy", "warnings", "DEFECT FOUND"][worst])
    return worst


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=os.environ.get("JARVIS_DEVICE_HOST", ""),
                    help="device IP or host (or set JARVIS_DEVICE_HOST)")
    ap.add_argument("--watch", action="store_true",
                    help="re-run every 10 s so you can touch the glass and watch "
                         "the counters move")
    args = ap.parse_args()
    if not args.host:
        print("set JARVIS_DEVICE_HOST or pass --host "
              "(find it with: arp -a | grep jarvisnano)", file=sys.stderr)
        return 2
    base = args.host if args.host.startswith("http") else "http://" + args.host
    base = base.rstrip("/")
    if not args.watch:
        return run(base)
    try:
        while True:
            run(base)
            print("─" * 60)
            time.sleep(10)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
