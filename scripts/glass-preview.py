#!/usr/bin/env python3
"""Play the production glass compositor through time and write it as an animation.

    scripts/glass-preview.py --script ring --out ring.gif
    scripts/glass-preview.py --script shade --slow 4 --contact-sheet 6

Builds components/jr_display/tests/probe/glass_preview.c in the jarvisnano-hosttests
Docker image, runs a script (components/jr_display/tests/probe/preview/NAME.script, or
a path), and assembles the frames at true speed with the round panel masked to a
circle on a dark surround.

With --knob 1 (the panel dip, perf_knob.h) the frames are dimmed the way the panel would be
and the report lists every brightness write the dip made: frame, ms, dip and the value sent.

Outputs next to --out: NAME.gif always (256 colours, so the glows band: it is the
fallback), NAME.png as an APNG (24-bit, no banding, the file to judge from; it is
the default for --format), NAME.webp when Pillow has WebP (lossless). A contact
sheet is NAME_sheet.png with --contact-sheet K (every K-th frame).

What is stand-in: see docs/reference/glass-preview.md. The note the C side prints is
repeated at the end of every run.
"""
import argparse
import hashlib
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
PROBE = ROOT / "components" / "jr_display" / "tests" / "probe"
PREVIEW_DIR = PROBE / "preview"
REACTIVE = ROOT / "firmware" / "mascot" / "reactive"
W = 466
DIALS = ["diver", "dress", "pilot", "future", "skeleton", "celestial"]
SURROUND = (14, 14, 16)
PAD = 22


def rgb565(img):
    import numpy as np
    a = np.asarray(img.convert("RGB"), dtype=np.uint16)
    v = ((a[..., 0] >> 3) << 11) | ((a[..., 1] >> 2) << 5) | (a[..., 2] >> 3)
    return v.astype("<u2").tobytes()


def prepare_backgrounds(work):
    """face.rgb565 (the idle face's frames) and dial_*.rgb565, cut from the repo's art."""
    bg = work / "bg"
    bg.mkdir(parents=True, exist_ok=True)
    face = bg / "face.rgb565"
    if not face.exists():
        sheet = Image.open(REACTIVE / "preview" / "rwave_idle.png").convert("RGB")
        # 8 columns, pitch 474, the 466 px frame starts 8 px in (measured from the
        # all-black gutters in the sheet); 30 frames fill 3 rows and a part of the 4th.
        frames = []
        for i in range(30):
            c, r = i % 8, i // 8
            x0, y0 = 8 + 474 * c, 8 + 474 * r
            frames.append(rgb565(sheet.crop((x0, y0, x0 + W, y0 + W))))
        face.write_bytes(b"".join(frames))
    for name in DIALS:
        out = bg / f"dial_{name}.rgb565"
        src = REACTIVE / f"dial_{name}_preview.png"
        if not out.exists() and src.exists():
            im = Image.open(src).convert("RGBA")
            base = Image.new("RGBA", im.size, (0, 0, 0, 255))
            out.write_bytes(rgb565(Image.alpha_composite(base, im)))
    return bg


def docker(work, args, **kw):
    cmd = ["docker", "run", "--rm", "-v", f"{ROOT}:/project", "-v", f"{work}:/work",
           "-w", "/project", "jarvisnano-hosttests"] + args
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    return subprocess.run(cmd, env=env, capture_output=True, text=True, **kw)


def build(work):
    r = docker(work, ["sh", "-c",
                      "cmake -S components/jr_display/tests -B /work/build >/dev/null && "
                      "cmake --build /work/build --target glass_preview 2>&1 | tail -30"])
    if r.returncode != 0 or "Built target glass_preview" not in r.stdout:
        sys.exit("build failed:\n" + r.stdout + r.stderr)


def run_script(work, script, fps, tag, knob=0):
    shutil_copy = work / f"{tag}.script"
    shutil_copy.write_text(Path(script).read_text())
    raw = work / f"{tag}.rgb"
    r = docker(work, ["/work/build/glass_preview", f"/work/{tag}.script", f"out=/work/{tag}.rgb",
                      "bg=/work/bg", f"fps={fps}", f"knob={knob}"])
    if r.returncode != 0:
        sys.exit(f"glass_preview failed ({r.returncode}):\n{r.stdout[-2000:]}{r.stderr[-2000:]}")
    frames, marks, notes, dips, holds = [], [], [], [], []
    for line in r.stdout.splitlines():
        p = line.split()
        if line.startswith("F "):
            frames.append((int(p[1]), int(p[2]), p[3]))
        elif line.startswith("DIP "):
            dips.append((int(p[1]), int(p[2]), int(p[3]), int(p[4])))
        elif line.startswith("HOLD "):
            holds.append((int(p[1]), int(p[2])))
        elif line.startswith("MARK "):
            marks.append((int(p[1]), int(p[2])))
        elif line.startswith("NOTE ") or line.startswith("DONE"):
            notes.append(line)
    if r.stderr.strip():
        notes += ["STDERR " + x for x in r.stderr.strip().splitlines()]
    return raw, frames, marks, notes, dips + [(fr, -1, -1, ms) for fr, ms in holds]


