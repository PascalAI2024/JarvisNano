#!/usr/bin/env python3
"""Bake the reactive "Pulsing Orb" face into per-state EAF animations.

This produces FOUR looping EAF files — one per face state — for the
`esp_emote_gfx` `gfx_anim` widget. NO runtime pixel drawing happens on the device;
all motion is in these pre-rendered frames (the engine has no canvas widget — a
CPU-drawn RGB565A8 buffer spirals on the CO5300 panel, proven empirically). The
runtime code (firmware/emote/reactive_face.c) only selects which baked frames to
loop, using the live audio amplitude. See docs/REACTIVE_FACE_PLAN.md.

States (names are a HARD CONTRACT with reactive_face.c's RWAVE_ASSET_* strings):
  rwave_idle    calm breathing orb — single soft glow pulsing at centre (full-loop)
  rwave_listen  concentric orb stacks, cyan palette; frame 0 = centre orb,
                frame K-1 = 4 orbs fully lit (mic amplitude → reach)
  rwave_think   orbiting particle with fading trail circling a dim anchor (full-loop)
  rwave_speak   same orb-stack geometry as listen, hot amber palette (output RMS)

The listen/speak files are baked as a MONOTONIC amplitude ramp so the runtime can
play [0..end] where `end` tracks loudness — louder voice = more orbs visible.
The idle/think files are baked as a self-contained motion loop.

Encoder: emit_eaf()/assert_roundtrip() are imported VERBATIM from gen_mascot_eaf.py
(byte-verified against gfx_eaf_dec.c). Palette is stored UNswapped BGRA; the engine
applies the QSPI byte-swap at decode (eaf_palette_get_color, swap=true) — that is
why baked EAF renders correct colour where the old runtime buffer rendered white.

Usage:
  python3 gen_reactive_face.py all                  # bake all four states
  python3 gen_reactive_face.py one rwave_listen     # bake a single state
  python3 gen_reactive_face.py preview              # write per-frame PNG contact sheets
  python3 gen_reactive_face.py all --canvas 466 --frames 24 --enc raw

Requires: Pillow + numpy + stdlib only. NO network, NO API keys. Does NOT flash or
build — it only writes .eaf + preview PNGs under firmware/mascot/reactive/. The
orchestrator owns mounting the EAFs into the `emote` asset partition.
"""
import argparse
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw
import numpy as np

import struct

# Reuse the byte-verified EAF encoder + round-trip checker. They live in
# gen_mascot_eaf.py next to this file; import without triggering its CLI.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_mascot_eaf import emit_eaf as emit_eaf_raw, assert_roundtrip  # noqa: E402

# ----------------------------------------------------------------------------
# RLE-capable EAF emitter (extends the RAW emit_eaf from gen_mascot_eaf.py).
# The outer container is identical to gen_mascot_eaf.emit_eaf (byte-verified vs
# gfx_eaf_dec.c); only the per-block body changes: RAW = ENC byte + raw indices,
# RLE = ENC byte + (count:u8, value:u8) pairs, exactly what eaf_decode_rle reads
# (gfx_eaf_dec.c:409 — pairs until in_pos+1<input_size; decodes to width*block_h).
# The mostly-black (palette index 0) reactive frames compress hugely under RLE.
# ----------------------------------------------------------------------------
EAF_MAGIC_HEAD = 0x5A5A
ENC_RLE = 0
ENC_RAW = 5


def _rle_encode(strip: bytes) -> bytes:
    """Encode a byte strip as (count,value) pairs, count in 1..255. The decoder
    (gfx_eaf_dec.c:409) reads pairs and expands; runs longer than 255 split."""
    out = bytearray()
    i, n = 0, len(strip)
    while i < n:
        v = strip[i]
        run = 1
        while i + run < n and strip[i + run] == v and run < 255:
            run += 1
        out.append(run)
        out.append(v)
        i += run
    return bytes(out)


