# AMOLED mascot face — disc `.eaf` generation

Produces the **center-disc mascot face** animation assets for the Waveshare
AMOLED-1.75 board, per `docs/DISPLAY_UX_PLAN.md` §Phase 2A (RAW `.eaf` format +
`emit_eaf()` encoder) and §Phase 2B (composite layout: mascot is the 360×360
center disc, art within ⌀352, corners transparent).

Character is LOCKED: `images/mascot-amoled-concept-locked.png` — chibi JARVIS
helmet bust, dark visor (no face), headphone ear-cups, glowing "J", arc-reactor
orange rim-glow (`#F5870B`) + hot yellow-white core (`#FFE25E`), near-black bg.
Because the mascot has a **visor and no face**, each voice state is conveyed by
**glow intensity/color + pose/lean**, not facial expression.

Emote names are a HARD CONTRACT with the shipped `emote_set_*` helpers:
`neutral` (idle), `listen`, `thinking`, `happy` (speaking), `offline`.
display-eng confirmed from source that `emote_set_anim_emoji` resolves by the
`emote.json` **NAME** key (hash-looked-up), not the `.eaf` filename — so the
filenames are literal `neutral.eaf` … `offline.eaf` with a clean 1:1 name=src map.

## Pipeline (`gen_mascot_eaf.py`)

1. **Image-to-image** off the locked base (keeps every frame on-model). Done via
   Kie.ai Flux Kontext (`flux-kontext-pro`; `KIE_MODEL=flux-kontext-max` for a
   flatter look). Flux needs the reference as a URL, so the locked base is pinned
   to its immutable `raw.githubusercontent` SHA (commit `ca20416`).
2. PIL crop/center into 360×360, art within ⌀352 (4 px transparent margin).
3. **Round mask ONLY**: pixels outside the ⌀352 circle become transparent; all
   in-circle pixels stay opaque. We deliberately do NOT key the near-black
   background out — the matte-black armor is itself near-black and a luma key
   can't separate body from background. On the AMOLED true-black panel those
   near-black pixels are simply OFF, so the glow-on-black mascot happens in
   hardware; only the corners need index-0 alpha, which the round mask supplies.
4. Quantize to ≤256-color indexed, **palette index 0 = fully transparent**,
   Floyd-Steinberg dither so the glow gradient holds.
5. `emit_eaf()` — **verbatim** from DISPLAY_UX_PLAN §2A, round-trip-tested — then
   re-parse to assert magic / checksum / `_S` header / trailing `_C` sentinel.
   The encoder is byte-verified against the shipped decoder `gfx_eaf_dec.c`
   (bit_depth 8, header offsets, cumulative `block_len` strip offsets, 256×4 BGRA
   palette, `EAF_ENCODING_RAW=5`).

```
python3 gen_mascot_eaf.py states          # 1 frame per state → eaf/<state>.eaf
python3 gen_mascot_eaf.py states --no-gen # re-pack existing raw/ PNGs (no API)
python3 gen_mascot_eaf.py contact-sheet   # assemble the 5-state review sheet
```

Setup: `python3 -m venv .venv && .venv/bin/pip install Pillow numpy`.

## Delivery shape (tracked under `firmware/mascot/`)

| file | role |
|---|---|
| `eaf/{neutral,listen,thinking,happy,offline}.eaf` | the disc pack (the assets) |
| `config.json` | `{"emoji_collection": "emoji_hud_466"}` |
| `emote.json` | 5-entry 1:1 name=src map, fps 8 (gentle disc per §2B) |
| `gen_mascot_eaf.py`, `README.md` | reproducible pipeline + docs |
| `raw/`, `disc/`, `contact-sheet.png` | generations / previews / review sheet |

These are **source assets**, not source patches. `bootstrap.sh` installs them into
the esp-claw clone — `eaf/*.eaf` → `assets_local/emoji_hud_466/`, the two JSON →
`assets_local/466_466/` — the same way it copies `firmware/lua` and
`firmware/router_rules`. That wiring + the CMake `284_240`→`466_466` switch + the
`layout.json` disc/annulus split are **firmware-eng's tasks**; until they land the
active pack stays 284 and these files sit build-ready. The annulus `.eaf` (Codex's)
is added to the same `emote.json` later.

> Does NOT flash or build — the orchestrator owns the single board + integrated
> build. This script only writes PNGs + `.eaf` files under `firmware/mascot/`.

## Status

These are **single representative keyframes** per state (staged for review). Once
the per-state look is approved, the multi-frame animations follow (§2B: neutral
blink loop, happy lipsync, thinking considering-cycle, listen attentive) — same
pipeline, `emit_eaf([frame0, frame1, …], …)`.
