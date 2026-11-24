#!/usr/bin/env python3
"""Photograph every screen the device can show, into ONE labelled QA sheet.

Screens 2.0:
  - Uses deterministic `POST /api/glass?screen=<name>` navigation (verifying
    `space` against `/api/display`) instead of fragile swipe loops.
  - Captures via fast `/api/display/snapshot.rgb565` (434 KB) with Pillow
    RGB565 decoding, falling back cleanly to `/api/display/snapshot.ppm`.
  - Supports `--modes` (ON AIR, FOCUS, BREATHE, INBOX), `--dials` (all 10
    watch faces), and `--extras` (CONTROLS shade, COMPANION operator lease).
  - Stamps compact per-tile FPS, ms/f, and top layer_us onto each caption and
    prints a Markdown performance summary table.

    JARVIS_DEVICE_HOST=<ip> python3 scripts/screens.py
    JARVIS_DEVICE_HOST=<ip> python3 scripts/screens.py --dials --modes --out qa.png
    JARVIS_DEVICE_HOST=<ip> python3 scripts/screens.py --extras

Needs Pillow, and the device reachable with a paired token or dev diagnostics.
"""
from __future__ import annotations

import argparse
import io
import json
import os
import re
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("needs Pillow:  pip install pillow")

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import jarvisctl  # noqa: E402

TILE = 466
CAPTION_H = 44
LAYER_NAMES = ("dial", "canvas", "hud", "shell", "surface")
_HEADER = "components/jr_display/include/jr_display/jr_display.h"

# Core screens in deterministic capture order: (display label, /api/glass screen param, enum name)
CORE_SCREENS: tuple[tuple[str, str, str], ...] = (
    ("HOME", "home", "JARVIS"),
    ("WATCH", "watch", "WATCH"),
    ("WEATHER", "weather", "WEATHER"),
    ("STATUS", "status", "STATUS"),
    ("ACTIVITY", "activity", "ACTIVITY"),
    ("DESK", "desk", "DESK"),
)


def ring_from_header(root: str) -> list[str]:
    """Screen names in enum order, read from jr_display_space_t itself."""
    with open(os.path.join(root, _HEADER), encoding="utf-8") as fh:
        src = fh.read()
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


def normalize_host(value: str) -> str:
    return jarvisctl.normalize_host(value)


def pairing_token(host: str) -> str | None:
    return jarvisctl.load_pairing_token(host)


