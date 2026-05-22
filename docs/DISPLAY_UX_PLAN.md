# Display & UX Plan — Waveshare AMOLED-1.75 (466×466)

> Make the round AMOLED feel **alive** (voice-state faces + motion) and
> **customizable** (theming, info, touch menus). Every item below names the
> **exact** emote/gfx API it uses. Nothing here is invented — APIs were read
> from the headers cited in the Inventory section.

Status: voice-state UI **partially shipped and at risk** (see §0). This plan
captures it into a patch, fills the gaps (Thinking/Connecting), and lays out
the native-art / theming / menu phases.

---

## 0. Inventory (grounded API surface)

### 0.1 Our emote wrapper — `esp-claw/components/common/emote/`

`include/emote.h` (the contract other components call):

| Symbol | Purpose | Status |
|---|---|---|
| `emote_start()` | init engine, mount `emote` partition, draw idle | shipped |
| `emote_set_network_status(bool sta, const char *ap_ssid)` | swim/offline + Wi-Fi msg | shipped |
| `emote_set_status_detail(const char *detail)` | idle line `"Ready * <model>"` (patch 0009) | shipped |
| `emote_set_listening()` | voice Listening face+label | **uncaptured** (§0.4) |
| `emote_set_speaking()` | voice Speaking face+label | **uncaptured** |
| `emote_set_voice_idle()` | back to idle status | **uncaptured** |
| `emote_set_thinking()` | voice Thinking face+label | **MISSING — this plan adds** |
| `emote_set_connecting()` | voice Connecting face+label | **MISSING — this plan adds** |

`emote.c` internals we build on:
- `emote_apply(idle, msg)` — sets the eye animation (`emote_set_anim_emoji`) +
  the `EMOTE_MGR_EVT_SYS` message, then `emote_notify_all_refresh` if we own the
  display arbiter. **All voice helpers funnel through this.**
- Cached state: `s_sta_connected`, `s_ap_ssid[48]`, `s_status_detail[48]` +
  `emote_render_status()` rebuild the idle line. Voice-idle just re-renders this.
- Display ownership is gated by `display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_EMOTE)`
  — anything we draw must respect the arbiter or it gets dropped.

### 0.2 Emote-manager API — `managed_components/espressif2022__esp_emote_expression/include/expression_emote/emote_api.h`

Richer than what `emote.c` currently uses. Confirmed symbols:

- **Event/message:** `emote_set_event_msg(handle, event, message)` where event ∈
  `EMOTE_MGR_EVT_IDLE/SPEAK/LISTEN/SYS/SET/BAT/OFF`. We currently only use `SYS`.
- **Eye animation:** `emote_set_anim_emoji(handle, name)` — `name` indexes
  `emote.json` (e.g. `"listen"`, `"thinking"`, `"happy"`, `"neutral"`).
- **Timed overlay:** `emote_insert_anim_dialog(handle, name, duration_ms)` /
  `emote_stop_anim_dialog` / `emote_set_dialog_anim` — for transient pop-ups
  (e.g. a "tap to talk" hint that auto-dismisses). Uses the `emerg_dlg` element.
- **Generic objects:** `emote_create_obj_by_type(handle, type, name)` where type ∈
  `"anim"/"image"/"label"/"qrcode"/"timer"`, plus `emote_get_obj_by_name`,
  `emote_set_obj_visible(handle, name, bool)`, `emote_set_anim_visible`. This is
  how we add **battery / Wi-Fi / clock** elements without re-architecting.
- **Predefined element names:** `EMT_DEF_ELEM_DEFAULT_LABEL`, `..._TOAST_LABEL`,
  `..._STATUS_ICON`, `..._CHARGE_ICON`, `..._BAT_LEFT_LABEL`, `..._CLOCK_LABEL`,
  `..._LISTEN_ANIM`, `..._QRCODE`. Several already exist in our `layout.json`.
- **Thread safety:** `emote_lock` / `emote_unlock` around multi-object updates.

### 0.3 gfx primitives — `managed_components/espressif2022__esp_emote_gfx/include/`

- **Label** (`widget/gfx_label.h`): `gfx_label_set_text(_fmt)`, `set_color`,
  `set_bg_color`, `set_bg_enable`, `set_opa`, `set_font`, `set_text_align`,
  `set_long_mode` (scroll), `set_scroll_speed`. Colors are `gfx_color_t`.
- **Anim** (`widget/gfx_anim.h`): `gfx_anim_set_src`, `gfx_anim_set_segment`
  (start/end/fps/repeat), `gfx_anim_start/stop`, `gfx_anim_set_mirror`.
- **Object/layout** (`core/gfx_obj.h`): `gfx_obj_set_pos`, `gfx_obj_set_size`,
  `gfx_obj_align(obj, GFX_ALIGN_*, x_ofs, y_ofs)`, `gfx_obj_set_visible`,
  `gfx_obj_update_layout`. `GFX_ALIGN_CENTER`, `..._TOP_MID`, `..._BOTTOM_MID`, etc.
- **Touch** (`core/gfx_touch.h`): `gfx_touch_add(handle, cfg)` → `gfx_touch_t*`;
  `cfg.event_cb` is `gfx_touch_event_cb_t(touch, event, user_data)`;
  `event.type` ∈ press/release/move (see `gfx_touch_event_type_t`); `event.hit_obj`
  set when bound to a disp; `gfx_touch_set_disp`, `gfx_touch_del`.

The canvas is **466×466** (set from board `lcd_width/lcd_height` in
`emote_get_default_config`). Today's layout.json positions assume **240-wide**
labels — those need re-centering for 466 (see §3).

