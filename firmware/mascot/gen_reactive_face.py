#!/usr/bin/env python3
"""Bake the reactive premium face into per-state EAF animations.

This produces FOUR looping EAF files — one per face state — for the
`esp_emote_gfx` `gfx_anim` widget. NO runtime pixel drawing happens on the device;
all motion is in these pre-rendered frames (the engine has no canvas widget — a
CPU-drawn RGB565A8 buffer spirals on the CO5300 panel, proven empirically). The
runtime code (firmware/emote/reactive_face.c) only selects which baked frames to
loop, using the live audio amplitude. See docs/REACTIVE_FACE_PLAN.md.

States (names are a HARD CONTRACT with reactive_face.c's RWAVE_ASSET_* strings):
  rwave_idle    low amber standby: slow halo, dim core, stable visor
  rwave_listen  cyan/teal receive state: smooth lower waveform, no sparkle noise
  rwave_think   violet scanner state: slow sweep, restrained orbit accents
  rwave_speak   warm amber transmit state: same geometry as listen, hotter core

The listen/speak files are baked as a MONOTONIC amplitude ramp so the runtime can
select small loop windows around the current loudness bucket. Louder voice = a
deeper, brighter section of the ramp without resetting playback every tick. The
idle/think files are baked as self-contained motion loops.

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

# Brand/state accents on pure black. AMOLED likes restraint. How terribly modern.
HOT_CORE = (0xFF, 0xE2, 0x5E)              # #FFE25E hot yellow core
ACCENT = (0xF5, 0x87, 0x0B)               # #F5870B amber
DIM = (0x6A, 0x3A, 0x05)                   # dim amber (idle rest / think track)
WARM = (0xFF, 0xB8, 0x2C)                  # warm intermediate
CYAN_GLOW = (0x4A, 0xF0, 0xE0)             # cyan accent for listening
TEAL_DEEP = (0x12, 0x8A, 0x83)             # listening shadow
THINK = (0x78, 0x86, 0xFF)                 # blue-violet scanner, not pink
THINK_DIM = (0x18, 0x20, 0x58)

DEFAULT_CANVAS = 466                       # FULL panel — use every pixel of the AMOLED
DEFAULT_FRAMES = 28                        # calmer motion without bloating the asset pack
PALETTE_COLORS = 96                        # stylized gradients, sane RLE output

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
    """Draw a calm non-white visor core."""
    img = Image.fromarray(arr, "RGBA")
    draw = ImageDraw.Draw(img, "RGBA")
    w = int(138 + 20 * openness)
    h = int(18 + 32 * openness)
    y = int(cy - 36)
    x0 = int(cx - w / 2)
    x1 = int(cx + w / 2)
    y0 = int(y - h / 2)
    y1 = int(y + h / 2)
    draw.rounded_rectangle((x0, y0, x1, y1), radius=max(8, h // 2),
                           outline=(rgb[0], rgb[1], rgb[2], alpha), width=3)
    draw.line((x0 + 24, y, x1 - 24, y),
              fill=(HOT_CORE[0], HOT_CORE[1], HOT_CORE[2], min(235, alpha + 35)),
              width=2)
    arr[:] = np.array(img)


def _voice_bars(arr, cx, cy, reach, rgb, alpha, bars=9):
    energy = reach * reach * (3.0 - 2.0 * reach)
    if energy < 0.035:
        return
    img = Image.fromarray(arr, "RGBA")
    draw = ImageDraw.Draw(img, "RGBA")
    span = 214
    step = span / (bars - 1)
    for k in range(bars):
        n = k / (bars - 1)
        env = math.sin(n * math.pi)
        h = 3 + energy * env * 122
        x = cx - span / 2 + k * step
        w = 5 + 2 * env
        draw.rounded_rectangle((x - w / 2, cy - h / 2, x + w / 2, cy + h / 2),
                               radius=5,
                               fill=(rgb[0], rgb[1], rgb[2],
                                     int(alpha * energy * (0.30 + 0.70 * env))))
    arr[:] = np.array(img)


def _premium_shell(arr, cx, cy, phase, primary, secondary, mood=0.0):
    """Round AMOLED-friendly face shell: one halo and restrained instrument marks."""
    pulse = 0.5 + 0.5 * math.sin(phase)
    _soft_orb(arr, cx, cy + 12, 34 + 14 * mood + 3 * pulse, primary,
              int(20 + 32 * mood), feather=56)

    outer = 178 + 2 * pulse
    inner = 118 + 2 * math.sin(phase + 1.2)
    spin = phase * 0.07
    for s in range(3):
        base = spin + s * math.tau / 3
        _arc(arr, cx, cy, outer, base + 0.10, base + 0.52,
             secondary, int(42 + 34 * mood), width=3, steps=42)
    for s in range(2):
        base = -spin * 0.7 + s * math.pi + 0.38
        _arc(arr, cx, cy, inner, base, base + 0.26,
             primary, int(72 + 38 * mood), width=4, steps=34)

    for a in (-math.pi * 0.72, -math.pi * 0.30, math.pi * 0.30, math.pi * 0.72):
        r0 = 150
        r1 = 158
        _stroke_poly(arr,
                     [(cx + r0 * math.cos(a), cy + r0 * math.sin(a)),
                      (cx + r1 * math.cos(a), cy + r1 * math.sin(a))],
                     primary, int(38 + 42 * mood), width=2)


def _finish_frame(arr, canvas):
    """Composite the drawn glow onto opaque black so every baked frame clears."""
    out = np.zeros((canvas, canvas, 4), dtype=np.uint8)
    alpha = arr[:, :, 3:4].astype(np.uint16)
    rgb = (arr[:, :, :3].astype(np.uint16) * alpha) // 255
    rgb[rgb < 7] = 0
    rgb = np.minimum(((rgb + 3) // 6) * 6, 255)
    out[:, :, :3] = rgb.astype(np.uint8)
    out[:, :, 3] = 255
    return out


# ---- per-state frame renderers --------------------------------------------
def render_idle_frame(canvas, i, n):
    """Idle sprite: slow amber breathing with a stable center."""
    arr = _blank(canvas)
    cx = cy = (canvas - 1) / 2.0
    phase = 2 * math.pi * (i / n)
    breath = 0.5 + 0.5 * math.sin(phase)
    _premium_shell(arr, cx, cy, phase, ACCENT, DIM, mood=0.10 + 0.16 * breath)
    _soft_orb(arr, cx, cy + 22, 26 + 5 * breath, ACCENT, 44 + int(26 * breath), feather=42)
    _visor(arr, cx, cy, 0.10 + 0.06 * breath, ACCENT, 140 + int(42 * breath))
    return _finish_frame(arr, canvas)


def render_ramp_frame(canvas, i, n, hot_bias):
    """Amplitude sprite: clean state colour, stable geometry, smooth energy gain."""
    arr = _blank(canvas)
    reach = i / max(1, (n - 1))                    # 0..1
    cx = cy = (canvas - 1) / 2.0

    if hot_bias < 0.5:
        rgb = _lerp_col(CYAN_GLOW, HOT_CORE, 0.10)
        accent = CYAN_GLOW
        shell = TEAL_DEEP
    else:
        rgb = _lerp_col(WARM, HOT_CORE, 0.45)
        accent = ACCENT
        shell = DIM

    phase = reach * math.tau
    _premium_shell(arr, cx, cy, phase, accent, shell, mood=0.18 + 0.58 * reach)
    _soft_orb(arr, cx, cy + 28, 30 + reach * 42, accent,
              int(24 + reach * 76), feather=42)
    _visor(arr, cx, cy, 0.18 + reach * 0.42, rgb, int(150 + reach * 70))
    _voice_bars(arr, cx, cy + 82, reach, rgb, int(95 + reach * 120))
    return _finish_frame(arr, canvas)


def render_think_frame(canvas, i, n):
    """Thinking sprite: slow violet scanner, no glitter storm."""
    arr = _blank(canvas)
    cx = cy = (canvas - 1) / 2.0
    phase = i / n * math.tau
    _premium_shell(arr, cx, cy, phase, THINK, THINK_DIM, mood=0.34)
    _visor(arr, cx, cy, 0.06, THINK, 168)

    sweep = phase * 0.85
    _arc(arr, cx, cy, 174, sweep, sweep + math.radians(54), THINK, 198, width=5, steps=58)
    _arc(arr, cx, cy, 132, -sweep * 0.72, -sweep * 0.72 + math.radians(34),
         ACCENT, 110, width=3, steps=38)
    a = sweep
    r = 174
    _soft_orb(arr, cx + r * math.cos(a), cy + r * math.sin(a), 4.0,
              THINK, 145, feather=8)

    return _finish_frame(arr, canvas)


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
    # Index 1 is opaque black. Index 0 remains transparent for format hygiene,
    # but the baked frames deliberately paint black so old pixels get cleared.
    stack = []
    for arr in frames_rgba:
        lit = (arr[:, :, 3] >= 40) & (arr[:, :, :3].max(axis=2) > 7)
        stack.append(arr[:, :, :3][lit])
    allpx = np.concatenate(stack, axis=0) if stack else np.zeros((1, 3), np.uint8)
    pal_src = Image.fromarray(allpx.reshape(-1, 1, 3).astype(np.uint8), "RGB")
    pal_img = pal_src.quantize(colors=PALETTE_COLORS - 1, method=Image.MEDIANCUT, dither=Image.NONE)
    pal_rgb = pal_img.getpalette() or []
    ncol = min(PALETTE_COLORS - 1, len(pal_rgb) // 3)
    palette_bgra = [(0, 0, 0, 0), (0, 0, 0, 255)]
    pal_list = [(0, 0, 0)]
    for k in range(ncol):
        r, g, b = pal_rgb[k * 3], pal_rgb[k * 3 + 1], pal_rgb[k * 3 + 2]
        if r <= 5 and g <= 5 and b <= 5:
            continue
        palette_bgra.append((b, g, r, 255))
        pal_list.append((r, g, b))
        if len(palette_bgra) >= 256:
            break
    # pad palette to 256 entries (emit_eaf writes 256 BGRA slots regardless)
    while len(palette_bgra) < 256:
        palette_bgra.append((0, 0, 0, 0))
    pal_arr = np.array(pal_list, dtype=np.int16)     # palette index k -> k+1
    return palette_bgra, pal_arr


def quantize_frame(arr, pal_arr, alpha_cutoff=40):
    """Map an RGBA frame to 8-bit palette indices using the shared palette.
    Transparent pixels -> 0; else nearest palette colour -> 1..255."""
    h, w = arr.shape[:2]
    rgb = arr[:, :, :3].astype(np.int32).reshape(-1, 3)
    pal = pal_arr.astype(np.int32)
    idx_flat = np.empty(rgb.shape[0], dtype=np.uint8)
    # Chunked nearest-colour in RGB. int32 is intentional: int16 squared wraps
    # and turns black into bright palette colours. Ask me how I know.
    chunk = 32768
    for off in range(0, rgb.shape[0], chunk):
        cur = rgb[off:off + chunk]
        d = ((cur[:, None, :] - pal[None, :, :]) ** 2).sum(axis=2)
        idx_flat[off:off + chunk] = d.argmin(axis=1).astype(np.uint8) + 1
    idx = idx_flat.reshape(h, w)
    transparent = arr[:, :, 3] < alpha_cutoff
    idx[transparent] = 0
    return [bytes(idx[y].tolist()) for y in range(h)]


def _rows_to_preview_image(rows, palette_bgra):
    """Render quantized EAF rows back to RGBA so previews show baked colours."""
    h = len(rows)
    w = len(rows[0])
    idx = np.frombuffer(b"".join(rows), dtype=np.uint8).reshape(h, w)
    lut = np.array([(r, g, b, a) for b, g, r, a in palette_bgra], dtype=np.uint8)
    return Image.fromarray(lut[idx], "RGBA")


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
    lit = [int((fr[:, :, :3].max(axis=2) >= 16).sum()) for fr in frames]
    energy = [int(fr[:, :, :3].max(axis=2).sum()) for fr in frames]
    transparent = max(int((fr[:, :, 3] < 255).sum()) for fr in frames)
    brightest = max(int(fr[:, :, :3].max()) for fr in frames)
    whiteish = max(int(((fr[:, :, 0] > 245) & (fr[:, :, 1] > 245) & (fr[:, :, 2] > 245)
                        & (fr[:, :, 3] >= 40)).sum()) for fr in frames)
    pinkish = max(int(((fr[:, :, 0] > 145) & (fr[:, :, 1] < 115) & (fr[:, :, 2] > 145)
                       & (fr[:, :, 3] >= 40)).sum()) for fr in frames)
    if min(lit) == 0:
        raise SystemExit(f"{state}: blank frame detected")
    if transparent:
        raise SystemExit(f"{state}: {transparent} transparent pixels detected")
    if whiteish:
        raise SystemExit(f"{state}: {whiteish} white-ish pixels detected")
    if pinkish:
        raise SystemExit(f"{state}: {pinkish} pink/magenta pixels detected")
    if state in ("rwave_listen", "rwave_speak") and energy[-1] <= energy[0]:
        raise SystemExit(f"{state}: ramp does not gain visible energy")
    print(f"  [{state}] check OK: lit pixels {min(lit)}..{max(lit)}, "
          f"brightest channel={brightest}, transparent=0, pink-ish=0, white-ish=0")


def write_preview(state, canvas, n_frames):
    PREVIEW_DIR.mkdir(parents=True, exist_ok=True)
    rnd = RENDERERS[state]
    pad = 8
    frames_rgba = [rnd(canvas, i, n_frames) for i in range(n_frames)]
    palette_bgra, pal_arr = build_palette(frames_rgba)
    frames_rows = [quantize_frame(arr, pal_arr) for arr in frames_rgba]
    tiles = [_rows_to_preview_image(rows, palette_bgra) for rows in frames_rows]
    cols = min(n_frames, 8)
    rows = (n_frames + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * canvas + pad * (cols + 1),
                               rows * canvas + pad * (rows + 1)), (0, 0, 0, 255))
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
