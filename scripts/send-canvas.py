#!/usr/bin/env python3
"""Send any image to the JarvisNano glass.

Resizes/letterboxes to the round 466x466 panel, converts to RGB565
little-endian, and POSTs it to /api/display/canvas. The device shows it in
place of the face for the TTL, then the face returns.

    python3 scripts/send-canvas.py photo.png --host $JARVIS_DEVICE_HOST
    python3 scripts/send-canvas.py chart.jpg --ttl 60
    python3 scripts/send-canvas.py --clear

Requires Pillow (pip install pillow).
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
import urllib.request

SIZE = 466


def to_rgb565(path: str) -> bytes:
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit("Pillow required: pip3 install pillow")
    img = Image.open(path).convert("RGB")
    # cover-fit: fill the round panel, cropping overflow
    scale = max(SIZE / img.width, SIZE / img.height)
    img = img.resize((max(1, round(img.width * scale)),
                      max(1, round(img.height * scale))))
    left = (img.width - SIZE) // 2
    top = (img.height - SIZE) // 2
    img = img.crop((left, top, left + SIZE, top + SIZE))
    out = bytearray(SIZE * SIZE * 2)
    px = img.load()
    i = 0
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b = px[x, y]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            struct.pack_into("<H", out, i, v)
            i += 2
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", nargs="?", help="any image file Pillow can read")
    ap.add_argument("--host", default=os.environ.get("JARVIS_DEVICE_HOST"),
                    help="device host/IP (or $JARVIS_DEVICE_HOST)")
    ap.add_argument("--ttl", type=int, default=30,
                    help="seconds on screen (default 30, max 300)")
    ap.add_argument("--clear", action="store_true",
                    help="clear the canvas and restore the face")
    args = ap.parse_args()
    if not args.host:
        raise SystemExit("--host or JARVIS_DEVICE_HOST required")

    if args.clear:
        url = f"http://{args.host}/api/display/canvas?clear=1"
        req = urllib.request.Request(url, data=b"", method="POST")
    else:
        if not args.image:
            raise SystemExit("image path required (or --clear)")
        body = to_rgb565(args.image)
        url = (f"http://{args.host}/api/display/canvas"
               f"?ttl={max(1, args.ttl) * 1000}")
        req = urllib.request.Request(url, data=body, method="POST")
        req.add_header("Content-Type", "application/octet-stream")
    req.add_header("X-JarvisNano-Control", "1")
    with urllib.request.urlopen(req, timeout=30) as resp:
        print(resp.read().decode())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
