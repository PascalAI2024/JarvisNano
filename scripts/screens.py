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

TILE = 466

# The ring, in slide-down order, PARSED FROM THE FIRMWARE ENUM rather than
# copied. A hand-kept copy drifted: it still listed a "MOTION" screen that the
# firmware does not have, so every tile from the fourth on was captioned with
# the wrong name and the sheet looked like the ring was skipping a screen and
# wrapping early. A QA tool that mislabels its own evidence is worse than no
# tool, because it manufactures bugs that were never in the firmware.
_HEADER = "components/jr_display/include/jr_display/jr_display.h"


def ring_from_header(root: str) -> list[str]:
    """Screen names in enum order, read from jr_display_space_t itself."""
    import re
    src = open(os.path.join(root, _HEADER)).read()
    # Anchor on the enum's CLOSING tag and walk back to its own opening brace.
    # Searching forward from the first "typedef enum {" instead swept in the
    # #defines above it (JR_DISPLAY_SPACE_MS, _SPACE_HOLD_MS), which share the
    # JR_DISPLAY_SPACE_ prefix but are not screens.
    end = src.find("} jr_display_space_t;")
    if end < 0:
        raise SystemExit(f"could not find jr_display_space_t in {_HEADER}")
    start = src.rfind("typedef enum {", 0, end)
    if start < 0:
        raise SystemExit(f"malformed jr_display_space_t in {_HEADER}")
    names = re.findall(r"JR_DISPLAY_SPACE_([A-Z0-9_]+)", src[start:end])
    ring = [n for n in names if n != "COUNT"]
    if not ring:
        raise SystemExit("parsed zero screens from jr_display_space_t")
    return ring


def _req(host: str, path: str, method: str = "GET", timeout: float = 25.0):
    r = urllib.request.Request(
        f"http://{host}{path}", method=method,
        headers={"X-JarvisNano-Control": "1"},
        data=b"" if method == "POST" else None)
    return urllib.request.urlopen(r, timeout=timeout).read()


class SweepError(RuntimeError):
    """A control call the sweep depends on did not take effect."""


def post(host: str, path: str, required: bool = True) -> bool:
    """POST a control call. LOUD by default.

    This used to swallow every failure and return False, which nothing
    checked. The shade call had the wrong parameter for a while (the handler
    wants ?action=open, not ?open=1), so the shade never opened, the sweep
    captured the plain JARVIS screen, captioned it CONTROLS, and presented it
    as a rendering defect. A silent failure in a QA tool does not leave a gap
    in the evidence; it produces confident WRONG evidence, which is worse.
    """
    try:
        _req(host, path, "POST", 12.0)
        return True
    except Exception as exc:
        if required:
            raise SweepError(f"{path} failed: {exc}") from exc
        print(f"  (optional call failed: {path}: {exc})")
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
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ring = ring_from_header(root)
    print(f"ring ({len(ring)} screens, from jr_display_space_t): {', '.join(ring)}")
    if not a.host:
        print("set JARVIS_DEVICE_HOST or pass --host "
              "(find it with: arp -a | grep jarvisnano)", file=sys.stderr)
        return 2

    shots = []

    # Release any lease FIRST. A companion lease reserves gestures, so a sweep
    # taken under one used to return the same frame seven times — every tile
    # identical, which is how the swallowing bug was found in the first place.
    # Start from an un-inhabited body, then capture companion mode last.
    post(a.host, "/api/operator/lease?release=1", required=False)
    time.sleep(0.5)

    # Home next, so the sweep starts from a known screen rather than wherever
    # the last person left it. Double-tap is the global escape.
    post(a.host, "/api/debug/input?kind=double&x=233&y=233")
    time.sleep(a.settle)

    for i, name in enumerate(ring):
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
        post(a.host, "/api/ui/shade?action=open")
        time.sleep(a.settle)
        im = grab(a.host)
        if im:
            shots.append(caption(im, "CONTROLS", "shade"))
            print("  CONTROLS: ok")
        post(a.host, "/api/ui/shade?action=close")
        time.sleep(a.settle)

        post(a.host, "/api/operator/lease?ttl=60")
        time.sleep(a.settle)
        im = grab(a.host)
        if im:
            shots.append(caption(im, "COMPANION", "operator lease"))
            print("  COMPANION: ok")
        post(a.host, "/api/operator/lease?release=1", required=False)

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
