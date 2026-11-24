#!/usr/bin/env python3
"""Send any image or procedural OLED banner to the JarvisNano glass.

Resizes/cover-crops to the round 466x466 panel, converts to RGB565
little-endian, and POSTs it to /api/display/canvas. The device shows it in
place of the face for the TTL, then the face returns.

    python3 scripts/send-canvas.py photo.png --host $JARVIS_DEVICE_HOST
    python3 scripts/send-canvas.py chart.jpg --ttl 60 --takeover
    python3 scripts/send-canvas.py --banner "DEEP FOCUS" --sub "Back at 3:00 PM" --accent cyan
    python3 scripts/send-canvas.py --clear

Requires Pillow (pip install pillow).
"""
from __future__ import annotations

import argparse
import math
import os
import struct
import sys
import urllib.error
import urllib.request
from typing import Sequence

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import jarvisctl  # noqa: E402

SIZE = 466
ACCENTS: dict[str, dict[str, tuple[int, int, int]]] = {
    "cyan": {
        "primary": (34, 220, 255),
        "bright": (196, 248, 255),
        "glow": (12, 72, 98),
        "deep": (5, 22, 34),
    },
    "violet": {
        "primary": (176, 108, 255),
        "bright": (236, 216, 255),
        "glow": (70, 32, 118),
        "deep": (24, 10, 44),
    },
    "amber": {
        "primary": (255, 159, 46),
        "bright": (255, 228, 186),
        "glow": (106, 56, 12),
        "deep": (38, 18, 4),
    },
    "mint": {
        "primary": (70, 240, 165),
        "bright": (204, 252, 230),
        "glow": (18, 96, 64),
        "deep": (6, 34, 22),
    },
    "crimson": {
        "primary": (255, 74, 98),
        "bright": (255, 206, 214),
        "glow": (122, 22, 38),
        "deep": (44, 6, 14),
    },
}


def load_pairing_token(host: str) -> str | None:
    """Reuse jarvisctl's host-bound pairing token lookup without exposing it."""
    return jarvisctl.load_pairing_token(host)


def build_request(
    base_url: str,
    path: str,
    *,
    data: bytes = b"",
    token: str | None = None,
    content_type: str | None = None,
) -> urllib.request.Request:
    req = urllib.request.Request(base_url + path, data=data, method="POST")
    req.add_header("X-JarvisNano-Control", "1")
    if token:
        req.add_header("X-JarvisNano-Token", token)
    if content_type:
        req.add_header("Content-Type", content_type)
    return req


def image_to_rgb565(img) -> bytes:
    """Convert any Pillow Image to 466x466 RGB565 little-endian bytes."""
    rgb = img.convert("RGB")
    if rgb.size != (SIZE, SIZE):
        from PIL import Image

        scale = max(SIZE / rgb.width, SIZE / rgb.height)
        rgb = rgb.resize(
            (max(1, round(rgb.width * scale)), max(1, round(rgb.height * scale))),
            Image.Resampling.LANCZOS,
        )
        left = (rgb.width - SIZE) // 2
        top = (rgb.height - SIZE) // 2
        rgb = rgb.crop((left, top, left + SIZE, top + SIZE))
    raw = rgb.tobytes()
    out = bytearray(SIZE * SIZE * 2)
    pack = struct.pack_into
    j = 0
    for i in range(0, len(raw), 3):
        v = ((raw[i] & 0xF8) << 8) | ((raw[i + 1] & 0xFC) << 3) | (raw[i + 2] >> 3)
        pack("<H", out, j, v)
        j += 2
    return bytes(out)


def to_rgb565(path: str) -> bytes:
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit("Pillow required: pip3 install pillow")
    return image_to_rgb565(Image.open(path))