def _emit_frame_S(rows, palette_bgra, enc, block_height=32):
    h = len(rows)
    w = len(rows[0])
    blocks = (h + block_height - 1) // block_height
    block_datas = []
    for b in range(blocks):
        y0, y1 = b * block_height, min(b * block_height + block_height, h)
        strip = b"".join(bytes(rows[y]) for y in range(y0, y1))
        if enc == ENC_RLE:
            block_datas.append(bytes([ENC_RLE]) + _rle_encode(strip))
        else:
            block_datas.append(bytes([ENC_RAW]) + strip)
    block_len = [len(bd) for bd in block_datas]
    hdr = b"_S" + b"\x00" + b"v1.0\x00\x00" + bytes([8])
    hdr += struct.pack("<HHHH", w, h, blocks, block_height)
    hdr += b"".join(struct.pack("<I", bl) for bl in block_len)
    pal = b"".join(bytes(palette_bgra[i]) if i < len(palette_bgra) else b"\0\0\0\0"
                   for i in range(256))
    return hdr + pal + b"".join(block_datas)


def emit_eaf_enc(frames_rows, palette_bgra, path, enc, block_height=32):
    """Same container as gen_mascot_eaf.emit_eaf, with selectable block encoding."""
    if enc != ENC_RLE:
        return emit_eaf_raw(frames_rows, palette_bgra, path, block_height)
    blobs = [struct.pack("<H", EAF_MAGIC_HEAD) + _emit_frame_S(fr, palette_bgra, enc, block_height)
             for fr in frames_rows]
    blobs.append(struct.pack("<H", EAF_MAGIC_HEAD) + b"_C")
    table = bytearray()
    data = bytearray()
    for blob in blobs:
        table += struct.pack("<II", len(blob), len(data))
        data += blob
    tpd = bytes(table) + bytes(data)
    checksum = sum(tpd) & 0xFFFFFFFF
    out = (bytes([0x89]) + b"EAF" + struct.pack("<i", len(blobs))
           + struct.pack("<I", checksum) + struct.pack("<I", len(tpd)) + tpd)
    Path(path).write_bytes(out)
    return path

# ----------------------------------------------------------------------------
# Paths & brand palette
# ----------------------------------------------------------------------------
OUT_DIR = Path(__file__).resolve().parent / "reactive"
EAF_DIR = OUT_DIR                          # the .eaf deliverables
PREVIEW_DIR = OUT_DIR / "preview"          # optional per-frame PNGs

# Brand-exact accents on pure black.
HOT_CORE = (0xFF, 0xE2, 0x5E)              # #FFE25E hot yellow core
ACCENT = (0xF5, 0x87, 0x0B)               # #F5870B amber
DIM = (0x6A, 0x3A, 0x05)                   # dim amber (idle rest / think track)
WARM = (0xFF, 0xB8, 0x2C)                  # warm intermediate
CYAN_GLOW = (0x4A, 0xF0, 0xE0)             # cyan accent for listening

DEFAULT_CANVAS = 466                       # FULL panel — use every pixel of the AMOLED
DEFAULT_FRAMES = 24                        # smoother motion for the larger canvas

# State -> (renderer key). Names are the HARD CONTRACT with reactive_face.c.
STATE_ORDER = ["rwave_idle", "rwave_listen", "rwave_think", "rwave_speak"]


# ----------------------------------------------------------------------------
# Drawing helpers (host-side, into an RGBA frame). These are NOT shipped to the
# device — they only produce the baked PNG that becomes an EAF frame.
# ----------------------------------------------------------------------------
def _blank(canvas):
    return np.zeros((canvas, canvas, 4), dtype=np.uint8)


def _lerp_col(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def _soft_orb(arr, cx, cy, r, rgb, alpha, feather=25.0):
    """Radial-gradient soft orb — bright at centre, Gaussian fade outward.
    The round AMOLED panel makes this the natural primitive: no thin arcs or
    hard edges, just smooth glowing orbs that look at home on a circular display."""
    if r < 1.0:
        r = 1.0
    yy, xx = np.mgrid[0:arr.shape[0], 0:arr.shape[1]]
    dist = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2)
    # Gaussian: peak at centre, ~0.6 at r, ~0 at r+feather
    sigma = (r + feather) / 2.5
    t = np.exp(-(dist ** 2) / (2.0 * sigma * sigma))
    t = np.clip(t, 0, 1)
    mask = t > 0.01
    for y, x in zip(*np.where(mask)):
        a = alpha * t[y, x]
        if a > arr[y, x, 3]:
            arr[y, x, 0] = rgb[0]
            arr[y, x, 1] = rgb[1]
            arr[y, x, 2] = rgb[2]
            arr[y, x, 3] = min(255, int(a))