### 0.4 Asset pipeline — how `emote_assets.bin` is packed

CMake (`components/common/emote/CMakeLists.txt`) calls:
```
build_speaker_assets_bin("emote" "284_240" emote_assets.bin <name_len> <assets_local>)
```
which runs `esp_emote_assets/scripts/spiffs_assets/build_all.py`, which:
1. reads `assets_local/284_240/config.json` → `emoji_collection: "emoji_large"`,
2. reads `assets_local/284_240/{emote.json, layout.json}` (the resolution dir),
3. resolves the eaf collection dir via `find_path_in_bases("emoji_large", external=assets_local …)`
   — **external is searched first**, so it resolves to
   `assets_local/emoji_large/` (which today holds **only `swim.eaf` + `offline.eaf`**),
4. for each `emote.json` entry, copies `<collection>/<src>` into the pack; if the
   eaf is missing it **logs `Warning: EAF file not found` and marks `"lack": true`**
   (build still succeeds; that emote just renders nothing / its fallback),
5. `spiffs_assets_gen.py` packs everything into `emote_assets.bin` → flashed to the
   `emote` partition at `0x420000` (6 MB after patch 0030; was 3 MB @ 0x820000 and
   the voice pack overflowed it — see Phase 2C).

**Current `assets_local/284_240/emote.json` already maps the voice emotes:**
```
swim/offline (have art) + listen→listen.eaf, thinking→confused.eaf,
happy→Happy.eaf, neutral→neutral.eaf, winking, surprised→shocked.eaf,
sad→Sad.eaf, angry→angry.eaf
```
…but **only swim.eaf + offline.eaf exist in `assets_local/emoji_large/`.** The
other eaf files DO exist upstream in
`managed_components/espressif2022__esp_emote_assets/emoji_large/`
(listen 100 KB, confused 211 KB, Happy 81 KB, neutral 161 KB, …). **This is the
real blocker for a *visible* voice-state UI** — the C code is fine, the art is
absent from the local collection.

There is **no local `.eaf` encoder.** `esp_emote_gfx/scripts/image_converter.py`
only does PNG→RGB565 `.bin` (static icons). The animated `.eaf` ("emote asset
format", huffman/heatshrink frame sequences — cf. test assets
`mi_1_eye_8bit_huff.eaf`) is produced by Espressif's separate emote toolchain,
not anything in this tree. **Implication for §2 (466 art): native frames need
that external encoder or a new one; this plan scopes the path, not the art.**

### 0.5 State → emote wiring (today)

Router rules (`firmware/router_rules/router_rules.json`) only route **IM
messages** — they do **not** drive emote state. Voice state is driven directly:
`cap_gemini_live.c` (owned by voice-eng) has a state machine
`GL_STATE_IDLE/CONNECTING/READY/LISTENING/THINKING/SPEAKING` and calls
`emote_set_listening/speaking/voice_idle()` at transitions, plus
`emote_set_status_detail(detail)` centrally in `gl_set_state()`.

**Gap:** `GL_STATE_THINKING` and `GL_STATE_CONNECTING` exist but set **no face**
— the eye stays on the previous emote. §1 closes this.

---

## Phase 0 (DONE) — Capture the uncaptured edits → patch 0012

`esp-claw/` is a gitignored generated tree. On a **clean** bootstrap,
`clone_or_update_esp_claw()` clones upstream emote.c (no voice helpers), then
`apply_emote_status_detail_patch` (patch 0009) regenerates only the
`status_detail` version. The voice helpers (`emote_set_listening/speaking/
voice_idle`), the `idle = "neutral"` change, and the voice entries in
`emote.json` exist **only in this local checkout** — they are **not in any
patch** and will vanish, breaking the `cap_gemini_live.c` link
(`undefined reference to emote_set_listening`).

**Done:** `patches/0012-emote-voice-states.patch` + `bootstrap.sh`
`apply_emote_voice_states_patch()` (runs after 0009; numbered 0012 to avoid
collision with touch-eng's 0010 and voice-eng's 0011). It:
1. appends the voice helpers (`listening/speaking/thinking/voice_idle/connecting`)
   to `emote.c` (idempotent: guard on `grep -q emote_set_listening`),
2. appends their declarations to `emote.h`,
3. rewrites `assets_local/284_240/emote.json` to include the voice emotes,
4. copies the missing eaf art from the upstream `emoji_large/` collection into
   `assets_local/emoji_large/` so the faces actually render (idempotent: skip if
   `listen.eaf` already present).

This is the gate. Everything else builds on a tree that survives rebuild.

---

## Phase 1 — Voice-state UI (highest leverage) — IMPLEMENT NOW

**Mapping (emote name → emote.json → eaf art):**

| Voice state | `emote_set_*` | eye anim | message label | art source |
|---|---|---|---|---|
| Connecting | `emote_set_connecting()` | `"thinking"` (confused) | `"Connecting…"` | confused.eaf |
| Listening | `emote_set_listening()` | `"listen"` | `"Listening…"` | listen.eaf |
| Thinking | `emote_set_thinking()` | `"thinking"` (confused) | `"Thinking…"` | confused.eaf |
| Speaking | `emote_set_speaking()` | `"happy"` | `"Speaking…"` | Happy.eaf |
| Idle | `emote_set_voice_idle()` | `emote_render_status()` | `"Ready * <model>"` | swim/neutral |

