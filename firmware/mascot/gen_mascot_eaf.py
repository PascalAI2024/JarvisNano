#!/usr/bin/env python3
"""Generate the AMOLED center-disc mascot face .eaf files.

Pipeline per state keyframe:
  1. Gemini 2.5 Flash Image (nano banana), image-to-image conditioned on the
     LOCKED base (images/mascot-amoled-concept-locked.png) so every frame stays
     on-model: chibi JARVIS helmet bust, matte-black armor, dark visor (no face),
     headphone ear-cups, glowing "J" chest emblem, arc-reactor orange rim-glow
     (#F5870B) with hot yellow-white core (#FFE25E), near-black background.
  2. PIL crop/center the character into a 360x360 canvas, art within a circle of
     diameter 352 (4px transparent margin) per DISPLAY_UX_PLAN.md Phase 2B.
  3. Round mask: key the near-black background to transparent and clip everything
     outside the visible circle. The alpha channel IS the round mask (Phase 2A).
  4. Quantize to a <=256-color indexed palette with ADAPTIVE palette +
     Floyd-Steinberg dither so the orange->hot-core glow gradient does not
     posterize. Reserve palette index 0 as fully-transparent (BGRA 00 00 00 00).
  5. emit_eaf() (verbatim from DISPLAY_UX_PLAN.md Phase 2A, round-trip-tested)
     into the disc collection, then re-parse to assert magic/checksum/_S/_C.

States (emote names are a HARD CONTRACT with the shipped emote_set_* helpers):
  neutral, listen, thinking, happy, offline  -- convey state through GLOW
  intensity/color + pose/lean, NOT facial expressions (the mascot has a visor).

Usage:
  python3 gen_mascot_eaf.py states           # 1 representative frame per state
  python3 gen_mascot_eaf.py states --no-gen   # re-pack existing PNGs (no API)
  python3 gen_mascot_eaf.py contact-sheet     # assemble the 5-state review sheet

Requires: Pillow, numpy, GEMINI_API_KEY (falls back to the imagine.sh default).
DO NOT flash or build from here -- the orchestrator owns the single board and the
integrated build. This script only writes PNGs + .eaf files into firmware/mascot/.
"""
import argparse
import base64
import json
import os
import struct
import sys
import urllib.request
from pathlib import Path

from PIL import Image

# ----------------------------------------------------------------------------
# Paths & constants
# ----------------------------------------------------------------------------
REPO = Path(__file__).resolve().parents[2]
LOCKED_BASE = REPO / "images" / "mascot-amoled-concept-locked.png"
OUT_DIR = Path(__file__).resolve().parent
RAW_DIR = OUT_DIR / "raw"          # generated PNGs (pre-processing)
DISC_DIR = OUT_DIR / "disc"        # processed 360x360 RGBA PNGs (preview)
EAF_DIR = OUT_DIR / "eaf"          # packed .eaf disc pack (the deliverable)
# The .eaf + config.json/emote.json next to this script ARE the tracked source
# assets. bootstrap.sh installs them into the esp-claw clone's
# assets_local/emoji_hud_466/ + assets_local/466_466/ (same pattern as
# firmware/lua + firmware/router_rules), wired by firmware-eng alongside the
# CMake 284_240 -> 466_466 switch. This script never writes into esp-claw/.

CANVAS = 360                       # .eaf disc canvas (Phase 2B)
ART_DIA = 352                      # visible art circle (4px transparent margin)
ART_R = ART_DIA / 2.0

# Brand-exact accents (Phase 2B): used only to bias the generation prompt.
HOT_CORE = "#FFE25E"
ORANGE = "#F5870B"

# Image-to-image is done via Kie.ai Flux Kontext (the imagine skill's Gemini key
# is CONSUMER_SUSPENDED). Flux needs the reference as a public URL, so we pin the
# locked base to its immutable raw.githubusercontent SHA (commit ca20416).
KIE_API_KEY = os.environ.get("KIE_API_KEY")  # set via env; NEVER hardcode (open-source repo)
KIE_MODEL = os.environ.get("KIE_MODEL", "flux-kontext-pro")  # or flux-kontext-max
LOCKED_BASE_URL = os.environ.get(
    "LOCKED_BASE_URL",
    "https://raw.githubusercontent.com/PascalAI2024/JarvisNano/"
    "ca204165b2aad697cdc1b57e2e65de0dfe112cf4/"
    "images/mascot-amoled-concept-locked.png")