def circle_mask(size, scale):
    ss = 4
    m = Image.new("L", (size * scale * ss, size * scale * ss), 0)
    ImageDraw.Draw(m).ellipse((0, 0, size * scale * ss - 1, size * scale * ss - 1), fill=255)
    return m.resize((size * scale, size * scale), Image.LANCZOS)


def device_frame(raw_bytes, scale, mask):
    im = Image.frombytes("RGB", (W, W), raw_bytes)
    if scale != 1:
        im = im.resize((W * scale, W * scale), Image.NEAREST)
    side = (W + 2 * PAD) * scale
    canvas = Image.new("RGB", (side, side), SURROUND)
    d = ImageDraw.Draw(canvas)
    # a thin bezel lip so the circle reads as glass
    d.ellipse((PAD * scale - 3, PAD * scale - 3, side - PAD * scale + 2, side - PAD * scale + 2),
              outline=(40, 40, 44), width=2)
    canvas.paste(im, (PAD * scale, PAD * scale), mask)
    return canvas


def read_frames(raw, n):
    with open(raw, "rb") as f:
        for _ in range(n):
            yield f.read(W * W * 3)


def durations_ms(n, fps, slow):
    """GIF stores 10 ms units: carry the rounding so the clip's length is exact."""
    out, acc, want = [], 0, 0.0
    for _ in range(n):
        want += 1000.0 * slow / fps
        d = int(round(want / 10.0)) * 10 - acc
        d = max(d, 20)
        out.append(d)
        acc += d
    return out