**Exact calls:** each helper is one line — `return emote_apply("<eye>", "<msg>");`
— reusing the existing `emote_apply()` (which does `emote_set_anim_emoji` +
`emote_set_event_msg(…, EMOTE_MGR_EVT_SYS, msg)` + arbiter-gated refresh).
`emote_set_voice_idle()` calls `emote_render_status()` so it restores the
`"Ready * <model>"` line. **Future polish:** switch the message event from
`EMOTE_MGR_EVT_SYS` to the semantic `EMOTE_MGR_EVT_LISTEN` / `EMOTE_MGR_EVT_SPEAK`
so the manager can theme listen/speak distinctly — deferred (low risk, needs the
manager's listen-anim element wired in layout.json).

**Contract with voice-eng (task #2) — LANDED.** I own the `emote.h` symbols (all
5 defined in `emote.c`, captured in patch 0012 → integrated build links clean).
voice-eng centralized every face call into `gl_set_state()`'s switch (removed the
4 scattered calls), so there is exactly one call site per face:
```c
switch (st) {
  case GL_STATE_CONNECTING: emote_set_connecting(); break;
  case GL_STATE_LISTENING:  emote_set_listening();  break;
  case GL_STATE_THINKING:   emote_set_thinking();   break;
  case GL_STATE_SPEAKING:   emote_set_speaking();   break;
  case GL_STATE_IDLE:       emote_set_voice_idle(); break;
  case GL_STATE_READY:      /* no face — transient post-setup */ break;
  default: break;
}
```
Every transition now repaints the face; Connecting and Thinking are no longer
faceless.

**⚠️ Validation caveat — Thinking face won't show on the first round-trip.** The
model path is connecting→listening→speaking; voice-eng has NOT wired any
transition INTO `GL_STATE_THINKING` yet, so `emote_set_thinking()` is defined and
hooked but never fires today. This is expected, NOT a bug — the face is ready the
moment a thinking phase is added. Don't flag a missing Thinking face during board
validation.

**Tap-to-talk affordance:** when idle and a session is available, show a transient
hint via `emote_insert_anim_dialog(handle, "winking", 1500)` or a toast label
(`EMT_DEF_ELEM_TOAST_LABEL` → `gfx_label_set_text`, made visible with
`emote_set_obj_visible`). The actual *tap* is owned by the touch contract (§4) —
display only renders the affordance; it does not poll touch.

---

## Phase 2 — Native 466×466 emote art pipeline (PLAN ONLY, no art yet)

Today's pack is 284×240 frames letterbox-scaled onto the 466 canvas. Native path:

1. **New resolution dir** `assets_local/466_466/{config.json, emote.json, layout.json}`
   (copy 284_240, re-center label widths to ~360–420 px, see §3).
2. **New 466 eaf collection** `assets_local/emoji_hud_466/` (single canonical
   name — used by both the mascot disc and the Codex annulus) referenced by
   `config.json`'s `emoji_collection`. Holds the 466-composite `.eaf` files —
   mascot disc (`neutral/listen/thinking/happy/offline`) + the HUD annulus.
3. **CMake switch:** `build_speaker_assets_bin("emote" "466_466" …)` — one-line
   change in `components/common/emote/CMakeLists.txt`. Re-validate the packed
   size against the `emote` partition (build_all.py + the partition-size check
   abort if it overflows). The partition is now **6 MB @ 0x420000** (patch 0030,
   § Phase 2C) — sized for the native 466 pack; was 3 MB and the voice pack alone
   overflowed it.
4. **eaf encoding:** no local encoder exists, but we DON'T need Espressif's
   toolchain — the `.eaf` format is fully reverse-engineered (byte-exact,
   verified against a real `listen.eaf`) and the decoder accepts an
   **uncompressed RAW** encoding, so a ~60-line Python encoder suffices. The
   complete Codex-ready spec is **§ Phase 2A** below.
5. **Art production via Codex** (per user direction): Codex (codex:codex-rescue)
   writes *procedural render code* that emits frames in the packed format — a
   J.A.R.V.I.S. HUD pack (arc-reactor rings, etc.). Codex does not hand-draw;
   it generates pixels → `emit_eaf()` → `.eaf`. The spec in § Phase 2A is what
   Codex consumes. Note: the voice-reactive **waveform** is NOT baked — it is
   runtime-drawn (see § Phase 2A "baked vs runtime").

---

## Phase 2A — Codex EAF generation spec (HANDOFF TO codex:codex-rescue)

> Audience: a coding agent, not an artist. Everything below is verified against
> the live decoder `managed_components/espressif2022__esp_emote_gfx/src/lib/eaf/gfx_eaf_dec.{h,c}`
> and a real `emoji_large/listen.eaf` (byte layout + checksum confirmed). Codex
> generates pixel frames procedurally, runs them through `emit_eaf()` (given at
> the end, round-trip-tested), and drops the `.eaf` files into the collection
> dir. Codex does NOT touch the outer pack — CMake does that. **Codex MUST
> validate its output by round-tripping through a parser that mirrors `eaf_init`
> (the decoder rejects bad magic/checksum/format).** All integers little-endian.

### (1) Per-frame pixel format

Two bit depths the panel renders (`gfx_anim.c` renderer table): **8-bit indexed**
(recommended — HUD art is palette-friendly, small) or **24-bit RGB888**. 4-bit is
declared but logs "not supported". Use **8-bit indexed**:
- **Palette:** exactly `1 << bit_depth` = **256 entries, 4 bytes each, order
  `B, G, R, A`** (BGRA). Entry `00 00 00 00` = **fully transparent**. The decoder
  converts palette→RGB565 itself: `R=(r&0xF8)<<8 | (g&0xFC)<<3 | (b&0xF8)>>3`.
  So author colors as BGR888 + alpha; do NOT pre-convert to RGB565.