# State -> prompt. Each prompt restates the locked identity then varies ONLY the
# glow/pose for that voice state. The mascot has a visor and no face, so emotion
# is carried by glow intensity/color + slight lean.
IDENTITY = (
    "Keep this EXACT character on-model: a chibi Funko-style J.A.R.V.I.S. helmet "
    "mascot bust, smooth matte-black armored body, oversized rounded helmet, a "
    "dark visor across the upper face (NO facial features), integrated headphone "
    "ear-cups on both sides, a glowing 'J' monogram on the chest. Neon rim-glow "
    f"emitting from the accent lines: signature orange {ORANGE} ring with a hot "
    f"yellow-white core {HOT_CORE}. Pure near-black background. Centered, "
    "front-facing, slightly forward-leaning, tight bust crop with empty margin "
    "around the figure so nothing touches the frame edges. Flat icon look, no "
    "external lighting -- the glow EMITS from the accent lines."
)
STATE_PROMPTS = {
    "neutral": IDENTITY + " STATE: idle. Calm, steady, even orange rim-glow at "
        "moderate intensity. Relaxed upright pose. This is the resting look.",
    "listen": IDENTITY + " STATE: listening. Brighter overall glow, the headphone "
        "ear-cup rings clearly lit and energized, a subtle cyan accent mixed into "
        "the rim glow, slight attentive forward lean. Alert and receptive.",
    "thinking": IDENTITY + " STATE: thinking. Glow shifting and animated, helmet "
        "tilted slightly upward as if considering, the chest 'J' and visor edge "
        "pulsing. A contemplative 'looking up' feel.",
    "happy": IDENTITY + " STATE: speaking/energized. Glow pulsing noticeably "
        "brighter and warmer, the hot yellow-white core dominant, radiant and "
        "lively. Confident, expressive, full of energy.",
    "offline": IDENTITY + " STATE: offline/powered-down. Visor dark and dim, the "
        "rim-glow nearly extinguished to a faint ember, body in shadow. A sleeping, "
        "powered-down pose. Very dark overall.",
}
STATE_ORDER = ["neutral", "listen", "thinking", "happy", "offline"]