def save_gif(frames, path, durs):
    import numpy as np
    sample = frames[:: max(1, len(frames) // 24)]
    tile = Image.new("RGB", (sample[0].width, sample[0].height * len(sample)))
    for i, f in enumerate(sample):
        tile.paste(f, (0, i * f.height))
    pal = tile.quantize(colors=255, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    q = [f.quantize(palette=pal, dither=Image.Dither.NONE) for f in frames]
    q[0].save(path, save_all=True, append_images=q[1:], duration=durs, loop=0, disposal=1)
    del np


def save_apng(frames, path, durs):
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=durs, loop=0, format="PNG")


def save_webp(frames, path, durs):
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=durs, loop=0,
                   lossless=True, format="WEBP")


def contact_sheet(raw, frames_meta, marks, script_lines, k, path, scale, cols=6):
    ids = list(range(0, len(frames_meta), k))
    tw = 233 if scale == 1 else 466
    rows = (len(ids) + cols - 1) // cols
    lab = 34
    sheet = Image.new("RGB", (cols * tw, rows * (tw + lab)), (10, 10, 10))
    d = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("arial.ttf", 15)
    except OSError:
        font = ImageFont.load_default()
    # which script step fired just before each frame
    ev = {}
    for fr, ln in marks:
        txt = script_lines.get(ln)
        if txt and not txt.startswith("wait"):
            ev.setdefault(fr, []).append(txt)
    data = list(read_frames(raw, len(frames_meta)))
    for n, i in enumerate(ids):
        im = Image.frombytes("RGB", (W, W), data[i]).resize((tw, tw), Image.LANCZOS if tw < W else Image.NEAREST)
        x, y = (n % cols) * tw, (n // cols) * (tw + lab)
        sheet.paste(im, (x, y + lab))
        tags = []
        for fr in range(i - k + 1, i + 1):
            tags += ev.get(fr, [])
        d.text((x + 6, y + 4), f"f{i} {frames_meta[i][1] / 1000:.2f}s", fill=(220, 220, 220), font=font)
        if tags:
            d.text((x + 6, y + 18), "; ".join(tags)[:40], fill=(255, 190, 60), font=font)
    sheet.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--script", required=True, help="a name in probe/preview/ or a path")
    ap.add_argument("--out", help="output path; the extension is replaced per format (default: temp dir)")
    ap.add_argument("--scale", type=int, choices=(1, 2), default=1)
    ap.add_argument("--slow", type=int, default=1, help="play at 1/N speed")
    ap.add_argument("--knob", type=lambda v: int(v, 0), default=0,
                    help="the perf knob word (perf_knob.h) to render with; 0 is the shipped path")
    ap.add_argument("--fps", type=int, default=24, help="render cadence 2..24 (the mood ladder: 24, 12, 6, 3)")
    ap.add_argument("--format", choices=("apng", "gif", "webp"), default="apng",
                    help="the format reported as the main file (all available ones are written)")
    ap.add_argument("--contact-sheet", type=int, nargs="?", const=6, metavar="K",
                    help="also write NAME_sheet.png with every K-th frame (default 6)")
    ap.add_argument("--sheet-cols", type=int, default=6, help="columns of the contact sheet (default 6)")
    ap.add_argument("--check", action="store_true", help="run twice and require identical frame hashes")
    ap.add_argument("--work", help="work dir for the build and raw frames (default: temp)")
    a = ap.parse_args()

    script = Path(a.script)
    if not script.exists():
        script = PREVIEW_DIR / f"{a.script}.script"
    if not script.exists():
        sys.exit(f"no script {a.script} (looked in {PREVIEW_DIR})")
    name = script.stem
    work = Path(a.work) if a.work else Path(tempfile.gettempdir()) / "glass-preview-work"
    work.mkdir(parents=True, exist_ok=True)
    out = Path(a.out) if a.out else Path(tempfile.gettempdir()) / "glass-preview-out" / f"{name}.gif"
    out.parent.mkdir(parents=True, exist_ok=True)

    prepare_backgrounds(work)
    build(work)
    raw, frames, marks, notes, dips = run_script(work, script, a.fps, name, a.knob)
    if a.check:
        _, frames2, _, _, _ = run_script(work, script, a.fps, name + "_b", a.knob)
        if [h for _, _, h in frames] != [h for _, _, h in frames2]:
            sys.exit("NOT DETERMINISTIC: frame hashes differ between two runs")
        print(f"deterministic: {len(frames)} frames, identical hashes twice")
    n = len(frames)
    mask = circle_mask(W, a.scale)
    imgs = [device_frame(b, a.scale, mask) for b in read_frames(raw, n)]
    durs = durations_ms(n, a.fps, a.slow)
    written = {}
    stem = out.with_suffix("")
    save_gif(imgs, stem.with_suffix(".gif"), durs)
    written["gif"] = stem.with_suffix(".gif")
    save_apng(imgs, stem.with_suffix(".png"), durs)
    written["apng"] = stem.with_suffix(".png")
    try:
        save_webp(imgs, stem.with_suffix(".webp"), durs)
        written["webp"] = stem.with_suffix(".webp")
    except Exception as e:  # Pillow built without WebP
        print("webp skipped:", e)
    if a.contact_sheet:
        lines = {}
        for i, ln in enumerate(script.read_text().splitlines(), 1):
            lines[i] = ln.split("#")[0].strip()
        sp = Path(str(stem) + "_sheet.png")
        contact_sheet(raw, frames, marks, lines, a.contact_sheet, sp, a.scale, a.sheet_cols)
        written["sheet"] = sp
    print(f"{name}: {n} frames at {a.fps} fps = {n / a.fps:.2f} s of device time, played x1/{a.slow}")
    for kind, p in written.items():
        print(f"  {kind}: {p}")
    print(f"  main ({a.format}): {written.get(a.format)}")
    for line in notes:
        print(" ", line)
    if dips:
        # the brightness steps of the panel dip: the frame they fell in, the clock inside it, the dip
        # (0..256) and the 0x51 value the driver was sent (the mood is 100)
        print(f"  panel dip: {len([d for d in dips if d[1] >= 0])} brightness writes (frame, ms, dip, sent)")
        rows = [d for d in dips if d[1] >= 0]
        for fr, ms, dip, sent in rows[:80]:
            print(f"    {fr:4d} {ms:6d} {dip:4d} {sent:4d}")
        if len(rows) > 80:
            print(f"    ... {len(rows) - 80} more")
        for fr, _, _, hold in [d for d in dips if d[1] < 0]:
            print(f"  dark hold ended in frame {fr}: dip_hold_ms {hold}")
    h = hashlib.sha256("".join(h for _, _, h in frames).encode()).hexdigest()[:16]
    print(f"  frame-hash digest {h}")


if __name__ == "__main__":
    main()