- **Pixels:** one byte per pixel = palette index, row-major, top-to-bottom.

### (2) Dimensions & round mask

- Canvas **466×466**. Each frame's `_S` header carries `width`/`height` (image
  dims, **constant across all frames** of one `.eaf` — verified). Center the art;
  use a frame the size of the visible motion (need not be full 466 — the layout
  places it via `layout.json` align/offset).
- **Round mask is the alpha channel** — there is no separate mask file. Make
  palette index 0 transparent (`00 00 00 00`) and paint only inside the visible
  circle; corners stay transparent so the round panel reads correctly.

### (3) `.eaf` byte layout (file → frame table → frames)

```
FILE HEADER (16 bytes)
  off 0   u8    0x89                      magic
  off 1   3s    "EAF"                     format string ("AAF" also accepted)
  off 4   i32   total_frames              (includes the trailing _C sentinel)
  off 8   u32   checksum                  = sum of every byte from off 16 to EOF, &0xFFFFFFFF
  off 12  u32   length                    = bytes from off 16 to EOF
FRAME TABLE (off 16, total_frames * 8 bytes)  — eaf_frame_table_entry_t
  per entry: u32 frame_size, u32 frame_offset (offset is relative to end of table)
FRAME DATA (off 16 + total_frames*8 + frame_offset)
  each frame begins with u16 0x5A5A (EAF_MAGIC_HEAD), then the frame body.
  The decoder reads the body at frame_mem + 2 (it skips the 0x5A5A).
```

`_S` (image) frame body, read from `frame_mem + 2`:
```
  +0   2s   "_S"
  +2   u8   pad (unused; version is read at +3)
  +3   6s   version (e.g. "v1.0\0\0")
  +9   u8   bit_depth (8)
  +10  u16  width
  +12  u16  height
  +14  u16  blocks            = ceil(height / block_height)
  +16  u16  block_height      (e.g. 32; real packs use 32)
  +18  u32[blocks] block_len  byte length of each block's encoded data
  then palette: 256 * 4 bytes (BGRA)
  then block data: for each strip, [1 byte encoding_type][encoded pixels]
```
- **Strips:** the frame is split into `blocks` horizontal strips of `block_height`
  rows (last strip is the remainder). Each strip is independently encoded;
  `block_len[i]` is that strip's encoded size *including* the 1 encoding-type byte.
- **Encoding type byte** (`block_data[0]`): `0=RLE 1=HUFFMAN 2=JPEG 3=HUFFMAN_DIRECT
  4=HEATSHRINK 5=RAW`. **Use `5` (RAW)** — the rest of the strip is then raw
  8-bit palette indices (`width * strip_rows` bytes). No compression to implement.

`_C` (control/sentinel) frame: body is just `"_C"` (after the 0x5A5A). The player
probes each frame; a `_C` frame returns `EAF_FORMAT_FLAG` which the anim widget
treats as **end-of-animation / loop boundary**. **Append exactly one `_C` frame
after the last `_S` frame** — every real pack (listen/Happy/neutral) ends this way.

### (4) Packing command (Codex does NOT run this)

Codex only writes `<emote>.eaf` files into the collection dir. The existing build
turns them into `emote_assets.bin`:
- CMake `components/common/emote/CMakeLists.txt` →
  `build_speaker_assets_bin("emote" "466_466" …)` (one-line change to the
  resolution from `"284_240"`) →
- `esp_emote_assets/scripts/spiffs_assets/build_all.py` → `build.py` reads
  `assets_local/466_466/{config,emote,layout}.json`, copies the eaf files named in
  `emote.json` from the collection dir → `spiffs_assets_gen.py` prepends `0x5A5A`
  per file, builds the mmap name/size/offset table, sums a `&0xFFFF` checksum, and
  writes `emote_assets.bin`. **The mmap layer sets width/height = 0**, so the
  panel reads real dims from each `.eaf`'s `_S` header — which is why (1)/(3)
  matter and the outer pack does not.
- build_all.py aborts if the pack exceeds the `emote` partition (6 MB @ 0x420000
  after patch 0030 — § Phase 2C).

Directory Codex produces:
```
assets_local/466_466/config.json      {"emoji_collection": "emoji_hud_466"}
assets_local/466_466/emote.json       [{"emote":"listen","src":"listen.eaf","loop":true,"fps":20}, ...]
assets_local/466_466/layout.json      (gfx object placement; copy/adapt 284_240)
assets_local/emoji_hud_466/listen.eaf (+ thinking/happy/neutral/swim/offline...)
```
emote names Codex must provide (match § Phase 1 mapping): `neutral` (idle),
`offline`, `listen`, `thinking` (→ confused), `happy` (→ speaking), plus optional
`swim/winking/sad/angry`.

### (5) Worked example + verified encoder

A 4×4, 2-color frame (idx0 transparent, idx1 red `BGRA=00 00 FF FF`), single strip:
file = `89 'EAF'` + `total=3`(2 `_S` frames + `_C`) + checksum + length + table(3×8)
+ `[5A5A '_S' 00 'v1.0\0\0' 08  04 00  04 00  01 00  20 00  <block_len u32> <256×4 palette> <05><16 index bytes>]` ×2 + `[5A5A '_C']`.

The following **round-trip-tested** encoder (plain stdlib, no numpy) produced
files my decoder-mirroring parser accepts, with checksum matching the real
`listen.eaf` algorithm. Codex should adopt it verbatim and add its procedural
frame generator on top (`frames_rows` = list of `[bytes-per-row]` index maps):

