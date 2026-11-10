#!/usr/bin/env python3
"""Rasterise the glass icon set into components/jr_display/src/glass_icon_data.c.

The source art is third_party/icons/glass-icons-sheet.png: twelve white-on-black
line icons in a 4 x 3 grid, generated in-house (see THIRD_PARTY_NOTICES.md).
Row by row: sun, moon, sun-behind-cloud, cloud, rain, thunder, snow, fog,
microphone, loudspeaker, bolt, wifi. A thirteenth icon, the muted microphone,
is the microphone with a slash this script draws (a stroke the width of the
sheet's own line, with a gap cut either side so it reads against the body).

The model that drew the sheet never keeps its grid exact, so the cells are
found, not assumed: ink is thresholded, the rows are the runs of ink rows
separated by a clear gap, the icons in a row are the runs of ink columns
separated by a clear gap, and each icon is cropped to its own ink box.

Every icon is scaled by ONE factor, chosen so the widest or tallest of them just
fills the box (a pixel of margin each side); that keeps the stroke weight the
same across the set. It is centred in a square, drawn at 8x and box-filtered
down (true area coverage), and packed as 4-bit alpha, two pixels per byte, high
nibble first, row-major, S*S pixels per icon, like the glass glyphs.

    python scripts/gen-glass-icons.py           regenerate the committed file
    python scripts/gen-glass-icons.py --check   regenerate in memory; fail if
                                                the committed file differs
    python scripts/gen-glass-icons.py --preview PNG   write a contact sheet

Needs Pillow. The check normalises line endings (the checkout is CRLF on the
desk).
"""
import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
SHEET = ROOT / "third_party" / "icons" / "glass-icons-sheet.png"
OUT = ROOT / "components" / "jr_display" / "src" / "glass_icon_data.c"

NAMES = ["SUN", "MOON", "PARTLY", "CLOUD", "RAIN", "THUNDER", "SNOW", "FOG",
         "MIC", "SPEAKER", "BOLT", "WIFI"]
EXTRA = ["MIC_MUTED"]
SIZES = [("SM", 28), ("LG", 44)]
SS = 8                  # supersampling factor
ALPHA_GAMMA = 0.85      # <1 thickens light-on-dark strokes a touch, as the font does
INK = 64                # luminance that counts as ink when looking for cells
ROW_GAP = 50            # clear source pixels that separate two rows / two icons
COL_GAP = 50
LINE_PX_SRC = 12.0      # the sheet's stroke weight at source scale (measured)


def runs(flags, gap):
    """Index ranges [a, b) of True flags, merged when less than `gap` apart."""
    out = []
    i, n = 0, len(flags)
    while i < n:
        if not flags[i]:
            i += 1
            continue
        j = i
        while j < n and flags[j]:
            j += 1
        if out and i - out[-1][1] < gap:
            out[-1][1] = j
        else:
            out.append([i, j])
        i = j
    return out


def slice_sheet(img):
    lum = img.convert("L")
    w, h = lum.size
    px = lum.load()
    rows_ink = [any(px[x, y] > INK for x in range(w)) for y in range(h)]
    bands = runs(rows_ink, ROW_GAP)
    if len(bands) != 3:
        sys.exit(f"sheet: expected 3 icon rows, found {len(bands)}")
    icons = []
    for y0, y1 in bands:
        cols_ink = [any(px[x, y] > INK for y in range(y0, y1)) for x in range(w)]
        cells = runs(cols_ink, COL_GAP)
        if len(cells) != 4:
            sys.exit(f"sheet: expected 4 icons in the row at y{y0}, found {len(cells)}")
        for x0, x1 in cells:
            crop = lum.crop((x0, y0, x1, y1))
            box = crop.point(lambda v: 255 if v > INK else 0).getbbox()
            icons.append(crop.crop(box))
    return icons


