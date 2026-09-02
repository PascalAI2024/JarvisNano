#!/usr/bin/env python3
"""Bake the four luxury watch dials into one-frame EAF clips.

Direction: docs/GLASS_DESIGN.md, "The watches, second cut — luxury". The dial
is baked art and the hands are live geometry, so each style ships ONE 466×466
frame cut from the concept sheet the owner chose (docs/evidence/
20260902-dial-concepts-*.png — the sheets are 2×2 grids on black, one dial per
quadrant):

  diver   codex-diver   top-left   black sunburst, gold-rimmed lume dots,
                                   gold-framed date window at 3
  dress   codex-dress   top-left   brushed champagne, applied gold batons
  pilot   codex-pilot   top-left   matte black, white numerals, three empty
                                   recessed sub-dial rings at 3 / 6 / 9
  future  codex-future  top-right  cyan tech rings with gold accents, an
                                   arc-reactor centre, four empty data cells

Per dial: crop the quadrant, find the disc (bounding box of non-black pixels),
fit it to 466 px with LANCZOS, mask to the circle on transparent, fill the
centre hole (the photographed dials have a ~10 px hand-stack hole; the live
hub covers r ≤ 12 but a black ring must never peek out under a thin hand) by
copying the dial along each radius, and for DIVER blank the printed digits in
the date window so the firmware can draw the real day. Quantise to 255
colours (MEDIANCUT, NO dithering — measured 2026-09-02: dithering costs runs
and adds nothing at 466 px) using the shared palette convention from
gen_reactive_face.py (index 0 transparent, index 1 opaque black, palette stored
UNswapped BGRA; the engine swaps at decode), and emit with the byte-verified
container from gen_mascot_eaf.py, choosing RAW or RLE per 32-row block —
the decoder reads the encoding byte per block. A sunburst's radial grain
makes RLE larger than raw (263 KB vs 217 KB whole-frame on the diver), so
its bands stay raw while the black corners run.

Also measures, in 466-px coordinates, the geometry the firmware needs to place
live elements over the art: the DIVER date box, the PILOT sub-dial centres and
radii, the FUTURE data cells. `measure` prints them; the numbers are copied
into the design section "as shipped".

Usage:
  python3 gen_watch_dials.py all              # bake all four
  python3 gen_watch_dials.py one diver        # bake one
  python3 gen_watch_dials.py measure          # print the geometry only
  python3 gen_watch_dials.py all --sheets DIR # read the sheets from elsewhere

Requires Pillow + numpy. No network. Writes firmware/mascot/reactive/
dial_<style>.eaf and dial_<style>_preview.png. Does not flash.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from gen_mascot_eaf import assert_roundtrip                    # noqa: E402
from gen_reactive_face import (build_palette, quantize_frame,   # noqa: E402
                               _rle_encode, EAF_MAGIC_HEAD, ENC_RAW, ENC_RLE)
import struct                                                    # noqa: E402

REPO = HERE.parent.parent
SHEETS = REPO / "docs" / "evidence"
OUT = HERE / "reactive"
CANVAS = 466
R = CANVAS // 2                     # 233
CX = CY = R                         # centre pixel is (233, 233) like the HUD

# THE PARTITION IS THE BUDGET. spiffsgen (page 256, block 4096, the sdkconfig
# values) fits about 5.2 MB of files into the 6,016 KB partition — SPIFFS
# keeps two pages of every sixteen for its index and two blocks free — and
# the eight face clips already take 4,728 KB. Measured 2026-09-02 with
# spiffsgen: four raw dials (5,592 KB of files) overflow; the four as encoded
# below (5,388 KB) fit with 96–128 KB to spare. So each dial takes the
# encoding that fits it:
#   raw  = 217 KB regardless of content (a sunburst's grain defeats RLE);
#   rle  = runs of palette index 1 (black) after a BLACK FLOOR — every pixel
#          whose brightest channel is under `floor` becomes pure black, which
#          the AMOLED shows as the same black anyway. Pilot's matte dial and
#          Future's near-black field collapse to 82 KB and 141 KB at floor 28;
#          the diver's sunburst does not (213 KB at floor 20, 161 at 28 with
#          its dark wedges flattened), and the dress's brushed champagne has
#          no black at all (284 KB under RLE, worse than raw).
# The decoder reads the encoding byte PER 32-ROW BLOCK (gfx_eaf_dec.c:372,
# `encoding_type = block_data[0]`), so every block takes whichever of RAW or
# RLE is smaller for it: the black corners outside the disc and a dark field
# run, a sunburst band stays raw. The black floor is per style.
# style -> black floor
BLACK_FLOOR = {
    "diver":  0,
    "dress":  0,
    "pilot":  28,
    "future": 28,
}
BLOCK_H = 32


def emit_eaf_mixed(rows, palette_bgra, path: Path):
    """One frame, the byte-verified container of gen_mascot_eaf.emit_eaf, and
    per block the smaller of RAW and RLE. Returns (bytes, raw_blocks, rle_blocks)."""
    h, w = len(rows), len(rows[0])
    blocks = (h + BLOCK_H - 1) // BLOCK_H
    datas, n_raw, n_rle = [], 0, 0
    for b in range(blocks):
        strip = b"".join(bytes(rows[y]) for y in range(b * BLOCK_H, min(h, (b + 1) * BLOCK_H)))
        rle = _rle_encode(strip)
        if len(rle) < len(strip):
            datas.append(bytes([ENC_RLE]) + rle)
            n_rle += 1
        else:
            datas.append(bytes([ENC_RAW]) + strip)
            n_raw += 1
    hdr = b"_S" + b"\x00" + b"v1.0\x00\x00" + bytes([8])
    hdr += struct.pack("<HHHH", w, h, blocks, BLOCK_H)
    hdr += b"".join(struct.pack("<I", len(d)) for d in datas)
    pal = b"".join(bytes(palette_bgra[i]) if i < len(palette_bgra) else b"\0\0\0\0"
                   for i in range(256))
    frame = struct.pack("<H", EAF_MAGIC_HEAD) + hdr + pal + b"".join(datas)
    blobs = [frame, struct.pack("<H", EAF_MAGIC_HEAD) + b"_C"]
    table, data = bytearray(), bytearray()
    for blob in blobs:
        table += struct.pack("<II", len(blob), len(data))
        data += blob
    tpd = bytes(table) + bytes(data)
    out = (bytes([0x89]) + b"EAF" + struct.pack("<i", len(blobs))
           + struct.pack("<I", sum(tpd) & 0xFFFFFFFF) + struct.pack("<I", len(tpd)) + tpd)
    Path(path).write_bytes(out)
    return len(out), n_raw, n_rle

# style -> (sheet stem, quadrant)   quadrants: 0 TL, 1 TR, 2 BL, 3 BR
DIALS = {
    "diver":  ("20260902-dial-concepts-codex-diver.png",  0),
    "dress":  ("20260902-dial-concepts-codex-dress.png",  0),
    "pilot":  ("20260902-dial-concepts-codex-pilot.png",  0),
    "future": ("20260902-dial-concepts-codex-future.png", 1),
}


# ---------------------------------------------------------------------------
# cutting the disc out of the sheet
# ---------------------------------------------------------------------------
def quadrant(sheet: Image.Image, q: int) -> Image.Image:
    w, h = sheet.size
    x0 = (q % 2) * (w // 2)
    y0 = (q // 2) * (h // 2)
    return sheet.crop((x0, y0, x0 + w // 2, y0 + h // 2))


def disc_bbox(img: Image.Image, thresh: int = 28):
    """Bounding box of the dial: pixels whose brightest channel clears the
    black background. The photographed dials sit on pure black."""
    a = np.asarray(img.convert("RGB"))
    lit = a.max(axis=2) > thresh
    ys, xs = np.where(lit)
    cx = (xs.min() + xs.max()) / 2.0
    cy = (ys.min() + ys.max()) / 2.0
    r = max(xs.max() - xs.min(), ys.max() - ys.min()) / 2.0
    return cx, cy, r


def fit_disc(img: Image.Image) -> Image.Image:
    """Crop the disc square and fit it to CANVAS px; the disc edge lands on the
    glass edge (r = 233)."""
    cx, cy, r = disc_bbox(img)
    r *= 1.004                      # keep the antialiased rim inside the crop
    box = (cx - r, cy - r, cx + r, cy + r)
    return img.convert("RGB").resize(
        (CANVAS, CANVAS), Image.LANCZOS, box=box)


def circle_alpha() -> np.ndarray:
    m = Image.new("L", (CANVAS, CANVAS), 0)
    ImageDraw.Draw(m).ellipse((0, 0, CANVAS - 1, CANVAS - 1), fill=255)
    return np.asarray(m)


def polar_grid():
    yy, xx = np.mgrid[0:CANVAS, 0:CANVAS]
    dx = xx - CX
    dy = yy - CY
    return np.hypot(dx, dy), np.arctan2(dy, dx)


def fill_centre_hole(rgb: np.ndarray) -> tuple[np.ndarray, int]:
    """The hand-stack hole: a dark disc at the exact centre. Find its radius
    (first ring whose median brightness rises to the dial's) and copy the dial
    inward along every radius from just outside it — a sunburst is radial, so
    the grain continues instead of smearing."""
    rr, th = polar_grid()
    bright = rgb.max(axis=2)
    ring_lvl = []
    for r in range(0, 40):
        sel = (rr >= r) & (rr < r + 1)
        ring_lvl.append(np.median(bright[sel]))
    dial_lvl = np.median(ring_lvl[30:40])
    hole = 0
    for r in range(0, 30):
        if ring_lvl[r] >= dial_lvl * 0.7:
            hole = r
            break
    src_r = hole + 4
    out = rgb.copy()
    fill = rr < hole + 2
    ys, xs = np.where(fill)
    sx = np.clip(np.rint(CX + src_r * np.cos(th[fill])).astype(int), 0, CANVAS - 1)
    sy = np.clip(np.rint(CY + src_r * np.sin(th[fill])).astype(int), 0, CANVAS - 1)
    out[ys, xs] = rgb[sy, sx]
    return out, hole


def frame_box(mask: np.ndarray, region, frac: float = 0.5):
    """The rectangle a mask draws inside a search region (x0,y0,x1,y1): the
    rows and columns that carry at least `frac` of the busiest row/column.
    A bounding box would swallow every stray tick and hairline nearby; a
    histogram sees the long straight edges of a box and nothing else."""
    x0, y0, x1, y1 = region
    sub = mask[y0:y1, x0:x1]
    if not sub.any():
        return None
    cols = sub.sum(axis=0)
    rows = sub.sum(axis=1)
    cx = np.where(cols >= cols.max() * frac)[0]
    ry = np.where(rows >= rows.max() * frac)[0]
    return (int(x0 + cx.min()), int(y0 + ry.min()),
            int(x0 + cx.max()), int(y0 + ry.max()))


def frame_edges(mask: np.ndarray, region, gap: int = 10):
    """The rectangle a HOLLOW frame draws inside a search region: the two
    strongest rows and the two strongest columns of the mask, at least `gap`
    px apart, so a dimmer far edge (a glow fades) and a stray hairline next
    to the box are both read correctly. `frame_box` is for filled boxes."""
    x0, y0, x1, y1 = region
    sub = mask[y0:y1, x0:x1]
    if not sub.any():
        return None

    def two_peaks(counts):
        order = np.argsort(counts)[::-1]
        first = int(order[0])
        second = next((int(i) for i in order[1:] if abs(int(i) - first) >= gap), first)
        return min(first, second), max(first, second)

    cx0, cx1 = two_peaks(sub.sum(axis=0))
    ry0, ry1 = two_peaks(sub.sum(axis=1))
    return (int(x0 + cx0), int(y0 + ry0), int(x0 + cx1), int(y0 + ry1))


def blank_date_window(rgb: np.ndarray):
    """DIVER: the sheet printed a day in the window; the firmware draws the
    real one. Keep the frame and the paper, drop the ink."""
    region = (CX + 110, CY - 40, CX + 225, CY + 40)
    white = rgb.min(axis=2) > 200
    box = frame_box(white, region, frac=0.5)
    if box is None:
        raise SystemExit("diver: no date window found")
    x0, y0, x1, y1 = box
    inner = rgb[y0:y1 + 1, x0:x1 + 1]
    paper = np.median(inner[inner.min(axis=2) > 200], axis=0).astype(np.uint8)
    out = rgb.copy()
    out[y0:y1 + 1, x0:x1 + 1] = paper
    return out, (int(x0), int(y0), int(x1), int(y1))


# ---------------------------------------------------------------------------
# measuring the geometry the firmware places live elements into
# ---------------------------------------------------------------------------
def measure_pilot(rgb: np.ndarray):
    """Sub-dial rings at 3 / 6 / 9. The recessed rings are nearly the dial's
    own grey, so brightness thresholds see nothing; the rim is an EDGE. Score
    candidate circles (centre on the axis, radius 28..60) by the mean gradient
    magnitude sampled around the circle and keep the best per position."""
    g = rgb.astype(float).mean(axis=2)
    gy, gx = np.gradient(g)
    mag = np.hypot(gx, gy)
    ang = np.linspace(0, 2 * np.pi, 180, endpoint=False)
    ca, sa = np.cos(ang), np.sin(ang)

    def best(centres):
        top = (0.0, None)
        for (cx, cy) in centres:
            for r in range(28, 61):
                xs = np.clip(np.rint(cx + r * ca).astype(int), 0, CANVAS - 1)
                ys = np.clip(np.rint(cy + r * sa).astype(int), 0, CANVAS - 1)
                score = float(mag[ys, xs].mean())
                if score > top[0]:
                    top = (score, (int(cx), int(cy), int(r)))
        return top[1]

    return {
        "9": best([(x, CY) for x in range(CX - 150, CX - 60)]),
        "3": best([(x, CY) for x in range(CX + 60, CX + 150)]),
        "6": best([(CX, y) for y in range(CY + 60, CY + 150)]),
    }


def measure_future(rgb: np.ndarray):
    """The four data cells: cyan hairline frames inside r < 150. Each cell is
    the bounding box of the frame pixels in its own search window."""
    a = rgb.astype(int)
    cyan = (a[:, :, 1] > 190) & (a[:, :, 2] > 190) & (a[:, :, 0] < 160)
    windows = {
        "top":    (CX - 130, CY - 155, CX + 130, CY - 85),
        "bottom": (CX - 130, CY + 85, CX + 130, CY + 155),
        "left":   (CX - 210, CY - 40, CX - 95, CY + 40),
        "right":  (CX + 95, CY - 40, CX + 210, CY + 40),
    }
    out = {}
    for name, region in windows.items():
        box = frame_edges(cyan, region)
        if box is not None:
            out[name] = box
    return out


# ---------------------------------------------------------------------------
# quantise + emit
# ---------------------------------------------------------------------------
def to_rgba(rgb: np.ndarray) -> np.ndarray:
    alpha = circle_alpha()
    rgba = np.dstack([rgb, alpha]).astype(np.uint8)
    rgba[alpha == 0, :3] = 0
    return rgba


def preview(rows, palette_bgra, path: Path):
    lut = np.array([(r, g, b, a) for b, g, r, a in palette_bgra], dtype=np.uint8)
    idx = np.array([list(r) for r in rows], dtype=np.uint8)
    Image.fromarray(lut[idx], "RGBA").save(path)


def bake(style: str, sheets: Path, write: bool = True) -> dict:
    stem, q = DIALS[style]
    sheet = Image.open(sheets / stem)
    disc = fit_disc(quadrant(sheet, q))
    rgb = np.asarray(disc).copy()
    rgb, hole = fill_centre_hole(rgb)
    geom = {"hole_r": hole}
    if style == "diver":
        rgb, geom["date_box"] = blank_date_window(rgb)
    if style == "pilot":
        geom["subdials"] = measure_pilot(rgb)
    if style == "future":
        geom["cells"] = measure_future(rgb)
    if write:
        floor = BLACK_FLOOR[style]
        if floor:
            rgb = rgb.copy()
            rgb[rgb.max(axis=2) < floor] = 0
        rgba = to_rgba(rgb)
        palette_bgra, pal_arr = build_palette([rgba])
        rows = quantize_frame(rgba, pal_arr)
        OUT.mkdir(parents=True, exist_ok=True)
        eaf = OUT / f"dial_{style}.eaf"
        size, n_raw, n_rle = emit_eaf_mixed(rows, palette_bgra, eaf)
        assert assert_roundtrip(eaf) == 2, "one frame plus the _C sentinel"
        preview(rows, palette_bgra, OUT / f"dial_{style}_preview.png")
        geom["bytes"] = size
        print(f"{style:7s} {eaf.name} {size:,} B  blocks raw {n_raw} / rle {n_rle}  floor {floor}  hole r={hole}")
    return geom


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["all", "one", "measure"])
    ap.add_argument("style", nargs="?")
    ap.add_argument("--sheets", type=Path, default=SHEETS)
    args = ap.parse_args()
    styles = [args.style] if args.cmd == "one" else list(DIALS)
    for s in styles:
        g = bake(s, args.sheets, write=(args.cmd != "measure"))
        for k, v in g.items():
            if k != "bytes":
                print(f"  {s}.{k} = {v}")


if __name__ == "__main__":
    main()