```python
import struct
EAF_MAGIC_HEAD = 0x5A5A
ENC_RAW = 5

def _emit_frame_S(rows, palette_bgra, block_height=32):
    h = len(rows); w = len(rows[0])
    blocks = (h + block_height - 1) // block_height
    block_datas = []
    for b in range(blocks):
        y0, y1 = b*block_height, min(b*block_height+block_height, h)
        strip = b''.join(bytes(rows[y]) for y in range(y0, y1))   # row-major indices
        block_datas.append(bytes([ENC_RAW]) + strip)              # [enc=5][raw pixels]
    block_len = [len(bd) for bd in block_datas]
    hdr  = b'_S' + b'\x00' + b'v1.0\x00\x00' + bytes([8])         # format,pad,version,bit_depth
    hdr += struct.pack('<HHHH', w, h, blocks, block_height)
    hdr += b''.join(struct.pack('<I', bl) for bl in block_len)
    pal  = b''.join(bytes(palette_bgra[i]) if i < len(palette_bgra) else b'\0\0\0\0'
                    for i in range(256))                          # 256 * BGRA
    return hdr + pal + b''.join(block_datas)

def emit_eaf(frames_rows, palette_bgra, path, block_height=32):
    blobs = [struct.pack('<H', EAF_MAGIC_HEAD) + _emit_frame_S(fr, palette_bgra, block_height)
             for fr in frames_rows]
    blobs.append(struct.pack('<H', EAF_MAGIC_HEAD) + b'_C')       # end-of-anim sentinel
    table = bytearray(); data = bytearray()
    for blob in blobs:
        table += struct.pack('<II', len(blob), len(data)); data += blob
    tpd = bytes(table) + bytes(data)
    checksum = sum(tpd) & 0xFFFFFFFF                              # decoder: 32-bit byte sum from off 16
    out = bytes([0x89]) + b'EAF' + struct.pack('<i', len(blobs)) \
        + struct.pack('<I', checksum) + struct.pack('<I', len(tpd)) + tpd
    open(path, 'wb').write(out); return path
```
Codex's validation step (mirror `eaf_init`): assert `d[0]==0x89 and d[1:4]==b'EAF'`;
recompute checksum over `d[16:]`; for each table entry assert the frame starts
with `0x5A5A`. (A full mirror parser lives in the throwaway test
`/tmp/eaf_emit_test.py` used to verify this section.)

### Baked-frame vs runtime-drawn (per orchestrator)

| UI element | how | why |
|---|---|---|
| Idle / Listening / Thinking / Speaking **face** (arc-reactor rings, eye motion) | **baked `.eaf`** (Codex-generated), played via `emote_set_anim_emoji` | fixed loops; cheap; matches the existing emote engine |
| Voice-reactive **waveform / amplitude bars** | **runtime-drawn** gfx primitives: `emote_create_obj_by_type("anim"/"image")` or a label/bar updated each frame from audio RMS via `gfx_obj_set_size`/`gfx_label_set_text` | must react to live mic/speaker amplitude — cannot be a fixed baked loop |
| Status line ("Listening…", "Ready * model") | runtime `gfx_label_set_text` (already shipped) | dynamic text |
| Battery / Wi-Fi / clock | runtime `emote_set_obj_visible` + `emote_set_event_msg` | dynamic values |

So Codex's deliverable is the **baked face packs** (rows 1) — the waveform (row 2)
is firmware work in the voice/display layer, not a Codex art task.

---

## Phase 2B — Composite layout: mascot face + HUD shell (the "Both" direction)

User picked **Both**: an AI-generated **mascot FACE** in the center disc, wrapped
by a Codex-generated procedural **HUD ring + voice-reactive waveform** in the
outer annulus. Two independent layers composited at runtime — they MUST be
separate `.eaf` files because the decoder requires `width`/`height` constant
within one `.eaf` (§ Phase 2A), and the disc and annulus differ in size and fps.

### Established mascot identity (for the image-gen brief)

Verified consistent across `images/mascot.png`, `images/mascot-bust.png`,
`images/igd-rebrand/mascot-jarvis-square.png` (same character) and the brand
logo `images/igd-rebrand/logo-arc-reactor-igd.png`. **There already IS a
consistent character — keep the new face on-model:**

- **Form:** chibi / Funko-style helmeted figure. Smooth matte-black armored
  body, oversized rounded helmet, a **dark visor across the upper face**,
  integrated **headphone ear-cups** on both sides, a **"J" monogram** on the
  chest. Confident, watchful, slightly forward-leaning stance.
- **Palette (sampled from the actual art, brand-exact — use these hex):**
  - Background / body: **`#000000`** pure black (ideal for AMOLED: pixels off =
    true black, no halo; make all non-art transparent).
  - Accent neon, hot core: **`#FFE25E`** (yellow-white center of the glow).
  - Accent neon, signature orange: **`#F5870B`** (the arc-reactor ring orange).
  - The accents read as a neon glow gradient hot-core→orange on black. Glow
    *emits* from the accent lines (visor lower edge, ear-cup rings, chest "J",
    base ring) — not external lighting.
- **Vibe:** Tony-Stark-AI-confidant meets chibi astronaut. Tron-like rim glow.
- **Brand cohesion bonus:** `logo-arc-reactor-igd.png` is *literally a glowing
  orange ring around a centered "J"* — that geometry IS this composite. The HUD
  annulus should echo the arc-reactor's tick-ring; the center disc echoes the
  helmet/visor face. Brief image-gen to match that logo's ring language.
- **Enclosure note:** `hardware/enclosure/.../concept-5-mascot-bust/PLAN.md`
  defines "AMOLED IS the face" with a *physical* orange accent ring at the bezel.
  So the on-screen annulus must stay INSIDE the active area (keep-out below) and
  not push orange to the pixel edge, or it smears into the physical bezel glow.