def _polar_points(cx, cy, r, a0, a1, steps):
    return [
        (cx + r * math.cos(a0 + (a1 - a0) * t / max(1, steps - 1)),
         cy + r * math.sin(a0 + (a1 - a0) * t / max(1, steps - 1)))
        for t in range(steps)
    ]


def _stroke_poly(arr, pts, rgb, alpha, width=3):
    img = Image.fromarray(arr, "RGBA")
    draw = ImageDraw.Draw(img, "RGBA")
    draw.line(pts, fill=(rgb[0], rgb[1], rgb[2], alpha), width=width, joint="curve")
    arr[:] = np.array(img)


def _arc(arr, cx, cy, r, a0, a1, rgb, alpha, width=3, steps=96):
    _stroke_poly(arr, _polar_points(cx, cy, r, a0, a1, steps), rgb, alpha, width)


def _visor(arr, cx, cy, openness, rgb, alpha):
    """Draw a warm, non-white visor/sprite core. Looks like a face without using
    the default emote eyes."""
    img = Image.fromarray(arr, "RGBA")
    draw = ImageDraw.Draw(img, "RGBA")
    w = int(154 + 18 * openness)
    h = int(24 + 42 * openness)
    y = int(cy - 36)
    x0 = int(cx - w / 2)
    x1 = int(cx + w / 2)
    y0 = int(y - h / 2)
    y1 = int(y + h / 2)
    draw.rounded_rectangle((x0, y0, x1, y1), radius=max(10, h // 2),
                           outline=(rgb[0], rgb[1], rgb[2], alpha), width=4)
    draw.line((x0 + 22, y, x1 - 22, y), fill=(HOT_CORE[0], HOT_CORE[1], HOT_CORE[2], min(255, alpha + 45)), width=3)
    # Tiny asymmetric glints, amber not white.
    draw.ellipse((x0 + 30, y - 5, x0 + 42, y + 5), fill=(HOT_CORE[0], HOT_CORE[1], HOT_CORE[2], 210))
    draw.ellipse((x1 - 42, y - 5, x1 - 30, y + 5), fill=(HOT_CORE[0], HOT_CORE[1], HOT_CORE[2], 170))
    arr[:] = np.array(img)


def _voice_bars(arr, cx, cy, reach, rgb, alpha, phase, bars=17):
    img = Image.fromarray(arr, "RGBA")
    draw = ImageDraw.Draw(img, "RGBA")
    span = 250
    step = span / (bars - 1)
    for k in range(bars):
        n = k / (bars - 1)
        env = math.sin(n * math.pi)
        shimmer = 0.75 + 0.25 * math.sin(phase + k * 0.73)
        h = 10 + reach * env * shimmer * 130
        x = cx - span / 2 + k * step
        draw.rounded_rectangle((x - 4, cy - h / 2, x + 4, cy + h / 2),
                               radius=4,
                               fill=(rgb[0], rgb[1], rgb[2], int(alpha * (0.45 + 0.55 * env))))
    arr[:] = np.array(img)


def _hud_shell(arr, cx, cy, phase, mood=0.0):
    """Round AMOLED-friendly HUD: segmented arcs, particles, and dark core."""
    # Glow bed
    _soft_orb(arr, cx, cy, 32 + 8 * math.sin(phase), ACCENT, 70, feather=90)

    for idx, r in enumerate((102, 146, 190)):
        spin = phase * (0.22 + idx * 0.07) + idx
        segs = 5 + idx
        for s in range(segs):
            base = spin + s * (2 * math.pi / segs)
            length = 0.30 + mood * 0.22
            color = HOT_CORE if idx == 0 else (WARM if idx == 1 else ACCENT)
            _arc(arr, cx, cy, r, base, base + length, color, 105 - idx * 18, width=max(2, 5 - idx))

    # Small orbit particles.
    for p in range(8):
        a = phase * (0.55 if p % 2 else -0.42) + p * math.tau / 8
        r = 120 + (p % 3) * 26
        _soft_orb(arr, cx + r * math.cos(a), cy + r * math.sin(a), 4.5,
                  HOT_CORE if p % 3 == 0 else ACCENT, 135, feather=7)


def _round_mask(arr, canvas):
    """Clip everything outside the inscribed circle to transparent."""
    yy, xx = np.mgrid[0:canvas, 0:canvas]
    c = (canvas - 1) / 2.0
    r = (canvas / 2.0) - 2.0  # 2px margin for clean edge
    outside = (xx - c) ** 2 + (yy - c) ** 2 > r ** 2
    arr[:, :, 3][outside] = 0
    return arr


# ---- per-state frame renderers --------------------------------------------
def render_idle_frame(canvas, i, n):
    """Jarvis idle sprite: breathing HUD shell + visor. No default white eyes."""
    arr = _blank(canvas)
    cx = cy = (canvas - 1) / 2.0
    phase = 2 * math.pi * (i / n)
    breath = 0.5 + 0.5 * math.sin(phase)
    _hud_shell(arr, cx, cy, phase, mood=0.15 + 0.18 * breath)
    _visor(arr, cx, cy, 0.16 + 0.08 * breath, ACCENT, 165 + int(55 * breath))
    _arc(arr, cx, cy + 48, 42, math.radians(22), math.radians(158), HOT_CORE, 135, width=3, steps=48)
    return _round_mask(arr, canvas)


def render_ramp_frame(canvas, i, n, hot_bias):
    """Amplitude sprite. It uses HUD rings + visor + segmented bars, not a blob."""
    arr = _blank(canvas)
    reach = i / max(1, (n - 1))                    # 0..1
    cx = cy = (canvas - 1) / 2.0
    phase = reach * math.tau * 1.25

    rgb = (_lerp_col(CYAN_GLOW, HOT_CORE, 0.18) if hot_bias < 0.5
           else _lerp_col(ACCENT, HOT_CORE, 0.72))
    accent = CYAN_GLOW if hot_bias < 0.5 else ACCENT

    _hud_shell(arr, cx, cy, phase, mood=reach)
    _soft_orb(arr, cx, cy + 8, 35 + reach * 92, accent, int(35 + reach * 115), feather=52)
    _visor(arr, cx, cy, 0.25 + reach * 0.58, rgb, int(155 + reach * 85))
    _voice_bars(arr, cx, cy + 78, reach, rgb, int(105 + reach * 130), phase)
    # Energy teeth at the bottom, giving the sprite more character on loud frames.
    for k in range(9):
        a = math.radians(212 + k * 14)
        r0 = 156
        r1 = 172 + reach * 26 * (0.6 + 0.4 * math.sin(phase + k))
        _stroke_poly(arr,
                     [(cx + r0 * math.cos(a), cy + r0 * math.sin(a)),
                      (cx + r1 * math.cos(a), cy + r1 * math.sin(a))],
                     rgb, int(70 + reach * 105), width=2)
    return _round_mask(arr, canvas)


def render_think_frame(canvas, i, n):
    """Thinking sprite: scanner ring, orbit particles, narrowed visor."""
    arr = _blank(canvas)
    cx = cy = (canvas - 1) / 2.0
    phase = i / n * math.tau
    _hud_shell(arr, cx, cy, phase, mood=0.35)
    _visor(arr, cx, cy, 0.08, ACCENT, 170)

    sweep = phase * 1.35
    _arc(arr, cx, cy, 174, sweep, sweep + math.radians(72), HOT_CORE, 210, width=6, steps=72)
    _arc(arr, cx, cy, 132, -sweep * 0.8, -sweep * 0.8 + math.radians(42), WARM, 145, width=4, steps=48)
    for trail in range(6):
        a = sweep - trail * 0.26
        r = 174
        _soft_orb(arr, cx + r * math.cos(a), cy + r * math.sin(a), 5.5,
                  HOT_CORE if trail == 0 else WARM, int(210 / (trail + 1)), feather=10)

    return _round_mask(arr, canvas)


RENDERERS = {
    "rwave_idle": lambda c, i, n: render_idle_frame(c, i, n),
    "rwave_listen": lambda c, i, n: render_ramp_frame(c, i, n, hot_bias=0.0),
    "rwave_think": lambda c, i, n: render_think_frame(c, i, n),
    "rwave_speak": lambda c, i, n: render_ramp_frame(c, i, n, hot_bias=1.0),
}


# ----------------------------------------------------------------------------
# RGBA frame -> (rows, palette_bgra) for emit_eaf(). Index 0 = transparent.
# A SHARED palette across all frames of a state keeps the EAF self-consistent
# (emit_eaf writes one palette per _S block; we pass the same one each frame).
# ----------------------------------------------------------------------------
def build_palette(frames_rgba):
    """Build one shared <=256-colour BGRA palette (index 0 transparent) from all
    frames of a state, plus a fast (r,g,b)->index lookup."""
    # Collect semi-opaque+ colours across every frame, quantize together.
    # Threshold lowered from 128 to 40: the Pulsing Orb design uses soft
    # Gaussian glow with alpha often in 20-120 range at low reach, so a
    # 128 threshold would discard ALL pixels from early ramp frames.
    stack = []
    for arr in frames_rgba:
        opaque = arr[:, :, 3] >= 40
        stack.append(arr[:, :, :3][opaque])
    allpx = np.concatenate(stack, axis=0) if stack else np.zeros((1, 3), np.uint8)
    pal_src = Image.fromarray(allpx.reshape(-1, 1, 3).astype(np.uint8), "RGB")
    pal_img = pal_src.quantize(colors=255, method=Image.MEDIANCUT, dither=Image.NONE)
    pal_rgb = pal_img.getpalette() or []
    ncol = min(255, len(pal_rgb) // 3)               # PIL may return < 255 colours
    palette_bgra = [(0, 0, 0, 0)]
    pal_list = []
    for k in range(ncol):
        r, g, b = pal_rgb[k * 3], pal_rgb[k * 3 + 1], pal_rgb[k * 3 + 2]
        palette_bgra.append((b, g, r, 255))
        pal_list.append((r, g, b))
    # pad palette to 256 entries (emit_eaf writes 256 BGRA slots regardless)
    while len(palette_bgra) < 256:
        palette_bgra.append((0, 0, 0, 0))
    pal_arr = np.array(pal_list, dtype=np.int16)     # 255x3, palette index k -> k+1
    return palette_bgra, pal_arr


def quantize_frame(arr, pal_arr, alpha_cutoff=40):
    """Map an RGBA frame to 8-bit palette indices using the shared palette.
    Transparent pixels -> 0; else nearest palette colour -> 1..255."""
    h, w = arr.shape[:2]
    rgb = arr[:, :, :3].astype(np.int16).reshape(-1, 3)
    # nearest-colour in RGB (small palette, brute force is fine for host tooling)
    d = ((rgb[:, None, :] - pal_arr[None, :, :]) ** 2).sum(axis=2)   # (HW,255)
    idx = d.argmin(axis=1).astype(np.uint8) + 1                      # 1..255
    idx = idx.reshape(h, w)
    transparent = arr[:, :, 3] < alpha_cutoff
    idx[transparent] = 0
    return [bytes(idx[y].tolist()) for y in range(h)]


# ----------------------------------------------------------------------------
# Bake one state -> .eaf
# ----------------------------------------------------------------------------
def bake_state(state, canvas, n_frames, enc=ENC_RLE):
    rnd = RENDERERS[state]
    frames_rgba = [rnd(canvas, i, n_frames) for i in range(n_frames)]
    palette_bgra, pal_arr = build_palette(frames_rgba)
    frames_rows = [quantize_frame(arr, pal_arr) for arr in frames_rgba]
    EAF_DIR.mkdir(parents=True, exist_ok=True)
    out = EAF_DIR / f"{state}.eaf"
    emit_eaf_enc(frames_rows, palette_bgra, out, enc)
    total = assert_roundtrip(out)                    # total includes the _C sentinel
    enc_name = "rle" if enc == ENC_RLE else "raw"
    print(f"  [{state}] -> {out}  ({out.stat().st_size} B, {n_frames} frames "
          f"+ _C = {total}, {canvas}x{canvas}, enc={enc_name}, round-trip OK)")
    return out


def check_state(state, canvas, n_frames):
    """Host-side sanity check for the baked design before it reaches hardware."""
    rnd = RENDERERS[state]
    frames = [rnd(canvas, i, n_frames) for i in range(n_frames)]
    alpha = [int((fr[:, :, 3] >= 40).sum()) for fr in frames]
    brightest = max(int(fr[:, :, :3].max()) for fr in frames)
    whiteish = max(int(((fr[:, :, 0] > 245) & (fr[:, :, 1] > 245) & (fr[:, :, 2] > 245)
                        & (fr[:, :, 3] >= 40)).sum()) for fr in frames)
    if min(alpha) == 0:
        raise SystemExit(f"{state}: blank frame detected")
    if whiteish:
        raise SystemExit(f"{state}: {whiteish} white-ish pixels detected")
    if state in ("rwave_listen", "rwave_speak") and alpha[-1] <= alpha[0]:
        raise SystemExit(f"{state}: ramp does not gain visible energy")
    print(f"  [{state}] check OK: alpha pixels {min(alpha)}..{max(alpha)}, "
          f"brightest channel={brightest}, white-ish pixels=0")


def write_preview(state, canvas, n_frames):
    PREVIEW_DIR.mkdir(parents=True, exist_ok=True)
    rnd = RENDERERS[state]
    pad = 8
    tiles = [Image.fromarray(rnd(canvas, i, n_frames), "RGBA") for i in range(n_frames)]
    cols = min(n_frames, 8)
    rows = (n_frames + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * canvas + pad * (cols + 1),
                               rows * canvas + pad * (rows + 1)), (8, 8, 10, 255))
    for k, im in enumerate(tiles):
        r, c = divmod(k, cols)
        sheet.alpha_composite(im, (pad + c * (canvas + pad), pad + r * (canvas + pad)))
    out = PREVIEW_DIR / f"{state}.png"
    sheet.convert("RGB").save(out)
    print(f"  [{state}] preview -> {out}")


# ----------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["all", "one", "preview", "check"])
    ap.add_argument("state", nargs="?", choices=STATE_ORDER,
                    help="for 'one': which state to bake")
    ap.add_argument("--canvas", type=int, default=DEFAULT_CANVAS,
                    help=f"frame size (square), default {DEFAULT_CANVAS}")
    ap.add_argument("--frames", type=int, default=DEFAULT_FRAMES,
                    help=f"frames per state, default {DEFAULT_FRAMES}")
    ap.add_argument("--enc", choices=["raw", "rle"], default="rle",
                    help="EAF block encoding. rle (default) compresses the mostly-"
                         "black reactive frames hugely — needed to fit the 6MB emote "
                         "partition alongside the eye assets. raw = uncompressed.")
    args = ap.parse_args()
    enc = ENC_RLE if args.enc == "rle" else ENC_RAW

    print(f"Reactive face generator: canvas={args.canvas} frames={args.frames} enc={args.enc}")
    if args.cmd == "check":
        for s in STATE_ORDER:
            check_state(s, args.canvas, args.frames)
    elif args.cmd == "preview":
        for s in STATE_ORDER:
            write_preview(s, args.canvas, args.frames)
    elif args.cmd == "one":
        if not args.state:
            ap.error("'one' requires a state name")
        bake_state(args.state, args.canvas, args.frames, enc)
    else:
        for s in STATE_ORDER:
            bake_state(s, args.canvas, args.frames, enc)
    print("Done. Mount the .eaf into the `emote` partition (see plan §7.3) before flashing.")


if __name__ == "__main__":
    sys.exit(main())