# ----------------------------------------------------------------------------
# (1) Kie.ai Flux Kontext image-to-image (reference = SHA-pinned raw URL)
# ----------------------------------------------------------------------------
def _kie_post(url: str, body: dict) -> dict:
    req = urllib.request.Request(
        url, data=json.dumps(body).encode(),
        headers={"Authorization": f"Bearer {KIE_API_KEY}",
                 "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)

def gen_state_png(state: str, dest: Path) -> Path:
    """Generate one keyframe via Flux Kontext i2i off the locked base URL."""
    import time
    body = {"prompt": STATE_PROMPTS[state], "inputImage": LOCKED_BASE_URL,
            "aspectRatio": "1:1", "outputFormat": "png", "model": KIE_MODEL,
            "promptUpsampling": False}
    resp = _kie_post("https://api.kie.ai/api/v1/flux/kontext/generate", body)
    task_id = resp.get("data", {}).get("taskId")
    if not task_id:
        raise RuntimeError(f"Flux createTask failed ({state}): {resp.get('msg', resp)}")
    poll = f"https://api.kie.ai/api/v1/flux/kontext/record-info?taskId={task_id}"
    for _ in range(45):
        time.sleep(2)
        req = urllib.request.Request(poll, headers={"Authorization": f"Bearer {KIE_API_KEY}"})
        with urllib.request.urlopen(req, timeout=60) as r:
            d = json.load(r).get("data", {})
        if d.get("successFlag") == 1:
            img_url = (d.get("response") or {}).get("resultImageUrl")
            if img_url:
                dl = urllib.request.Request(img_url, headers={"User-Agent": "Mozilla/5.0"})
                dest.write_bytes(urllib.request.urlopen(dl, timeout=60).read())
                return dest
        err = d.get("errorMessage")
        if err and err != "None":
            raise RuntimeError(f"Flux error ({state}): {err}")
    raise RuntimeError(f"Flux timeout waiting for '{state}' (90s)")

# ----------------------------------------------------------------------------
# (2)+(3) Crop/center into 360x360, round-mask + background key to transparent
# ----------------------------------------------------------------------------
def process_to_disc(src: Path, dest: Path) -> Image.Image:
    """Center the character in a 360x360 RGBA canvas, art within circle d=352.

    Round mask ONLY: pixels outside the visible circle become transparent; all
    in-circle pixels stay opaque. We deliberately do NOT key the near-black
    background to transparent — the matte-black armor is itself near-black and a
    luma key cannot separate body from background. On the AMOLED panel those
    near-black pixels are simply OFF (true black), so the glow-on-black mascot
    effect happens in hardware for free; only the corners need index-0 alpha,
    which the round mask supplies.
    """
    import numpy as np
    img = Image.open(src).convert("RGBA")
    # Square-pad then resize so the figure fills ~ART_DIA with margin to spare.
    w, h = img.size
    side = max(w, h)
    sq = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    sq.paste(img, ((side - w) // 2, (side - h) // 2))
    sq = sq.resize((ART_DIA, ART_DIA), Image.LANCZOS)

    canvas = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    off = (CANVAS - ART_DIA) // 2
    canvas.paste(sq, (off, off))

    arr = np.array(canvas)
    # Round mask: outside the art circle is transparent; inside stays opaque.
    yy, xx = np.mgrid[0:CANVAS, 0:CANVAS]
    c = (CANVAS - 1) / 2.0
    outside = (xx - c) ** 2 + (yy - c) ** 2 > ART_R ** 2
    arr[:, :, 3][outside] = 0
    out = Image.fromarray(arr, "RGBA")
    out.save(dest)
    return out

# ----------------------------------------------------------------------------
# (4) Quantize to <=256 indexed, palette index 0 fully transparent
# ----------------------------------------------------------------------------
def quantize_to_rows(disc: Image.Image):
    """Return (rows, palette_bgra) for emit_eaf().

    Index 0 is reserved as fully transparent. Opaque pixels are quantized with an
    ADAPTIVE palette + Floyd-Steinberg dither (255 colors) so the glow gradient
    holds. Transparent pixels (alpha < 128) map to index 0.
    """
    import numpy as np
    rgba = np.array(disc.convert("RGBA"))
    alpha = rgba[:, :, 3]
    opaque = alpha >= 128
    rgb = Image.fromarray(rgba[:, :, :3], "RGB")
    # Quantize RGB into 255 colors; we shift indices +1 so 0 stays transparent.
    pal_img = rgb.quantize(colors=255, method=Image.MEDIANCUT,
                           dither=Image.FLOYDSTEINBERG)
    idx = np.array(pal_img, dtype=np.uint16) + 1          # 1..255
    idx[~opaque] = 0                                       # transparent -> 0
    idx = idx.astype(np.uint8)

    # Build BGRA palette: entry 0 transparent; 1..255 from the PIL palette (RGB).
    pal_rgb = pal_img.getpalette()[: 255 * 3]
    palette_bgra = [(0, 0, 0, 0)]
    for i in range(255):
        r, g, b = pal_rgb[i * 3], pal_rgb[i * 3 + 1], pal_rgb[i * 3 + 2]
        palette_bgra.append((b, g, r, 255))
    rows = [bytes(idx[y].tolist()) for y in range(idx.shape[0])]
    return rows, palette_bgra

# ----------------------------------------------------------------------------
# (5) emit_eaf() -- verbatim from DISPLAY_UX_PLAN.md Phase 2A (round-trip-tested)
# ----------------------------------------------------------------------------
EAF_MAGIC_HEAD = 0x5A5A
ENC_RAW = 5

def _emit_frame_S(rows, palette_bgra, block_height=32):
    h = len(rows); w = len(rows[0])
    blocks = (h + block_height - 1) // block_height
    block_datas = []
    for b in range(blocks):
        y0, y1 = b * block_height, min(b * block_height + block_height, h)
        strip = b"".join(bytes(rows[y]) for y in range(y0, y1))
        block_datas.append(bytes([ENC_RAW]) + strip)
    block_len = [len(bd) for bd in block_datas]
    hdr = b"_S" + b"\x00" + b"v1.0\x00\x00" + bytes([8])
    hdr += struct.pack("<HHHH", w, h, blocks, block_height)
    hdr += b"".join(struct.pack("<I", bl) for bl in block_len)
    pal = b"".join(bytes(palette_bgra[i]) if i < len(palette_bgra) else b"\0\0\0\0"
                   for i in range(256))
    return hdr + pal + b"".join(block_datas)

def emit_eaf(frames_rows, palette_bgra, path, block_height=32):
    blobs = [struct.pack("<H", EAF_MAGIC_HEAD) + _emit_frame_S(fr, palette_bgra, block_height)
             for fr in frames_rows]
    blobs.append(struct.pack("<H", EAF_MAGIC_HEAD) + b"_C")
    table = bytearray(); data = bytearray()
    for blob in blobs:
        table += struct.pack("<II", len(blob), len(data)); data += blob
    tpd = bytes(table) + bytes(data)
    checksum = sum(tpd) & 0xFFFFFFFF
    out = (bytes([0x89]) + b"EAF" + struct.pack("<i", len(blobs))
           + struct.pack("<I", checksum) + struct.pack("<I", len(tpd)) + tpd)
    Path(path).write_bytes(out)
    return path

def assert_roundtrip(path):
    """Mirror eaf_init: assert magic/checksum/length + per-frame 0x5A5A + _C."""
    d = Path(path).read_bytes()
    assert d[0] == 0x89 and d[1:4] in (b"EAF", b"AAF"), "bad magic/fmt"
    total = struct.unpack("<i", d[4:8])[0]
    checksum = struct.unpack("<I", d[8:12])[0]
    length = struct.unpack("<I", d[12:16])[0]
    body = d[16:]
    assert len(body) == length, f"length {len(body)} != {length}"
    assert (sum(body) & 0xFFFFFFFF) == checksum, "checksum mismatch"
    table, frames = body[: total * 8], body[total * 8:]
    last_kind = None
    for i in range(total):
        fsz, foff = struct.unpack("<II", table[i * 8:i * 8 + 8])
        fr = frames[foff:foff + fsz]
        assert struct.unpack("<H", fr[:2])[0] == 0x5A5A, f"frame {i} head"
        last_kind = fr[2:4]
    assert last_kind == b"_C", "missing _C sentinel"
    return total

# ----------------------------------------------------------------------------
# Orchestration
# ----------------------------------------------------------------------------
def pack_state(state: str, generate: bool) -> Path:
    raw = RAW_DIR / f"{state}.png"
    if generate:
        print(f"  [{state}] generating via {KIE_MODEL} (i2i from locked base)...")
        gen_state_png(state, raw)
    elif not raw.exists():
        raise FileNotFoundError(f"{raw} missing; run without --no-gen first")
    disc = process_to_disc(raw, DISC_DIR / f"{state}.png")
    rows, pal = quantize_to_rows(disc)
    eaf = EAF_DIR / f"{state}.eaf"
    emit_eaf([rows], pal, eaf)             # single representative frame per state
    n = assert_roundtrip(eaf)
    print(f"  [{state}] -> {eaf}  ({eaf.stat().st_size} B, {n} frames, round-trip OK)")
    return eaf

def cmd_states(generate: bool):
    for d in (RAW_DIR, DISC_DIR, EAF_DIR):
        d.mkdir(parents=True, exist_ok=True)
    print(f"Locked base: {LOCKED_BASE}")
    for state in STATE_ORDER:
        pack_state(state, generate)
    print("Done. Run 'contact-sheet' to assemble the review image.")

def cmd_contact_sheet():
    """Assemble the 5 processed discs side by side on black for review."""
    pad = 16
    tiles = []
    for state in STATE_ORDER:
        p = DISC_DIR / f"{state}.png"
        if not p.exists():
            raise FileNotFoundError(f"{p} missing; run 'states' first")
        tiles.append((state, Image.open(p).convert("RGBA")))
    w = CANVAS * len(tiles) + pad * (len(tiles) + 1)
    h = CANVAS + pad * 2
    sheet = Image.new("RGBA", (w, h), (8, 8, 10, 255))   # near-black like the panel
    x = pad
    for _, im in tiles:
        sheet.alpha_composite(im, (x, pad))
        x += CANVAS + pad
    out = OUT_DIR / "contact-sheet.png"
    sheet.convert("RGB").save(out)
    print(f"Contact sheet: {out}  (states L->R: {', '.join(STATE_ORDER)})")
    return out

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["states", "contact-sheet"])
    ap.add_argument("--no-gen", action="store_true",
                    help="re-pack existing raw PNGs without calling the API")
    args = ap.parse_args()
    if args.cmd == "states":
        cmd_states(generate=not args.no_gen)
    else:
        cmd_contact_sheet()

if __name__ == "__main__":
    sys.exit(main())