### Composite geometry (466×466 panel — exact pixels)

```
  ┌─────────────────────────────┐  466×466 panel
  │  ░ 2px keep-out (bezel) ░    │  rows 0-1, 464-465 — paint nothing
  │   ╭───────────────────╮     │
  │   │  HUD ANNULUS       │     │  ⌀462 outer → ⌀360 inner  (51px band)
  │   │   ╭───────────╮    │     │   Codex-generated .eaf, everything
  │   │   │  MASCOT    │   │     │   inside ⌀360 transparent
  │   │   │   FACE     │   │     │
  │   │   │  (disc)    │   │     │  center disc ⌀352 visible
  │   │   ╰───────────╯    │     │   (4px transparent gap to annulus)
  │   ╰───────────────────╯     │
  └─────────────────────────────┘
```

| Layer | Size / file | Placement | Owner | fps |
|---|---|---|---|---|
| **Mascot face disc** | `360×360` `.eaf`, art within ⌀352, corners transparent | `gfx` anim obj, `GFX_ALIGN_CENTER` | **image-gen** (raster frames) → `emit_eaf()` | 6–10 (gentle) |
| **HUD annulus ring** | `462×462` `.eaf`, opaque only in ⌀360→⌀462 band, center transparent | `gfx` anim obj, `GFX_ALIGN_CENTER` | **Codex** (procedural) | 20 (smooth) |
| **Voice waveform** | runtime-drawn in the inner annulus band ⌀360→⌀384 | `emote_create_obj_by_type("anim")` / bars updated from audio RMS | **firmware** (display/voice) | 30+ (live) |

- **Outer keep-out:** 2 px (active art = inscribed ⌀462 circle).
- **Annulus band:** ⌀462 → ⌀360 = 51 px each side — fits arc-reactor tick marks +
  a thin waveform track + small status glyphs (Wi-Fi/battery dots).
- **Disc/annulus gap:** 4 px transparent so the two layers read as distinct (no
  seam). Mascot visible ⌀352; annulus inner edge ⌀360.
- Both `.eaf` corners/centers are transparent (palette index 0 = `00 00 00 00`),
  so the round panel + the two-layer stack composite cleanly over true black.

### Mascot expression frames per voice state (brief for image-gen)

Image-gen produces a few coherent **keyframes** per state (same character, same
crop, same lighting, transparent bg, ⌀352 framing); motion = hold + short
crossfade between keyframes at playback. **14 distinct keyframes total:**

| Voice state | emote name | frames | what changes between keyframes |
|---|---|---|---|
| idle | `neutral` | 2 | eyes-open ↔ eyes-closed (slow blink loop) |
| listening | `listen` | 3 | eyes-open, slight lean-in, ear-cups perked (attentive) |
| thinking | `thinking` | 4 | look up-left, up-right, center, blink (considering cycle) |
| speaking | `happy` | 4 | mouth closed→¼→½→open (lipsync; play fwd+reverse = 8-frame feel) |
| offline | `offline` | 1 | eyes-closed sleep pose (static) |

These emote names match § Phase 1 so `emote_set_listening/thinking/speaking/
voice_idle` swap the **mascot** disc with no firmware change.

### How the two layers are driven (firmware contract)

- **Annulus = ONE state-agnostic looping `.eaf`** (arc-reactor "breathing"),
  started once and left running like a screensaver. It does NOT change per voice
  state — keeps the engine simple.
- **Mascot disc = swapped per state** via the existing `emote_set_anim_emoji`
  inside `emote_set_listening/thinking/speaking/voice_idle` (already shipped).
  This needs the disc anim to be the object those helpers target — a small
  follow-up: the helpers currently drive the single `eye_anim` element; in the
  composite, `eye_anim` becomes the center disc and a new `hud_anim` element
  (added to `layout.json`) holds the annulus. **Layout follow-up task**, noted
  for Phase 3.
- **Waveform = runtime** on top of both, updated from audio RMS (voice-eng owns
  the amplitude source; display owns the draw). Not baked, not Codex.

**Handoff split:** image-gen → 14 mascot keyframes (⌀352, transparent, on-model
per the identity brief + hex above) → packed via `emit_eaf()` into the 5 disc
`.eaf` files. Codex → one 462×462 annulus `.eaf` (arc-reactor ring). Firmware →
the waveform + `layout.json` two-element split.

---

## Phase 2C (DONE) — Enlarge the emote partition (the gate)

The 8 voice-face `.eaf` files pushed `emote_assets.bin` to 3,395,435 B, **244 KB
over** the old 3 MB `emote` partition → `emote_mount_assets: ESP_ERR_INVALID_SIZE`
at boot → blank face (esptool flashes it regardless; it only checks total flash,
not logical partition fit). The native 466 mascot+HUD pack needs far more.

**Fix (patch 0030 + `apply_emote_partition_resize_patch`):** reclaim the unused
`ota_1` slot (4 MB, allocated in the CSV but never flashed — OTA-B was reserved-
but-empty) and grow `emote` and `storage`. New `partitions_16MB.csv` (16 MB flash):

| Partition | Old | **New** |
|---|---|---|
| ota_0 (app) | 0x020000, 4 MB | 0x020000, 4 MB *(unchanged; app ~2.5 MB)* |
| ~~ota_1~~ | 0x420000, 4 MB | *dropped (reclaimed)* |
| **emote** | 0x820000, 3 MB | **0x420000, 6 MB** |
| **storage** | 0xB20000, 4 MB | **0xA20000, 5 MB** |