def _load_font(size: int, *, bold: bool = False):
    from PIL import ImageFont

    candidates = (
        [
            "C:/Windows/Fonts/segoeuib.ttf",
            "C:/Windows/Fonts/arialbd.ttf",
            "/System/Library/Fonts/SFNSDisplay-Bold.otf",
            "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        ]
        if bold
        else [
            "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/arial.ttf",
            "/System/Library/Fonts/SFNS.ttf",
            "/System/Library/Fonts/Supplemental/Arial.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        ]
    )
    for font_path in candidates:
        if os.path.exists(font_path):
            try:
                return ImageFont.truetype(font_path, size)
            except OSError:
                continue
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


def _wrap_text(draw, text: str, font, max_width: int, max_lines: int = 2) -> list[str]:
    words = text.strip().split()
    if not words:
        return []
    lines: list[str] = []
    cur = words[0]
    for word in words[1:]:
        trial = f"{cur} {word}"
        box = draw.textbbox((0, 0), trial, font=font)
        if (box[2] - box[0]) <= max_width:
            cur = trial
        else:
            lines.append(cur)
            cur = word
            if len(lines) == max_lines - 1:
                break
    remaining = words[sum(len(l.split()) for l in lines) :]
    if len(lines) < max_lines:
        tail = " ".join(remaining) if lines else cur
        if lines and len(remaining) > 1:
            tail = " ".join(remaining)
        else:
            tail = cur
        while len(tail) > 3:
            box = draw.textbbox((0, 0), tail, font=font)
            if (box[2] - box[0]) <= max_width:
                break
            tail = tail[:-2].rstrip() + "…"
        lines.append(tail)
    return lines[:max_lines]


def render_banner_image(title: str, sub: str = "", accent: str = "cyan"):
    """Render a crisp 466x466 luxury OLED circular graphic card (2x supersampled)."""
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        raise SystemExit("Pillow required: pip3 install pillow")

    pal = ACCENTS.get(accent.lower(), ACCENTS["cyan"])
    pri = pal["primary"]
    bri = pal["bright"]
    glw = pal["glow"]
    deep = pal["deep"]

    ss = SIZE * 2  # 932x932 supersample for razor-sharp anti-aliased rings
    cx = cy = ss // 2
    img = Image.new("RGB", (ss, ss), (0, 0, 0))
    draw = ImageDraw.Draw(img)

    # True OLED radial ambient backdrop (pure black rim, luminous core gradient)
    max_r = int(ss * 0.485)
    for step in range(28, 0, -1):
        frac = step / 28.0
        r = int(max_r * frac)
        inv = (1.0 - frac) ** 1.65
        col = (
            int(deep[0] * inv * 1.15 + glw[0] * (inv ** 2) * 0.65),
            int(deep[1] * inv * 1.15 + glw[1] * (inv ** 2) * 0.65),
            int(deep[2] * inv * 1.15 + glw[2] * (inv ** 2) * 0.65),
        )
        draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=col)

    # Outer precision luminous halo + bezel rings
    for offset, col, width in (
        (16, (glw[0] // 2, glw[1] // 2, glw[2] // 2), 14),
        (24, glw, 8),
        (28, pri, 4),
        (46, (glw[0], glw[1], glw[2]), 2),
        (74, (deep[0] + glw[0] // 3, deep[1] + glw[1] // 3, deep[2] + glw[2] // 3), 2),
    ):
        draw.ellipse(
            (offset, offset, ss - offset, ss - offset),
            outline=col,
            width=width,
        )

    # Chronometer radial ticks around the perimeter
    r_outer = cx - 48
    for deg in range(0, 360, 6):
        rad = math.radians(deg)
        major = deg % 30 == 0
        cardinal = deg % 90 == 0
        length = 22 if cardinal else (14 if major else 7)
        w = 4 if cardinal else (2 if major else 1)
        col = bri if cardinal else (pri if major else glw)
        x0 = cx + math.cos(rad) * (r_outer - length)
        y0 = cy + math.sin(rad) * (r_outer - length)
        x1 = cx + math.cos(rad) * r_outer
        y1 = cy + math.sin(rad) * r_outer
        draw.line((x0, y0, x1, y1), fill=col, width=w)

    # Luminous accent arc segments (top and bottom framing)
    arc_box = (36, 36, ss - 36, ss - 36)
    draw.arc(arc_box, start=215, end=325, fill=bri, width=5)
    draw.arc(arc_box, start=35, end=145, fill=pri, width=4)

    # Top status eyebrow pill
    eyebrow = "JARVIS  ·  OLED"
    f_eye = _load_font(24, bold=True)
    eb = draw.textbbox((0, 0), eyebrow, font=f_eye)
    ew, eh = eb[2] - eb[0], eb[3] - eb[1]
    pill_y = int(ss * 0.215)
    pad_x, pad_y = 26, 11
    draw.rounded_rectangle(
        (cx - ew // 2 - pad_x, pill_y - eh // 2 - pad_y,
         cx + ew // 2 + pad_x, pill_y + eh // 2 + pad_y),
        radius=26,
        fill=deep,
        outline=pri,
        width=2,
    )
    draw.text((cx, pill_y), eyebrow, fill=bri, font=f_eye, anchor="mm")

    # Main Title (auto-fit inside round safe chord)
    title_clean = title.strip().upper()
    max_title_w = int(ss * 0.72)
    title_size = 92 if len(title_clean) <= 10 else (76 if len(title_clean) <= 18 else 64)
    f_title = _load_font(title_size, bold=True)
    title_lines = _wrap_text(draw, title_clean, f_title, max_title_w, max_lines=2)
    if len(title_lines) == 1:
        tb = draw.textbbox((0, 0), title_lines[0], font=f_title)
        if (tb[2] - tb[0]) > max_title_w and title_size > 54:
            f_title = _load_font(56, bold=True)
            title_lines = _wrap_text(draw, title_clean, f_title, max_title_w, max_lines=2)

    sub_clean = sub.strip()
    title_center_y = int(ss * (0.455 if sub_clean else 0.505))
    line_gap = int(title_size * 1.14)
    start_y = title_center_y - ((len(title_lines) - 1) * line_gap) // 2
    for idx, line in enumerate(title_lines):
        ly = start_y + idx * line_gap
        # Subtle luminous drop glow behind headline
        draw.text((cx, ly + 3), line, fill=glw, font=f_title, anchor="mm")
        draw.text((cx, ly), line, fill=(246, 252, 255), font=f_title, anchor="mm")

    # Accent hairline divider + Subtitle
    if sub_clean:
        div_y = start_y + (len(title_lines) - 1) * line_gap + int(title_size * 0.72)
        draw.line((cx - 110, div_y, cx + 110, div_y), fill=pri, width=3)
        draw.ellipse((cx - 5, div_y - 5, cx + 5, div_y + 5), fill=bri)
        f_sub = _load_font(38, bold=False)
        sub_lines = _wrap_text(draw, sub_clean, f_sub, int(ss * 0.66), max_lines=2)
        sub_start_y = div_y + 46
        for idx, line in enumerate(sub_lines):
            draw.text((cx, sub_start_y + idx * 46), line, fill=bri, font=f_sub, anchor="mm")

    # Bottom accent indicator pip
    pip_y = int(ss * 0.81)
    draw.ellipse((cx - 10, pip_y - 10, cx + 10, pip_y + 10), fill=glw)
    draw.ellipse((cx - 5, pip_y - 5, cx + 5, pip_y + 5), fill=pri)

    # Hard circular mask for pure OLED corners
    mask = Image.new("L", (ss, ss), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, ss - 1, ss - 1), fill=255)
    black = Image.new("RGB", (ss, ss), (0, 0, 0))
    img = Image.composite(img, black, mask)
    return img.resize((SIZE, SIZE), Image.Resampling.LANCZOS)


def banner_to_rgb565(title: str, sub: str = "", accent: str = "cyan") -> bytes:
    return image_to_rgb565(render_banner_image(title, sub=sub, accent=accent))


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", nargs="?", help="any image file Pillow can read")
    ap.add_argument(
        "--host",
        default=os.environ.get("JARVIS_DEVICE_HOST"),
        help="device host/IP (or $JARVIS_DEVICE_HOST)",
    )
    ap.add_argument(
        "--ttl",
        type=int,
        default=30,
        help="seconds on screen (default 30, max 300)",
    )
    ap.add_argument(
        "--clear",
        action="store_true",
        help="clear the canvas and restore the face",
    )
    ap.add_argument(
        "--takeover",
        action="store_true",
        help="claim Codex operator lease for the canvas TTL before pushing",
    )
    ap.add_argument(
        "--banner",
        metavar="TITLE",
        help="render a procedural luxury OLED banner with TITLE instead of an image file",
    )
    ap.add_argument(
        "--sub",
        default="",
        help="subtitle line for --banner mode",
    )
    ap.add_argument(
        "--accent",
        choices=tuple(ACCENTS.keys()),
        default="cyan",
        help="accent color for --banner mode (default: cyan)",
    )
    return ap


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if not args.host:
        raise SystemExit("--host or JARVIS_DEVICE_HOST required")

    base = jarvisctl.normalize_host(args.host)
    token = load_pairing_token(args.host)

    try:
        if args.clear:
            req = build_request(base, "/api/display/canvas?clear=1", data=b"", token=token)
            with urllib.request.urlopen(req, timeout=30) as resp:
                print(resp.read().decode())
            return 0

        if args.banner:
            body = banner_to_rgb565(args.banner, sub=args.sub, accent=args.accent)
        elif args.image:
            body = to_rgb565(args.image)
        else:
            raise SystemExit("image path, --banner TITLE, or --clear required")

        ttl_s = max(1, min(300, args.ttl))
        if args.takeover:
            lease_ttl = max(10, min(900, ttl_s))
            lease_req = build_request(
                base,
                f"/api/operator/lease?ttl={lease_ttl}&mode=codex",
                data=b"",
                token=token,
            )
            with urllib.request.urlopen(lease_req, timeout=15) as lresp:
                lresp.read()

        req = build_request(
            base,
            f"/api/display/canvas?ttl={ttl_s * 1000}",
            data=body,
            token=token,
            content_type="application/octet-stream",
        )
        with urllib.request.urlopen(req, timeout=30) as resp:
            print(resp.read().decode())
        return 0
    except urllib.error.HTTPError as exc:
        if exc.code == 401:
            print(
                "401: this host is not paired with the device. Hold BOOT "
                f"1.5-5 s, then: python3 scripts/jarvis-desk.py --host {args.host} pair --code NNNNNN",
                file=sys.stderr,
            )
            return 3
        raise


if __name__ == "__main__":
    raise SystemExit(main())
