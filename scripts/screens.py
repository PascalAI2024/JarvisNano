#!/usr/bin/env python3
"""Photograph every screen the device can show, into ONE labelled image.

Built because reviewing screens one at a time hides the problems that only
appear side by side. Every screen printing its own name twice, and four
unlabelled gauges on SETTINGS, were both invisible until seven tiles sat in a
grid together — and obvious within seconds once they did.

    JARVIS_DEVICE_HOST=<ip> python3 scripts/screens.py
    JARVIS_DEVICE_HOST=<ip> python3 scripts/screens.py --out /tmp/qa.png
    JARVIS_DEVICE_HOST=<ip> python3 scripts/screens.py --extras

Walks the mode ring with synthetic swipes, captures the panel mirror at each
stop, and tiles the results with captions. --extras also captures the states
that are not on the ring: controls open, and companion (operator) mode.

Needs Pillow, and the device reachable with diagnostics open — i.e.
JR_DEV_OPEN_DIAGNOSTICS 1, or a paired token in the environment.
"""
from __future__ import annotations

import argparse
import io
import os
import sys
import time
import urllib.error
import urllib.request

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("needs Pillow:  pip install pillow")

# The ring, in slide-down order. Keep in step with jr_display_space_t.
RING = ["JARVIS", "WATCH", "POWER", "MOTION", "DESK", "TOOLS", "SETTINGS"]
TILE = 466


def _req(host: str, path: str, method: str = "GET", timeout: float = 25.0):
    r = urllib.request.Request(
        f"http://{host}{path}", method=method,
        headers={"X-JarvisNano-Control": "1"},
        data=b"" if method == "POST" else None)
    return urllib.request.urlopen(r, timeout=timeout).read()


def post(host: str, path: str) -> bool:
    try:
        _req(host, path, "POST", 12.0)
        return True
    except Exception:
        return False


def grab(host: str, retries: int = 3):
    """One frame of the panel mirror. Retries: wifi power-save drops singles."""
    for attempt in range(retries):
        try:
            return Image.open(io.BytesIO(_req(host, "/api/display/snapshot.ppm")))
        except Exception:
            time.sleep(0.6 * (attempt + 1))
    return None


def caption(img, text, sub=""):
    """Label under the tile, so a grid is readable without counting positions."""
    out = Image.new("RGB", (TILE, TILE + 34), (12, 12, 12))
    out.paste(img, (0, 0))
    d = ImageDraw.Draw(out)
    d.text((10, TILE + 6), text, fill=(220, 220, 220))
    if sub:
        d.text((10 + 9 * len(text) + 12, TILE + 6), sub, fill=(120, 120, 120))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=os.environ.get("JARVIS_DEVICE_HOST", ""))
    ap.add_argument("--out", default="/tmp/jarvisnano-screens.png")
    ap.add_argument("--extras", action="store_true",
                    help="also capture controls-open and companion mode")
    ap.add_argument("--settle", type=float, default=1.2,
                    help="seconds to wait after each swipe (default 1.2)")
    a = ap.parse_args()
    if not a.host:
        print("set JARVIS_DEVICE_HOST or pass --host "
              "(find it with: arp -a | grep jarvisnano)", file=sys.stderr)
        return 2

    shots = []

    # Release any lease FIRST. A companion lease reserves gestures, so a sweep
    # taken under one used to return the same frame seven times — every tile
    # identical, which is how the swallowing bug was found in the first place.
    # Start from an un-inhabited body, then capture companion mode last.
    post(a.host, "/api/operator/lease?release=1")
    time.sleep(0.5)

    # Home next, so the sweep starts from a known screen rather than wherever
    # the last person left it. Double-tap is the global escape.
    post(a.host, "/api/debug/input?kind=double&x=233&y=233")
    time.sleep(a.settle)

    for i, name in enumerate(RING):
        if i:
            post(a.host, "/api/debug/input?kind=swipe&dir=down")
            time.sleep(a.settle)
        im = grab(a.host)
        if im is None:
            print(f"  {name}: CAPTURE FAILED")
            continue
        shots.append(caption(im, name, f"ring {i}"))
        print(f"  {name}: ok")

    if a.extras:
        post(a.host, "/api/debug/input?kind=double&x=233&y=233")
        time.sleep(a.settle)
        post(a.host, "/api/ui/shade?open=1")
        time.sleep(a.settle)
        im = grab(a.host)
        if im:
            shots.append(caption(im, "CONTROLS", "shade"))
            print("  CONTROLS: ok")
        post(a.host, "/api/ui/shade?open=0")
        time.sleep(a.settle)

        post(a.host, "/api/operator/lease?ttl=60")
        time.sleep(a.settle)
        im = grab(a.host)
        if im:
            shots.append(caption(im, "COMPANION", "operator lease"))
            print("  COMPANION: ok")
        post(a.host, "/api/operator/lease?release=1")

    if not shots:
        print("nothing captured — is the device reachable and dev mode on?")
        return 1

    cols = 4
    rows = (len(shots) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * TILE, rows * (TILE + 34)), (12, 12, 12))
    for i, s in enumerate(shots):
        sheet.paste(s, ((i % cols) * TILE, (i // cols) * (TILE + 34)))
    # Halve it: a 4-wide sheet of 466px tiles is unwieldy, and defects of the
    # kind this catches (duplicate labels, unlabelled gauges) survive the scale.
    sheet = sheet.resize((sheet.width // 2, sheet.height // 2))
    sheet.save(a.out)
    print(f"\n{len(shots)} screens -> {a.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