Layout still ends at `0xf20000` (unchanged top), ~896 KB free. Validated: no
overlaps, ota_0 64 KB-aligned, data partitions 4 KB-aligned. Partitions are
mounted by **label** in C (`EMOTE_ASSETS_PARTITION="emote"`, storage by label) and
flashed via `build/flasher_args.json` — no hardcoded offsets in firmware. Reflash
needs **STORAGE=1** (storage moved + grew → FATFS re-laid-out); `copy_firmware_assets`
repopulates Lua + router_rules from the repo, Wi-Fi gets re-provisioned. After this
lands, the 6 MB partition holds today's voice pack with room and the future 466
pack.

---

## Phase 3 — Layout, colors, info, theming

- **Re-center for 466:** current `layout.json` labels are `width:240`. Bump
  `default_label`/`toast_label` width to ~400, keep `GFX_ALIGN_BOTTOM_MID`,
  adjust `y` for the round bezel (content within the inner ~440 dia safe circle).
  Done in the 466 resolution dir (Phase 2), runtime-tunable via `gfx_obj_set_size`
  / `gfx_obj_align`.
- **Info elements** (add via `emote_create_obj_by_type` at init, toggle with
  `emote_set_obj_visible`):
  - **Battery:** `EMT_DEF_ELEM_BAT_LEFT_LABEL` (label) + `EMT_DEF_ELEM_CHARGE_ICON`
    (image) — fed from AXP2101 (battery cap already exists for HTTP health). Use
    `emote_set_event_msg(…, EMOTE_MGR_EVT_BAT, "87%")`.
  - **Wi-Fi:** `EMT_DEF_ELEM_STATUS_ICON` (already in layout.json) swap between
    `icon_wifi_ok.bin` / `icon_WiFi_fail.bin` (both present upstream).
  - **Clock:** `EMT_DEF_ELEM_CLOCK_LABEL` / `EMT_DEF_ELEM_TIMER_STATUS`.
- **Theming/colors:** `gfx_label_set_color(obj, gfx_color_t)` +
  `gfx_label_set_bg_color/bg_enable`. Define a small theme struct
  (`accent`, `text`, `bg`) and a `emote_set_theme(theme_t)` helper that walks the
  known labels and applies colors. Persisted theme choice → storage partition
  (FATFS), surfaced in the touch menu (§4). layout.json currently hardcodes
  `color: 11206570` (default) / `16777215` (toast) — make these theme-driven.
- **Fonts:** packed fonts `font_puhui_common_{16,20,30}_4.bin` are available;
  `gfx_label_set_font` switches per-label (larger face label, smaller status).

---

## Phase 4 — Touch-driven menus (after touch lands — task #1)

