#!/usr/bin/env python3
"""Bake the reactive premium face into per-state EAF animations.

This produces FOUR looping EAF files — one per face state — for the
`esp_emote_gfx` `gfx_anim` widget. NO runtime pixel drawing happens on the device;
all motion is in these pre-rendered frames (the engine has no canvas widget — a
CPU-drawn RGB565A8 buffer spirals on the CO5300 panel, proven empirically). The
runtime code (firmware/emote/reactive_face.c) only selects which baked frames to
loop, using the live audio amplitude. See docs/ARCHIVE/REACTIVE_FACE_PLAN.md.

DESIGN (2026-06-10 redesign): "arc-reactor HUD ring" in Stark cyan/ice-blue.
Shared anatomy across all four states so transitions feel coherent: outer tick
bezel (r~210), ten-segment reactor coil (r 152..184), inner ring (r 128),
glowing core orb. Rendering is a scalar energy field -> per-state colour LUT,
which gives physically-plausible glow (hot pale centre, cyan falloff) for free.

States (names are a HARD CONTRACT with reactive_face.c's RWAVE_ASSET_* strings):
  rwave_idle    dim cyan standby: slow breathing core, bezel creeps one tick/loop
  rwave_listen  receive ramp: coil + core brighten, intake spokes grow with RMS
  rwave_think   azure scanner: counter-rotating sweep arcs + orbiting dot
  rwave_speak   transmit ramp: hotter ice-white core, expanding shock ring

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

# Stark cyan/ice-blue energy->colour ramps (pos, (r,g,b)). The LUT is what makes
# the reactor read as "glowing": low energy = deep teal, high energy = ice.
# NOTE: max red channel is 224 on purpose — keeps the check_state() white-ish
# guard (all channels > 245 = palette-corruption symptom) meaningful.
LUT_IDLE = [(0.00, (0, 0, 0)), (0.30, (5, 30, 40)), (0.60, (16, 120, 150)),
            (0.85, (70, 200, 228)), (1.00, (180, 240, 252))]
LUT_LISTEN = [(0.00, (0, 0, 0)), (0.30, (6, 38, 50)), (0.58, (22, 160, 196)),
              (0.85, (96, 224, 248)), (1.00, (200, 250, 255))]
LUT_THINK = [(0.00, (0, 0, 0)), (0.30, (8, 28, 60)), (0.58, (48, 128, 228)),
             (0.85, (132, 198, 252)), (1.00, (208, 238, 255))]
LUT_SPEAK = [(0.00, (0, 0, 0)), (0.28, (8, 44, 56)), (0.55, (36, 186, 214)),
             (0.82, (130, 236, 252)), (1.00, (224, 252, 255))]

DEFAULT_CANVAS = 466                       # FULL panel — use every pixel of the AMOLED
DEFAULT_FRAMES = 28                        # calmer motion without bloating the asset pack
PALETTE_COLORS = 96                        # stylized gradients, sane RLE output

# State -> (renderer key). Names are the HARD CONTRACT with reactive_face.c.
STATE_ORDER = ["rwave_idle", "rwave_listen", "rwave_think", "rwave_speak"]

# Per-state frame counts (used when --frames is left at the default). The
# runtime reads the real frame count from each EAF, so states may differ:
# ramps get ~2.75 frames per amplitude bucket (8 buckets) for a finer response;
# loops get more frames for fluid motion. Budget: emote partition is 0x6E0000
# (7,208,960 B) and the non-rwave eye assets take ~3.40 MB, leaving ~3.8 MB
# for the four rwave packs — these counts land ~3.7 MB at canvas 466 / RLE.
STATE_FRAMES = {"rwave_idle": 30, "rwave_listen": 22,
                "rwave_think": 32, "rwave_speak": 22}


def state_frames(state, args_frames):
    """Explicit --frames overrides for every state; default = per-state map."""
    if args_frames != DEFAULT_FRAMES:
        return args_frames
    return STATE_FRAMES.get(state, args_frames)


# ----------------------------------------------------------------------------
# Field-based renderer (host-side). Each frame is a float32 scalar energy field
# accumulated from vectorized primitives, then mapped through a per-state colour
# LUT. Nothing here ships to the device — output is baked EAF frames.
# ----------------------------------------------------------------------------
_GRID_CACHE = {}


def _grid(canvas):
    """Cached (dx, dy, dist, theta) float32 grids for a square canvas."""
    if canvas not in _GRID_CACHE:
        c = (canvas - 1) / 2.0
        yy, xx = np.mgrid[0:canvas, 0:canvas].astype(np.float32)
        dx = xx - c
        dy = yy - c
        dist = np.sqrt(dx * dx + dy * dy)
        theta = np.arctan2(dy, dx)
        _GRID_CACHE[canvas] = (dx, dy, dist, theta)
    return _GRID_CACHE[canvas]


def _f_ring(dist, r, w):
    """Gaussian ring: peak 1.0 at radius r, falloff width w."""
    return np.exp(-((dist - r) ** 2) / (2.0 * w * w))


def _f_orb(dist_sq, sigma):
    """Gaussian orb from a squared-distance field."""
    return np.exp(-dist_sq / (2.0 * sigma * sigma))


def _smooth(t):
    return t * t * (3.0 - 2.0 * t)


def _f_band(dist, r0, r1, soft=3.0):
    """Smooth radial band [r0..r1]."""
    a = np.clip((dist - r0) / soft, 0.0, 1.0)
    b = np.clip((r1 - dist) / soft, 0.0, 1.0)
    return _smooth(np.minimum(a, b))


def _f_comb(theta, n, rot, duty):
    """n evenly-spaced angular features ("ticks"/"segments"), lit fraction=duty."""
    s = 0.5 + 0.5 * np.cos(n * (theta - rot))
    cut = 0.5 + 0.5 * math.cos(math.pi * max(1e-3, min(1.0, duty)))
    f = np.clip((s - cut) / max(1e-6, 1.0 - cut), 0.0, 1.0)
    return _smooth(f)


def _f_angwin(theta, center, half, soft):
    """Smooth angular window of half-width `half` around `center` (wrapped)."""
    d = np.abs((theta - center + np.pi) % (2.0 * np.pi) - np.pi)
    return _smooth(np.clip((half + soft - d) / soft, 0.0, 1.0))


LUT_LEVELS = 15   # energy quantization steps. Stepped concentric glow reads as
                  # deliberate HUD banding AND is what makes the EAFs fit the
                  # 6 MB emote partition: long flat runs RLE-compress ~8x better
                  # than smooth Gaussian gradients (measured 8.3 MB -> budget).


def _apply_lut(E, lut, levels=LUT_LEVELS):
    """Energy field (0..1) -> opaque RGBA via colour ramp. The field is first
    quantized to `levels` bands (HUD-style stepped glow, tiny palette, long RLE
    runs); black floor keeps index 1 (opaque black) dominant."""
    E = np.clip(E, 0.0, 1.0)
    E = np.round(E * levels) / float(levels)
    pos = np.array([p for p, _ in lut], dtype=np.float32)
    cols = np.array([c for _, c in lut], dtype=np.float32)
    h, w = E.shape
    out = np.zeros((h, w, 4), dtype=np.uint8)
    for ch in range(3):
        out[:, :, ch] = np.interp(E, pos, cols[:, ch]).astype(np.uint8)
    dark = out[:, :, :3].max(axis=2) < 7
    out[dark, :3] = 0
    out[:, :, 3] = 255
    return out


def _reactor(canvas, *, rot_bezel=0.0, coil_gain=0.3, coil_rot=0.0,
             core_r=40.0, core_gain=0.4, ring_gain=0.6,
             spoke_reach=0.0, spoke_rot=0.0):
    """Shared arc-reactor anatomy. Returns (energy_field, grids)."""
    dx, dy, dist, theta = _grid(canvas)
    E = np.zeros_like(dist)

    # 1. Outer bezel: hairline ring + 60 minor / 12 major tick marks.
    E += 0.10 * ring_gain * _f_ring(dist, 210.0, 1.6)
    E += 0.22 * ring_gain * _f_comb(theta, 60, rot_bezel, 0.16) * _f_band(dist, 203.0, 212.0)
    E += 0.32 * ring_gain * _f_comb(theta, 12, rot_bezel, 0.045) * _f_band(dist, 198.0, 215.0)

    # 2. Ten-segment reactor coil — the iconic winding ring. Brighter at its
    #    inner edge so it reads as lit from the core.
    seg = _f_comb(theta, 10, coil_rot, 0.78) * _f_band(dist, 152.0, 184.0, 5.0)
    inner_shade = np.clip((184.0 - dist) / 32.0, 0.0, 1.0)
    E += coil_gain * seg * (0.38 + 0.45 * inner_shade)

    # 3. Inner containment ring.
    E += (0.30 + 0.55 * coil_gain) * ring_gain * _f_ring(dist, 128.0, 2.2)

    # 4. Intake/transmit spokes between core and inner ring (voice energy).
    if spoke_reach > 0.001:
        sp = _f_comb(theta, 24, spoke_rot, 0.30) * _f_band(dist, core_r + 12.0, 124.0, 4.0)
        near = np.clip((124.0 - dist) / max(1.0, 124.0 - core_r - 12.0), 0.0, 1.0)
        E += spoke_reach * 0.55 * sp * (0.40 + 0.60 * near)

    # 5. Core: soft halo + hot centre.
    dist_sq = dist * dist
    E += core_gain * _f_orb(dist_sq, core_r)
    E += 0.90 * core_gain * _f_orb(dist_sq, core_r * 0.45)

    return E, (dx, dy, dist, theta)


# ---- per-state frame renderers --------------------------------------------
def render_idle_frame(canvas, i, n):
    """Idle: dim reactor at rest. Bezel creeps exactly one minor-tick pitch per
    loop (seamless wrap); core breathes one full sine cycle."""
    ph = 2.0 * math.pi * i / n
    breath = 0.5 + 0.5 * math.sin(ph)
    E, _ = _reactor(canvas,
                    rot_bezel=(2.0 * math.pi / 60.0) * (i / n),
                    coil_gain=0.28 + 0.10 * breath,
                    core_r=40.0 + 4.0 * breath,
                    core_gain=0.34 + 0.14 * breath,
                    ring_gain=0.55 + 0.10 * breath)
    return _apply_lut(E * 0.92, LUT_IDLE)


def render_ramp_frame(canvas, i, n, hot_bias):
    """Monotonic amplitude ramp (frame index == loudness). Louder = brighter
    coil, larger/hotter core, longer spokes; spokes also sweep slightly with
    reach so a narrow runtime loop window still shows motion."""
    reach = i / max(1, n - 1)
    ease = _smooth(np.float32(reach)).item()
    hot = hot_bias >= 0.5
    # LISTEN baseline floor: frame 0 must read as "I'm receiving" on the panel
    # — users couldn't tell listening from idle in a quiet room (mic amp ~0 →
    # loop window stuck in frames 0..7, whose eased energy is near zero).
    # Floor only the SPOKES and INNER RING: thin strokes are cheap in RLE
    # (a whole-ramp lift cost +76 KB and most of the partition headroom) and
    # they are the elements idle doesn't have, so even frame 0 is unmistakable.
    spoke_floor = 0.0 if hot else 0.28
    ring_floor = 0.60 + 0.30 * ease
    if not hot:
        ring_floor = max(ring_floor, 0.85)
    E, (dx, dy, dist, theta) = _reactor(
        canvas,
        rot_bezel=(2.0 * math.pi / 60.0) * reach,
        coil_gain=0.34 + 0.55 * ease,
        coil_rot=(2.0 * math.pi / 10.0) * 0.25 * reach,
        core_r=42.0 + 26.0 * ease,
        core_gain=0.40 + 0.55 * ease,
        ring_gain=ring_floor,
        spoke_reach=max(ease, spoke_floor),
        spoke_rot=(2.0 * math.pi / 24.0) * 1.5 * reach)
    if hot:
        # Transmit shock ring expanding past the coil with output level.
        E += (0.10 + 0.30 * ease) * _f_ring(dist, 188.0 + 34.0 * ease, 3.0 + 6.0 * ease)
    return _apply_lut(E * (0.85 + 0.15 * ease), LUT_SPEAK if hot else LUT_LISTEN)


def render_think_frame(canvas, i, n):
    """Thinking: HUD scanner. Outer arc sweeps one full revolution per loop,
    inner arc counter-rotates two revolutions, dot orbits the bezel — all
    integer cycles so the loop wraps seamlessly."""
    t = i / n
    sweep = 2.0 * math.pi * t
    E, (dx, dy, dist, theta) = _reactor(
        canvas,
        rot_bezel=-(2.0 * math.pi / 60.0) * t,
        coil_gain=0.30,
        coil_rot=(2.0 * math.pi / 10.0) * t,
        core_r=40.0,
        core_gain=0.40 + 0.10 * math.sin(sweep),
        ring_gain=0.70)
    E += 0.55 * _f_ring(dist, 196.0, 2.5) * _f_angwin(theta, sweep, 0.45, 0.25)
    E += 0.45 * _f_ring(dist, 140.0, 2.2) * _f_angwin(theta, -2.0 * sweep, 0.30, 0.20)
    px = 196.0 * math.cos(sweep)
    py = 196.0 * math.sin(sweep)
    E += 0.85 * _f_orb((dx - px) ** 2 + (dy - py) ** 2, 5.0)
    return _apply_lut(E, LUT_THINK)


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
            check_state(s, args.canvas, state_frames(s, args.frames))
    elif args.cmd == "preview":
        for s in STATE_ORDER:
            write_preview(s, args.canvas, state_frames(s, args.frames))
    elif args.cmd == "one":
        if not args.state:
            ap.error("'one' requires a state name")
        bake_state(args.state, args.canvas, state_frames(args.state, args.frames), enc)
    else:
        for s in STATE_ORDER:
            bake_state(s, args.canvas, state_frames(s, args.frames), enc)
    print("Done. Mount the .eaf into the `emote` partition (see plan §7.3) before flashing.")


if __name__ == "__main__":
    sys.exit(main())
