#!/usr/bin/env python3
"""Bake the SKELETON balance wheel into a C sprite.

The skeleton dial (dial_skeleton.eaf, gen_watch_dials.py) has an empty
circular recess in its lower middle. The wheel that swings in it is live: a
rotated, alpha-blended sprite drawn by components/jr_display/src/
balance_wheel.c under the hands. This script cuts that sprite from the
Codex render docs/evidence/20261110-dial-skeleton-codex-balance-wheel.png:

  1. measure the recess on the no-wheel dial (the ruby jewel at its centre
     gives the centre; the strongest radial edge around it gives the radius),
     in the same 466-px frame the dial is baked in;
  2. clean the wheel's alpha: the render has stray specks and a wide fringe
     of half-transparent debris, so threshold, keep the largest connected
     component, and rescale the survivors' alpha into a soft one-pixel edge;
  3. fit the wheel's disc to 2 * WHEEL_R + 1 px (it must fill the recess),
     pad two transparent pixels on every side so the rotating sampler never
     needs a bounds check, and bleed the colour of the opaque pixels into
     the transparent ones so bilinear sampling has no dark fringe;
  4. write RGB565 (little endian, host order) plus an 8-bit alpha plane as
     two const arrays.

Usage:
  python3 gen_balance_wheel.py            # measure + write the two files
  python3 gen_balance_wheel.py --check    # exit 1 if the files are stale
  python3 gen_balance_wheel.py --preview OUT.png   # the sprite on the dial

Requires Pillow, numpy, scipy. No network.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage as ndi

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import gen_watch_dials as gwd  # noqa: E402

REPO = HERE.parent.parent
EVID = REPO / "docs" / "evidence"
SRC = REPO / "components" / "jr_display" / "src"
NOWHEEL = EVID / "20261110-dial-skeleton-codex-nowheel.png"
WHEEL = EVID / "20261110-dial-skeleton-codex-balance-wheel.png"

WHEEL_R = 70                         # outer radius of the wheel on the dial, px
PAD = 2
SIZE = 2 * (WHEEL_R + PAD) + 1       # 145
CENTRE = WHEEL_R + PAD               # 72
ALPHA_LO, ALPHA_HI = 96, 224         # alpha ramp after cleaning
KEEP_T = 160                         # alpha that counts as "wheel" for the component pick


def measure_recess() -> tuple[float, float, float]:
    """(cx, cy, r) of the recess in the 466-px dial frame, sub-pixel."""
    im = Image.open(NOWHEEL).convert("RGB")
    a = np.asarray(im).astype(float)
    cx0, cy0, r0 = gwd.disc_bbox(im)
    r0 *= 1.004
    scale = gwd.CANVAS / (2.0 * r0)
    # the ruby: saturated magenta, in the lower middle of the disc
    x0, x1 = int(cx0 - 120), int(cx0 + 120)
    y0, y1 = int(cy0 + 150), int(cy0 + 380)
    sub = a[y0:y1, x0:x1]
    ruby = (sub[:, :, 0] > 120) & (sub[:, :, 2] > 90) & (sub[:, :, 1] < 80)
    ys, xs = np.where(ruby)
    if len(xs) < 20:
        raise SystemExit("balance wheel: no ruby found in the recess")
    jx, jy = xs.mean() + x0, ys.mean() + y0
    # radius: the strongest mean radial gradient on a ring around the jewel
    g = a.mean(axis=2)
    gy, gx = np.gradient(g)
    ang = np.linspace(0, 2 * np.pi, 720, endpoint=False)
    best = (0.0, 0)
    for rr in range(140, 200):
        xi = np.clip(np.rint(jx + rr * np.cos(ang)).astype(int), 0, g.shape[1] - 1)
        yi = np.clip(np.rint(jy + rr * np.sin(ang)).astype(int), 0, g.shape[0] - 1)
        rad = np.abs(gx[yi, xi] * np.cos(ang) + gy[yi, xi] * np.sin(ang)).mean()
        if rad > best[0]:
            best = (rad, rr)
    left, top = cx0 - r0, cy0 - r0
    # +0.5: a pixel index names the pixel's centre
    return ((jx + 0.5 - left) * scale - 0.5, (jy + 0.5 - top) * scale - 0.5,
            best[1] * scale)


def clean_wheel() -> np.ndarray:
    """RGBA uint8, SIZE x SIZE, the wheel's disc fitted to 2*WHEEL_R+1 px."""
    a = np.asarray(Image.open(WHEEL).convert("RGBA")).copy()
    al = a[:, :, 3].astype(np.int32)
    lab, n = ndi.label(al >= KEEP_T)
    sizes = ndi.sum(np.ones_like(al), lab, range(1, n + 1))
    keep = lab == (int(np.argmax(sizes)) + 1)
    near = ndi.binary_dilation(keep, iterations=3)
    soft = np.clip((al - ALPHA_LO) * 255 // (ALPHA_HI - ALPHA_LO), 0, 255)
    soft = np.where(near, soft, 0)
    # a stray speck that touches the wheel may survive the pick; drop what the
    # soft alpha leaves disconnected from the wheel body
    lab2, n2 = ndi.label(soft > 0)
    sz2 = ndi.sum(np.ones_like(soft), lab2, range(1, n2 + 1))
    soft = np.where(lab2 == (int(np.argmax(sz2)) + 1), soft, 0)
    a[:, :, 3] = soft.astype(np.uint8)
    ys, xs = np.where(soft > 40)
    x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    # premultiply for the resize so transparent colour cannot leak in
    f = a.astype(np.float64)
    alpha = f[:, :, 3:4] / 255.0
    pm = np.dstack([f[:, :, :3] * alpha, f[:, :, 3:4]])
    side = 2 * WHEEL_R + 1
    layers = []
    for c in range(4):
        layer = Image.fromarray(pm[:, :, c].astype(np.float32), "F")
        layers.append(np.asarray(layer.resize((side, side), Image.LANCZOS,
                                              box=(x0, y0, x1, y1))))
    pm2 = np.dstack(layers)
    al2 = np.clip(pm2[:, :, 3], 0, 255)
    rgb = np.where(al2[:, :, None] > 1.0,
                   pm2[:, :, :3] * 255.0 / np.maximum(al2[:, :, None], 1.0), 0.0)
    rgb = np.clip(rgb, 0, 255)
    # circular window: nothing outside the wheel's radius (kills corner specks)
    yy, xx = np.mgrid[0:side, 0:side]
    inside = (xx - WHEEL_R) ** 2 + (yy - WHEEL_R) ** 2 <= (WHEEL_R + 0.5) ** 2
    al2 = np.where(inside, al2, 0.0)
    # bleed colour of opaque pixels into the transparent ones
    solid = al2 > 128
    idx = ndi.distance_transform_edt(~solid, return_distances=False, return_indices=True)
    bled = rgb[idx[0], idx[1]]
    rgb = np.where(solid[:, :, None], rgb, bled)
    out = np.zeros((SIZE, SIZE, 4), np.uint8)
    out[PAD:PAD + side, PAD:PAD + side, :3] = np.rint(rgb).astype(np.uint8)
    out[PAD:PAD + side, PAD:PAD + side, 3] = np.rint(al2).astype(np.uint8)
    # padding colour: the nearest edge colour, alpha 0
    solid_full = out[:, :, 3] > 128
    idx2 = ndi.distance_transform_edt(~solid_full, return_distances=False, return_indices=True)
    pad_rgb = out[idx2[0], idx2[1], :3]
    out[:, :, :3] = np.where(out[:, :, 3:4] > 0, out[:, :, :3], pad_rgb)
    return out


def rgb565(rgb: np.ndarray) -> np.ndarray:
    r, g, b = (rgb[:, :, i].astype(np.uint16) for i in range(3))
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def render_c(sprite: np.ndarray, cx: float, cy: float, rr: float) -> tuple[str, str]:
    px = rgb565(sprite[:, :, :3]).reshape(-1)
    al = sprite[:, :, 3].reshape(-1)

    def rows(vals, per, fmt):
        lines = []
        for i in range(0, len(vals), per):
            lines.append("    " + ", ".join(fmt % v for v in vals[i:i + per]) + ",")
        return "\n".join(lines)

    wx, wy = int(round(cx)), int(round(cy))
    head = f"""/* GENERATED by firmware/mascot/gen_balance_wheel.py. Do not edit.
 * `python3 firmware/mascot/gen_balance_wheel.py --check` fails if stale.
 * The SKELETON balance wheel: {SIZE} x {SIZE} px, the wheel's disc is
 * {2 * WHEEL_R + 1} px across inside {PAD} transparent pixels of padding.
 * Art: docs/evidence/20261110-dial-skeleton-codex-balance-wheel.png
 * (in-house Codex image generation). */
#ifndef JR_DISPLAY_BALANCE_WHEEL_DATA_H
#define JR_DISPLAY_BALANCE_WHEEL_DATA_H

#include <stdint.h>

#define JR_BALANCE_SIZE    {SIZE}      /* sprite is SIZE x SIZE */
#define JR_BALANCE_C       {CENTRE}       /* its centre pixel, both axes */
#define JR_BALANCE_R       {WHEEL_R}       /* radius of the wheel itself */
/* where the recess is on the 466-px dial (measured {cx:.2f}, {cy:.2f}, r {rr:.1f}) */
#define JR_BALANCE_DIAL_CX {wx}
#define JR_BALANCE_DIAL_CY {wy}

extern const uint16_t jr_balance_rgb[JR_BALANCE_SIZE * JR_BALANCE_SIZE];
extern const uint8_t  jr_balance_alpha[JR_BALANCE_SIZE * JR_BALANCE_SIZE];

#endif
"""
    body = f"""/* GENERATED by firmware/mascot/gen_balance_wheel.py. Do not edit. */
#include "balance_wheel_data.h"

const uint16_t jr_balance_rgb[JR_BALANCE_SIZE * JR_BALANCE_SIZE] = {{
{rows(px.tolist(), 12, "0x%04X")}
}};

const uint8_t jr_balance_alpha[JR_BALANCE_SIZE * JR_BALANCE_SIZE] = {{
{rows(al.tolist(), 24, "%d")}
}};
"""
    return head, body


def build() -> tuple[str, str, np.ndarray, tuple[float, float, float]]:
    cx, cy, rr = measure_recess()
    sprite = clean_wheel()
    head, body = render_c(sprite, cx, cy, rr)
    return head, body, sprite, (cx, cy, rr)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--preview", type=Path)
    args = ap.parse_args()
    head, body, sprite, (cx, cy, rr) = build()
    print(f"recess centre ({cx:.2f}, {cy:.2f}) r {rr:.1f}; sprite {SIZE}x{SIZE}, "
          f"opaque px {(sprite[:, :, 3] > 128).sum()}, "
          f"touched px {(sprite[:, :, 3] > 0).sum()}")
    paths = {SRC / "balance_wheel_data.h": head, SRC / "balance_wheel_data.c": body}
    if args.preview:
        dial = Image.open(HERE / "reactive" / "dial_skeleton_preview.png").convert("RGBA")
        spr = Image.fromarray(sprite, "RGBA")
        dial.alpha_composite(spr, (int(round(cx)) - CENTRE, int(round(cy)) - CENTRE))
        dial.save(args.preview)
    if args.check:
        stale = [p.name for p, text in paths.items()
                 if not p.exists() or p.read_bytes().replace(b"\r\n", b"\n") != text.encode()]
        if stale:
            print("STALE:", ", ".join(stale))
            return 1
        print("balance wheel sources are current")
        return 0
    for p, text in paths.items():
        p.write_bytes(text.encode())
    return 0


if __name__ == "__main__":
    sys.exit(main())