**Touch is fixed (touch-eng, patch 0010).** The CST9217 "must be initialized"
12 Hz spam was a pointer over-indirection: `esp_board_manager_get_device_handle()`
returns the INNER handle whose FIRST member is the real `esp_lcd_touch_handle_t`;
consumers were reading `->device_handle` off it (the wrapper's field) and getting
a garbage non-NULL handle with a NULL vtable.

**Touch exposes a RAW handle, ONE per app.** touch-eng's fix yields a raw
`esp_lcd_touch_handle_t` (not a gfx_touch binding); the board manager owns its
lifecycle, everyone else borrows the same pointer. Coords `0..465` both axes
(466×466), no swap/mirror — matches the composite geometry in § Phase 2B.

**Today (interim, design b):** the only consumer is `main.c::touch_monitor_task`
(gated on `CONFIG_APP_CLAW_CAP_GEMINI_LIVE`), which polls `esp_lcd_touch_read_data`
every 80 ms and rising-edge → `cap_gemini_live_toggle`. Tap-to-talk works; the
toggle is NOT display-owned.

**⚠️ Do NOT stand up a second poller (I2C contention).** CST9217 shares the I2C
bus with the audio codecs + GPIO expander; `esp_lcd_touch_read_data` does a
multi-retry tx/rx and takes `tp->data.lock`. Two pollers on the same handle race
that lock and double traffic on an already-busy bus. So the menu layer must NOT
add its own independent poll timer.

**Target (design a, recommended by touch-eng) — single owner fans out:** ONE
`gfx_touch_add(handle, cfg)` owns the read loop (`gfx_touch.c::gfx_touch_poll_cb`
reads once per tick, fans out via `gfx_touch_dispatch` → per-object touch_event +
a global `event_cb`), and is **IRQ-driven** (5 ms poll only while INT GPIO11 fires;
chip sleeps otherwise). Until that migration, the menu stays unbuilt rather than
adding a second poller.

**Migration mechanics (confirmed with voice-eng — low-risk):**
- The toggle is **already decoupled** from the poller: `touch_monitor_task` just
  calls the public `cap_gemini_live_toggle()` (in `cap_gemini_live.h`) on rising
  edge; the toggle has zero knowledge of how the tap was detected. So no
  `cap_gemini_live.c` internal change is needed.
- Migration = (1) delete `touch_monitor_task` and (2) register
  `cap_gemini_live_toggle` as the center-tap subscriber on the shared `event_cb`.
- **Ownership of the delete:** `touch_monitor_task` lives in **esp-claw `main.c`
  (upstream)** and touch-eng's patch 0010 already edits that function — so
  retiring it is a `main.c` edit on **touch-eng's** surface, not voice-eng's.
  voice-eng provides the toggle subscriber hook; touch-eng + display own the
  poller removal + event routing.
- **Debounce:** the IRQ `event_cb` must edge-detect (fire center-tap only on a
  clean press→release rising edge) so `cap_gemini_live_toggle` isn't called twice
  per tap — it flips `session_active` + an event bit, cheap but not double-safe.
  The current 80 ms poll already edge-detects; the IRQ owner must replicate that.

**Handle-fetch contract** (for ANY `gfx_touch_add` / `gfx_touch_config_t.handle`
the display layer adds — use the working Lua path, NOT the buggy double-deref):
```c
void *h = NULL;
esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH, &h);
esp_lcd_touch_handle_t tp = ((dev_lcd_touch_i2c_handles_t *)h)->touch_handle; /* first member */
/* tp->config.int_gpio_num == 11, interrupt_callback set → gfx_touch_start picks the IRQ path */
```

**Menu design when it lands:** under the design-(a) single owner, the `event_cb`
fans out by gesture/region — center tap → tap-to-talk toggle, long-press or edge
swipe → settings overlay (distinct gestures so the menu never clashes with the
toggle). This is the gate for menus: build the menu only after the migration to a
shared `gfx_touch` owner (joint voice-eng + display call), never as a second
poller. The affordance display (§ Phase 1) only renders the hint; it does not
poll touch.

**Menu rendering (when unblocked):** build a settings overlay from
`emote_create_obj_by_type("label"/"image", …)` rows (theme, voice on/off, model,
Wi-Fi setup QR via `EMT_DEF_ELEM_QRCODE` + `emote_set_qrcode_data`), shown/hidden
with `emote_set_obj_visible`, dismissed on outside tap. Respect the display
arbiter — the menu and the emote face share `DISPLAY_ARBITER_OWNER_EMOTE`.

---

## Phase 5 (PRIMARY VISUAL — IMPLEMENTED) — Reactive "Siri-style" waveform face

**Art direction changed:** the user rejected the chibi mascot (too detailed) and
chose a minimalist reactive waveform as the **primary** face. The mascot/HUD
composite (§ Phase 2B) is **demoted to optional fallback** (still valid for "Both"
mode if revisited; mascot-eng's 5 disc `.eaf` remain usable as the `face off`
aesthetic). The waveform is pure motion, no face.

**Constraint that shaped the design:** emote_gfx has **no vector primitives**
(only label/anim/img/qrcode). So the waveform is rasterised per-frame into a
PSRAM **RGB565A8** strip (466×120, double-buffered, ~328 KB) and pushed to a gfx
**image** object via `gfx_img_set_src()` — which calls `gfx_obj_invalidate()`, so
the engine repaints at its fps. Verified against the live decoder: C-array magic
`0x19`, `header.w/h/cf` read from the dsc (`gfx_img_dec.c:183`), RGB565A8 alpha
plane at `w*h*2` (`gfx_img.c:133`). A 20 fps FreeRTOS task eases displayed
amplitude toward target (`disp += (target−disp)*0.25`) for fluidity.

**States** (`emote_face_state_t`, driven by `emote_face_set_state()`):
| State | Motion | Amplitude source |
|---|---|---|
| IDLE | calm breathing centre line (thickness+brightness pulse) | none (synthetic) |
| LISTENING | amplitude bars, centre-weighted envelope + shimmer | mic RMS |
| THINKING | traveling gaussian scan pulse over a dim baseline | none (synthetic) |
| SPEAKING | amplitude bars | output RMS |
Palette: hot core `#FFE25E`, accent `#F5870B`, true-black bg, alpha fade to tips.

**Object model:** a new `"waveform"` gfx image object (`emote_create_obj_by_type`,
`GFX_ALIGN_CENTER`). When active it's shown and the emote eye is hidden
(`emote_set_anim_visible(false)`); `face off` reverses it (restores the emote
idle). One z-order, shares `DISPLAY_ARBITER_OWNER_EMOTE`, drawn only when owner.

**Cameraless verification (the sign-off gate):** the board has no camera, so the
`face` CLI command drives each state with a **synthetic** amplitude so the user
can watch each state on the AMOLED *without* a Gemini session:
```
face off | idle | think
face listen [pct]   face speak [pct]    # pct 0-100; omit = sinusoidal sweep
```

**Audio hook (pluggable):** `emote_face_set_amplitude_source(cb)` registers a
live source where `cb(state)→0..1000`; `emote_face_set_synthetic_amplitude(milli)`
feeds the CLI demo (`<0` restores the live cb). **Patch 0031** is self-contained on
synthetic amplitude (validates immediately). **Patch 0032** (pending voice-eng)
wires the live getters — see Open coordination.

**Files** (patch 0031 + `apply_reactive_waveform_face_patch`, vendored
`firmware/emote/waveform.c`): new `waveform.c`, `emote.h` face API, `emote.c`
init call, `CMakeLists.txt` SRCS, `app_claw_cli.c` `face` command (emote-gated).
Verified: bash `-n` clean, apply anchors confirmed against a clean post-0012
baseline, idempotent (guards on `emote_face_set_state` / `cmd_face`).

---

## Open coordination

- **voice-eng:** RESOLVED — all 5 face calls centralized in `gl_set_state()`'s
  switch (patch 0012 supplies the symbols + art; links clean). Caveat: nothing
  transitions into `GL_STATE_THINKING` yet, so the Thinking face is wired but
  never fires today (see § Phase 1).
- **touch-eng:** RESOLVED — CST9217 fixed (patch 0010); tap-to-talk is
  `main.c::touch_monitor_task` → `cap_gemini_live_toggle`; handle-fetch contract
  documented in § Phase 4; coords 0..465 no swap.
- **orchestrator:** runs the single integrated build + flashes; validates the
  faces render once patch 0012 + eaf art land.