def _req(host: str, path: str, method: str = "GET",
         timeout: float = 25.0) -> tuple[dict[str, str], bytes]:
    base = normalize_host(host)
    headers = {"X-JarvisNano-Control": "1"}
    token = pairing_token(base)
    if token:
        headers["X-JarvisNano-Token"] = token
    req = urllib.request.Request(
        f"{base}{path}",
        method=method,
        headers=headers,
        data=b"" if method == "POST" else None,
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        hdrs = dict(resp.headers.items()) if resp.headers else {}
        return hdrs, resp.read()


def _header_ci(headers: dict[str, str], name: str, default: str = "") -> str:
    needle = name.lower()
    for k, v in headers.items():
        if k.lower() == needle:
            return v
    return default


def get_json(host: str, path: str, timeout: float = 10.0) -> dict[str, Any]:
    _, raw = _req(host, path, "GET", timeout=timeout)
    return json.loads(raw.decode("utf-8"))


class SweepError(RuntimeError):
    """A control call the sweep depends on did not take effect."""


def post(host: str, path: str, required: bool = True) -> bool:
    """POST a control call. Loud by default unless required=False."""
    try:
        _req(host, path, "POST", 12.0)
        return True
    except Exception as exc:  # noqa: BLE001
        if required:
            raise SweepError(f"{path} failed: {exc}") from exc
        print(f"  (optional call failed: {path}: {exc})")
        return False


def grab(host: str, retries: int = 3) -> Image.Image | None:
    """Capture one frame via fast /api/display/snapshot.rgb565 with PPM fallback."""
    for attempt in range(retries):
        try:
            hdrs, raw = _req(host, "/api/display/snapshot.rgb565", "GET", timeout=20.0)
            w = int(_header_ci(hdrs, "X-Jarvis-Display-Width", str(TILE)) or TILE)
            h = int(_header_ci(hdrs, "X-Jarvis-Display-Height", str(TILE)) or TILE)
            if len(raw) == w * h * 2:
                return jarvisctl.rgb565_to_pil(raw, w, h, big_endian=True)
        except Exception:  # noqa: BLE001
            pass
        try:
            _, ppm_raw = _req(host, "/api/display/snapshot.ppm", "GET", timeout=25.0)
            return Image.open(io.BytesIO(ppm_raw)).convert("RGB")
        except Exception:  # noqa: BLE001
            time.sleep(0.5 * (attempt + 1))
    return None


def sample_display_metrics(host: str) -> dict[str, Any]:
    """Read /api/display telemetry for per-tile captioning and perf summary."""
    try:
        d = get_json(host, "/api/display", timeout=8.0)
    except Exception:  # noqa: BLE001
        return {}
    layers_raw = d.get("layer_us") or [0, 0, 0, 0, 0]
    layers = [int(x) for x in (list(layers_raw) + [0, 0, 0, 0, 0])[:5]]
    top_idx = max(range(5), key=lambda i: layers[i])
    return {
        "space": d.get("space"),
        "fps": int(d.get("actual_fps") or 0),
        "frame_us": int(d.get("render_frame_us") or 0),
        "frame_ms": (int(d.get("render_frame_us") or 0)) / 1000.0,
        "layer_us": layers,
        "top_layer": LAYER_NAMES[top_idx],
        "top_layer_ms": layers[top_idx] / 1000.0,
        "flush_errors": int(d.get("flush_errors") or 0),
    }


def format_perf_stamp(metrics: dict[str, Any]) -> str:
    if not metrics:
        return "metrics n/a"
    return (
        f"{metrics['fps']}fps  {metrics['frame_ms']:.1f}ms/f  "
        f"{metrics['top_layer']}:{metrics['top_layer_ms']:.1f}ms"
    )


def caption(img: Image.Image, text: str, sub: str = "",
            metrics: dict[str, Any] | None = None) -> Image.Image:
    """Two-line tile footer: screen label + compact FPS/ms/top-layer telemetry."""
    if img.size != (TILE, TILE):
        img = img.resize((TILE, TILE))
    out = Image.new("RGB", (TILE, TILE + CAPTION_H), (12, 12, 12))
    out.paste(img, (0, 0))
    d = ImageDraw.Draw(out)
    d.text((10, TILE + 5), text, fill=(235, 240, 248))
    if sub:
        d.text((10 + 7 * len(text) + 14, TILE + 5), sub, fill=(135, 145, 160))
    if metrics:
        stamp = format_perf_stamp(metrics)
        fps = metrics.get("fps", 0)
        color = (120, 220, 255) if fps >= 14 else ((255, 196, 80) if fps >= 8 else (255, 100, 110))
        d.text((10, TILE + 23), stamp, fill=color)
    return out


def format_markdown_table(rows: list[dict[str, Any]]) -> str:
    """Render a clean Markdown performance summary table for captured tiles."""
    lines = [
        "| Tile | Space | FPS | ms/f | Top Layer | dial | canvas | hud | shell | surface |",
        "| --- | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for r in rows:
        m = r.get("metrics") or {}
        layers = m.get("layer_us") or [0, 0, 0, 0, 0]
        sp = m.get("space", "-")
        fps = m.get("fps", "-")
        fms = f"{m['frame_ms']:.1f}" if "frame_ms" in m else "-"
        top = f"{m['top_layer']} ({m['top_layer_ms']:.1f}ms)" if "top_layer" in m else "-"
        l_cols = " | ".join(f"{x / 1000.0:.1f}" for x in layers[:5])
        lines.append(f"| {r['name']} | {sp} | {fps} | {fms} | {top} | {l_cols} |")
    return "\n".join(lines)


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--host", default=os.environ.get("JARVIS_DEVICE_HOST", ""),
                    help="device host/IP (or $JARVIS_DEVICE_HOST)")
    ap.add_argument("--out", default=os.path.join(tempfile.gettempdir(),
                                                  "jarvisnano-screens.png"),
                    help="output contact-sheet PNG path")
    ap.add_argument("--extras", action="store_true",
                    help="also capture controls shade and companion operator mode")
    ap.add_argument("--modes", action="store_true",
                    help="also capture Desk Copilot modes: ON AIR, FOCUS, BREATHE, INBOX")
    ap.add_argument("--dials", action="store_true",
                    help="also capture all 10 watch faces via POST /api/glass?style=...")
    ap.add_argument("--settle", type=float, default=1.0,
                    help="seconds to wait after each screen/mode change (default 1.0)")
    return ap


def main(argv: list[str] | None = None) -> int:
    jarvisctl.ensure_utf8_console()
    a = build_parser().parse_args(argv)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ring = ring_from_header(root)
    space_index = {name: idx for idx, name in enumerate(ring)}
    print(f"ring ({len(ring)} spaces from jr_display_space_t): {', '.join(ring)}")

    if not a.host:
        print("set JARVIS_DEVICE_HOST or pass --host (the address is on the "
              "device's STATUS screen, or in your router's client list as "
              "jarvisnano)", file=sys.stderr)
        return 2

    # Record initial screen & watch style to restore cleanly when finished.
    orig_space = 0
    orig_style: str | None = None
    try:
        orig_space = int(get_json(a.host, "/api/display").get("space", 0))
    except Exception:  # noqa: BLE001
        pass
    try:
        orig_style = (get_json(a.host, "/api/cockpit").get("display") or {}).get("watch_style")
    except Exception:  # noqa: BLE001
        pass

    shots: list[Image.Image] = []
    perf_rows: list[dict[str, Any]] = []

    def record_tile(label: str, sub: str, metrics: dict[str, Any] | None = None) -> bool:
        m = metrics if metrics is not None else sample_display_metrics(a.host)
        im = grab(a.host)
        if im is None:
            print(f"  {label}: CAPTURE FAILED")
            return False
        shots.append(caption(im, label, sub, m))
        perf_rows.append({"name": label, "sub": sub, "metrics": m})
        print(f"  {label:<18} ok  ({format_perf_stamp(m)})")
        return True

    try:
        # Start from a clean, non-leased, shade-closed state.
        post(a.host, "/api/operator/lease?release=1", required=False)
        post(a.host, "/api/ui/shade?action=close", required=False)
        post(a.host, "/api/desk/mode?mode=normal", required=False)
        time.sleep(0.3)

        # 1. Deterministic core screens via POST /api/glass?screen=<name>
        for label, screen_param, enum_name in CORE_SCREENS:
            expected_sp = space_index.get(enum_name)
            ok = post(a.host, f"/api/glass?screen={screen_param}", required=(screen_param != "desk"))
            if not ok:
                print(f"  {label:<18} skipped (/api/glass?screen={screen_param} unsupported)")
                continue
            time.sleep(a.settle)
            m = sample_display_metrics(a.host)
            if expected_sp is not None and m.get("space") != expected_sp:
                # Retry once in case an animation or lease delayed the switch
                post(a.host, f"/api/glass?screen={screen_param}", required=False)
                time.sleep(a.settle)
                m = sample_display_metrics(a.host)
                if m.get("space") != expected_sp:
                    print(f"  WARN {label}: expected space={expected_sp}, got space={m.get('space')}")
            record_tile(label, f"space {m.get('space', '?')}", m)

        # 2. Optional Desk Copilot 3.5 modes (--modes)
        if a.modes:
            mode_specs = (
                ("ON AIR", "/api/desk/mode?mode=on_air&minutes=30", "home", "desk mode"),
                ("FOCUS", "/api/desk/mode?mode=focus&minutes=25", "home", "desk mode"),
                ("BREATHE", "/api/desk/mode?mode=breathe&pattern=box&cycles=4", "home", "desk mode"),
                ("INBOX", "/api/desk/mode?mode=normal", "activity", "desk inbox"),
            )
            for label, mode_path, screen_param, sub in mode_specs:
                post(a.host, f"/api/glass?screen={screen_param}", required=False)
                if not post(a.host, mode_path, required=False):
                    continue
                time.sleep(a.settle)
                record_tile(label, sub)
            post(a.host, "/api/desk/mode?mode=normal", required=False)

        # 3. Optional 10 watch faces (--dials)
        if a.dials:
            post(a.host, "/api/glass?screen=watch", required=True)
            for style in jarvisctl.WATCH_STYLES:
                if not post(a.host, f"/api/glass?screen=watch&style={style}", required=False):
                    continue
                time.sleep(a.settle)
                record_tile(f"DIAL:{style.upper()}", "watch", None)

        # 4. Optional off-ring extras (--extras)
        if a.extras:
            post(a.host, "/api/glass?screen=home", required=False)
            time.sleep(a.settle * 0.5)
            post(a.host, "/api/ui/shade?action=open", required=True)
            time.sleep(a.settle)
            record_tile("CONTROLS", "shade")
            post(a.host, "/api/ui/shade?action=close", required=True)
            time.sleep(a.settle * 0.5)

            post(a.host, "/api/operator/lease?ttl=60", required=True)
            time.sleep(a.settle)
            record_tile("COMPANION", "operator lease")
            post(a.host, "/api/operator/lease?release=1", required=False)
    finally:
        # Always restore device state cleanly even on error or Ctrl+C.
        if a.modes:
            post(a.host, "/api/desk/mode?mode=normal", required=False)
        if a.dials and orig_style:
            post(a.host, f"/api/glass?style={orig_style.lower()}", required=False)
        restore_scr = jarvisctl.SPACE_INDEX_TO_SCREEN.get(orig_space, "home")
        post(a.host, f"/api/glass?screen={restore_scr}", required=False)

    if not shots:
        print("nothing captured — is the device reachable and paired?")
        return 1

    cols = 4
    rows = (len(shots) + cols - 1) // cols
    tile_h = TILE + CAPTION_H
    sheet = Image.new("RGB", (cols * TILE, rows * tile_h), (12, 12, 12))
    for i, s in enumerate(shots):
        sheet.paste(s, ((i % cols) * TILE, (i // cols) * tile_h))
    sheet = sheet.resize((sheet.width // 2, sheet.height // 2))
    sheet.save(a.out)
    print(f"\n{len(shots)} tiles -> {a.out}\n")
    print(format_markdown_table(perf_rows))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
