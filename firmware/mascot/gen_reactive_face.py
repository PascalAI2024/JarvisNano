#!/usr/bin/env python3
"""Bake the reactive "Siri-style" waveform face into per-state EAF animations.

This produces FOUR looping EAF files — one per face state — for the
`esp_emote_gfx` `gfx_anim` widget. NO runtime pixel drawing happens on the device;
all motion is in these pre-rendered frames (the engine has no canvas widget — a
CPU-drawn RGB565A8 buffer spirals on the CO5300 panel, proven empirically). The
runtime code (firmware/emote/reactive_face.c) only selects which baked frames to
loop, using the live audio amplitude. See docs/REACTIVE_FACE_PLAN.md.

States (names are a HARD CONTRACT with reactive_face.c's RWAVE_ASSET_* strings):
  rwave_idle    calm breathing horizontal bar/dot (full-loop, not reactive)
  rwave_listen  amplitude RAMP: frame 0 = flat line, frame K-1 = full reach (mic)
  rwave_think   a scanning pulse sweeping left<->right over a dim track (full-loop)
  rwave_speak   same ramp geometry as listen, hotter palette bias (output RMS)

The listen/speak files are baked as a MONOTONIC amplitude ramp so the runtime can
play [0..end] where `end` tracks loudness — louder voice = waveform reaches wider.
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

from PIL import Image
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

DEFAULT_CANVAS = 360                       # see plan §5 (smaller than panel on purpose)
DEFAULT_FRAMES = 16                        # per-state frame count (size/quality tradeoff)

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


def _stamp(arr, x, y, rgb, alpha):
    """Additive-ish stamp of a single antialiased-ish pixel with clamping."""
    h, w = arr.shape[:2]
    if 0 <= x < w and 0 <= y < h:
        a = max(0, min(255, int(alpha)))
        if a >= arr[y, x, 3]:
            arr[y, x, 0] = rgb[0]
            arr[y, x, 1] = rgb[1]
            arr[y, x, 2] = rgb[2]
            arr[y, x, 3] = a


def _vbar(arr, cx, half_h, thickness, rgb_core, rgb_edge, alpha):
    """Draw a vertical rounded bar centred at column cx, half-height half_h."""
    h = arr.shape[0]
    cy = (h - 1) / 2.0
    for dy in range(-int(half_h), int(half_h) + 1):
        y = int(round(cy + dy))
        edge_t = abs(dy) / (half_h + 1.0)            # 0 centre .. 1 tip
        rgb = _lerp_col(rgb_core, rgb_edge, edge_t)
        a = alpha * (1.0 - 0.55 * edge_t)            # fade toward the tips
        for dx in range(-thickness, thickness + 1):
            xa = a * (1.0 - 0.5 * abs(dx) / (thickness + 1.0))
            _stamp(arr, cx + dx, y, rgb, xa)


def _round_mask(arr, canvas):
    """Clip everything outside the inscribed circle to transparent."""
    yy, xx = np.mgrid[0:canvas, 0:canvas]
    c = (canvas - 1) / 2.0
    r = (canvas / 2.0) - 4.0
    outside = (xx - c) ** 2 + (yy - c) ** 2 > r ** 2
    arr[:, :, 3][outside] = 0
    return arr


# ---- per-state frame renderers --------------------------------------------
def render_idle_frame(canvas, i, n):
    """Calm breathing centre line: a soft horizontal bar that gently pulses."""
    arr = _blank(canvas)
    cy = (canvas - 1) / 2.0
    phase = 2 * math.pi * (i / n)
    breath = 0.5 + 0.5 * math.sin(phase)             # 0..1
    half_w = int(canvas * (0.16 + 0.05 * breath))    # line length pulses a touch
    thick = 2
    base_a = 70 + 120 * breath
    cx0 = canvas // 2
    for dx in range(-half_w, half_w + 1):
        # taper alpha toward the line ends so it reads as a soft dash
        t = abs(dx) / (half_w + 1.0)
        a = base_a * (1.0 - 0.7 * t)
        rgb = _lerp_col(HOT_CORE, ACCENT, t)
        for ty in range(-thick, thick + 1):
            _stamp(arr, cx0 + dx, int(round(cy)) + ty, rgb, a * (1 - 0.4 * abs(ty) / (thick + 1)))
    return _round_mask(arr, canvas)


def render_ramp_frame(canvas, i, n, hot_bias):
    """Amplitude ramp: frame 0 = flat line, frame n-1 = full waveform reach.

    A symmetric set of vertical bars whose heights follow a smooth envelope.
    `i/(n-1)` is the global reach; per-bar height also has a fixed shape so the
    waveform looks organic. hot_bias (0..1) shifts the palette warmer (speak).
    """
    arr = _blank(canvas)
    reach = i / max(1, (n - 1))                      # 0..1 overall amplitude
    nbars = 9
    span = int(canvas * 0.34)                        # half-width the bars occupy
    cx0 = canvas // 2
    core = _lerp_col(ACCENT, HOT_CORE, 0.4 + 0.6 * hot_bias)
    edge = ACCENT
    # fixed per-bar shape (centre tallest), then scaled by reach
    shape = [0.35, 0.55, 0.72, 0.88, 1.0, 0.88, 0.72, 0.55, 0.35]
    for b in range(nbars):
        frac = (b - (nbars - 1) / 2.0) / ((nbars - 1) / 2.0)   # -1..1
        cx = int(cx0 + frac * span)
        s = shape[b] if b < len(shape) else 0.4
        # minimum 3px so frame 0 is a flat thin line, growing to full reach
        half_h = 3 + s * reach * (canvas * 0.30)
        thick = 3
        alpha = 150 + 105 * (0.4 + 0.6 * reach)
        _vbar(arr, cx, half_h, thick, core, edge, alpha)
    return _round_mask(arr, canvas)


def render_think_frame(canvas, i, n):
    """Scanning pulse: a bright dot sweeps left<->right over a dim track."""
    arr = _blank(canvas)
    cy = int((canvas - 1) / 2.0)
    track_half = int(canvas * 0.30)
    cx0 = canvas // 2
    # dim full track
    for dx in range(-track_half, track_half + 1):
        _stamp(arr, cx0 + dx, cy, DIM, 70)
        _stamp(arr, cx0 + dx, cy - 1, DIM, 45)
        _stamp(arr, cx0 + dx, cy + 1, DIM, 45)
    # bright head sweeping (triangle wave so it bounces L<->R)
    tw = i / n
    tri = 1.0 - abs(2.0 * tw - 1.0)                  # 0..1..0
    head_x = int(cx0 + (tri * 2 - 1) * track_half)
    for dx in range(-10, 11):
        falloff = max(0.0, 1.0 - abs(dx) / 10.0)
        rgb = _lerp_col(ACCENT, HOT_CORE, falloff)
        for dy in range(-3, 4):
            a = 255 * falloff * (1.0 - 0.5 * abs(dy) / 4.0)
            _stamp(arr, head_x + dx, cy + dy, rgb, a)
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
    # Collect opaque colours across every frame, quantize together.
    stack = []
    for arr in frames_rgba:
        opaque = arr[:, :, 3] >= 128
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


def quantize_frame(arr, pal_arr):
    """Map an RGBA frame to 8-bit palette indices using the shared palette.
    Transparent (alpha<128) -> 0; else nearest palette colour -> 1..255."""
    h, w = arr.shape[:2]
    rgb = arr[:, :, :3].astype(np.int16).reshape(-1, 3)
    # nearest-colour in RGB (small palette, brute force is fine for host tooling)
    d = ((rgb[:, None, :] - pal_arr[None, :, :]) ** 2).sum(axis=2)   # (HW,255)
    idx = d.argmin(axis=1).astype(np.uint8) + 1                      # 1..255
    idx = idx.reshape(h, w)
    transparent = arr[:, :, 3] < 128
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
    ap.add_argument("cmd", choices=["all", "one", "preview"])
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
    if args.cmd == "preview":
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