def render(icons, S):
    """Return {name: S*S list of 0..15} at box size S."""
    longest = max(max(i.size) for i in icons)
    scale = (S - 2) / longest
    stroke = LINE_PX_SRC * scale
    out = {}
    for name, src in zip(NAMES, icons):
        big = Image.new("L", (S * SS, S * SS), 0)
        tw, th = round(src.width * scale * SS), round(src.height * scale * SS)
        art = src.resize((tw, th), Image.LANCZOS)
        big.paste(art, ((S * SS - tw) // 2, (S * SS - th) // 2))
        out[name] = big
    # The muted microphone: the mic, a gap, then the slash.
    mic = out["MIC"].copy()
    d = ImageDraw.Draw(mic)
    a = (S * SS * 0.16, S * SS * 0.10)
    b = (S * SS * 0.84, S * SS * 0.90)
    gap_w = round((stroke * 2.6) * SS)
    line_w = round((stroke * 1.15) * SS)
    for width, fill in ((gap_w, 0), (line_w, 255)):
        d.line([a, b], fill=fill, width=width)
        r = width / 2
        for p in (a, b):
            d.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=fill)
    out["MIC_MUTED"] = mic
    packed = {}
    for name, big in out.items():
        small = big.resize((S, S), Image.BOX)
        packed[name] = [round(255 * (v / 255.0) ** ALPHA_GAMMA / 17) for v in small.tobytes()]
    return packed


def pack(px):
    if len(px) % 2:
        px = px + [0]
    return bytes((px[i] << 4) | px[i + 1] for i in range(0, len(px), 2))


def emit(sets):
    names = NAMES + EXTRA
    o = ["/* GENERATED by scripts/gen-glass-icons.py - do not edit.\n"
         " * Source: third_party/icons/glass-icons-sheet.png, generated in-house art\n"
         " * (THIRD_PARTY_NOTICES.md); the muted microphone's slash is drawn by the script.\n"
         " * 4-bit alpha, two pixels per byte (high nibble first), row-major, S*S\n"
         " * pixels per icon, in the order of glass_icon_id_t. */\n",
         '#include "jr_display/glass_icon.h"\n']
    for (tag, S), packed in zip(SIZES, sets):
        o.append(f"/* {tag}: {S} x {S} px, {len(names)} icons, {S * S // 2} bytes each */")
        o.append(f"const uint8_t glass_icon_{tag.lower()}[GLASS_ICON_COUNT][GLASS_ICON_{tag}_BYTES] = {{")
        for n in names:
            b = pack(packed[n])
            o.append(f"    [GLASS_ICON_{n}] = {{")
            for i in range(0, len(b), 16):
                o.append("        " + ",".join(f"0x{v:02x}" for v in b[i:i + 16]) + ",")
            o.append("    },")
        o.append("};\n")
    return "\n".join(o)


def preview(sets, path):
    names = NAMES + EXTRA
    cols = 5
    pad = 8
    sheet = Image.new("RGB", (cols * (44 + 28 + pad * 3), 3 * (44 + pad)), (0, 0, 0))
    for k, n in enumerate(names):
        for t, ((tag, S), packed) in enumerate(zip(SIZES, sets)):
            im = Image.new("L", (S, S))
            im.putdata([v * 17 for v in packed[n]])
            x = (k % cols) * (44 + 28 + pad * 3) + (0 if t == 0 else 28 + pad)
            y = (k // cols) * (44 + pad)
            sheet.paste(Image.merge("RGB", (im, im, im)), (x, y))
    sheet = sheet.resize((sheet.width * 4, sheet.height * 4), Image.NEAREST)
    sheet.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--preview", metavar="PNG")
    args = ap.parse_args()
    icons = slice_sheet(Image.open(SHEET))
    sets = [render(icons, S) for _, S in SIZES]
    if args.preview:
        preview(sets, args.preview)
        print(f"wrote {args.preview}")
    text = emit(sets)
    if args.check:
        cur = OUT.read_text(encoding="utf-8").replace("\r\n", "\n") if OUT.exists() else ""
        if cur != text:
            print(f"FAIL: {OUT.relative_to(ROOT)} differs from a fresh render", file=sys.stderr)
            return 1
        print("check ok: committed glass_icon_data.c matches a fresh render")
        return 0
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUT.relative_to(ROOT)}  ({len(NAMES) + len(EXTRA)} icons, "
          f"{sum(S * S // 2 for _, S in SIZES) * (len(NAMES) + len(EXTRA))} B)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
