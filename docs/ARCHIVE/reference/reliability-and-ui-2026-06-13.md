# Reliability & Interactive UI — 2026-06-13

> Status: investigation + remediation plan. Touch-init cure and feedback overlay
> are **applied to canonical sources** (verified in-tree below); voice patches are
> **committed but not yet hardware-verified** (Gemini daily quota exhausted; resets
> midnight Pacific). The interactive (tappable) display is a **designed, phased
> effort** — answer to the user's "can the screen do this?" is a clear **YES**, see §4.

This page answers two user reports captured live on 2026-06-13:

1. *"Device is buggy — works sometimes, other times partly works."*
2. *"Can the SCREEN show data / give me tappable options / show images?"* (today it
   only shows the animated face.)

Evidence captured live (SD log + diag endpoints): 6-boot SD log, plus
`touch.json`, `display.json`, `health.json`, `status.json`, `gemini.json`.

---

## 1. Current state — what works, what's intermittent

### What works reliably
- **Display / emote face.** The emote engine renders the round AMOLED on every
  boot, including boots where touch is dead. Live `display.json`:
  `running:true, object_ready:true, display_owned:true, driver_ticks:1919`. This
  is load-bearing for §3: a feedback overlay built on the emote lane is reliable
  *precisely in the failure mode we care about* (touch dead, display alive).
- **Wi-Fi / STA + HTTP diag.** `status.json`: `wifi_mode:"sta_ok"`, connected,
  diag endpoints reachable (`health.json requests:2`).
- **Voice plumbing (when quota allows).** `gemini.json` shows the d9031ac runtime
  config is live: `mic_pga_db:24`, `ref_pga_db:12` (state-aware gains),
  `barge_rms_threshold:9000`, server-VAD scaffolding present. State is `IDLE` at
  capture (no active session).

### What is intermittent — the key finding: CST9217 touch init
The primary current blocker. The capacitive touch controller (CST9217, 7-bit
`0x5A`, INT GPIO 11, RST GPIO 40 — `board_devices.yaml:127-149`) **intermittently
fails to initialise at board bring-up.**

**Boot census (last 6 boots):** 4 booted with working touch; the **last 2 booted
with touch dead.** A dead-touch boot shows this exact signature, repeating forever:

```
BOARD_MANAGER: Device lcd_touch not found, error: 801fe
BOARD_DEVICE:  Device lcd_touch handle is NULL
touch_mon:     Waiting for LCD touch handle      (× ~1564 in one boot)
```

Live SD-log census of the captured `logs.txt` confirms the signature is dominant:
`801fe` and `handle is NULL` appear **782×** each; `Waiting for LCD touch handle`
appears **195×**. Live `touch.json` shows the task stuck at the top of the wait
loop: `started:true, touch_ready:false, polls:0`.

**Why this is the "buggy" symptom:** when touch is dead, taps do nothing → the
user cannot start a Gemini conversation → *"I see the lights but that's it."* The
calm neutral idle face stays up, giving zero indication anything is wrong.

**Why it smells like a race, not a dead chip:** it flips boot-to-boot (4/6 good).
The shared I2C bus (`board_devices.yaml:3-4` documents init order: IO expander →
PMIC → display → touch) carries the codecs (ES8311 `0x30`, ES7210 `0x80`), PMIC
(AXP2101 `0x34`), IMU, and CST9217. A cold/soft-boot chip that hasn't ACKed yet
fails the probe. **Root cause confirmed in source — see §3, patch #2.**

### Quota note (external, not a bug)
The Gemini Live **daily quota is exhausted tonight** from heavy debugging: the
server returns only `setupComplete` then goes silent. Resets midnight Pacific.
This means the voice regression (§5) cannot run tonight, but **every reliability
and UI step except the live tool-call round-trip is testable offline** via the
`/api/*` endpoints + `snapshot.ppm`.

---

## 2. Ranked roadmap (reliability first)

The ordering is deliberate: **touch must be solid before tappable UI is worth
building** — a tappable UI on flaky touch is worthless.

| # | Effort | Area | What |
|---|--------|------|------|
| 1 | small  | touch-init   | **Self-heal:** re-init `lcd_touch` in the wait loop instead of spinning on NULL forever. *(applied)* |
| 2 | medium | boot-i2c     | **Root-cause cure:** reset CST9217 *before* the first I2C probe. *(applied via bootstrap patch)* |
| 3 | small  | ux-feedback  | **Make failure visible:** emote alert overlay + boot-time touch-NULL alert. *(applied)* |
| 4 | medium | voice        | Verify d9031ac patches shipped + lwIP TX-buffer override reaches the build. *(pending quota)* |
| 5 | medium | boot-i2c     | Harden shared boot check: required-device retry + SD touch breadcrumb + stale-coredump erase. *(applied via bootstrap patch)* |
| 6 | large  | ui           | UI-layer skeleton + arbiter handoff (P1 of the tappable screen). |
| 7 | large  | ui           | Choice arcs + tap hit-test + Gemini `ask_user` tool-call + data/image scenes (P2–P5). |

Reliability (1–3) → voice re-verify (4) → boot hardening (5) → interactive UI
(6–7, the "large" feature, gated on touch being proven across many boots).

---

## 3. The touch-init cure + other patches (apply order)

### Patch #1 — Touch self-heal (re-init instead of spin) · *applied*
**File:** `firmware/main/touch_demo.c` (canonical; `bootstrap.sh` copies
`firmware/main/*` into the esp-claw tree on build).

**Change:** the old acquire loop only ever *read* the handle via
`esp_board_manager_get_device_handle`, so a failed boot logged
`Waiting for LCD touch handle` every 500 ms forever. The loop now re-runs the real
init path. Verified in the current file at `touch_demo.c:301-340`:

- Every ~3 s of NULL (`wait_ticks % 6` at the 500 ms cadence) it calls
  `esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_LCD_TOUCH)`
  (`touch_demo.c:313-316`) and immediately re-reads the handle on `ESP_OK`.
- Capped at `TOUCH_REINIT_MAX = 20` (~60 s); after the cap it logs
  `lcd_touch re-init exhausted`, records `last_action="touch_init_failed"`, and
  `vTaskDelete(NULL)`s so the task can't busy-spin on dead hardware
  (`touch_demo.c:324-334`).

**Why it works (verified in source):** a failed boot init leaves
`device_handle == NULL` with `ref_count` decremented
(`esp_board_device.c:139` decrements on init failure). So
`esp_board_manager_init_device_by_name` does **not** hit the "already
initialized" early-return (`esp_board_device.c:129-131` only returns early when
`handle->device_handle` is non-NULL) — it re-enters the real init at
`esp_board_device.c:136` and re-runs probe+reset+config.
`esp_board_device_get_handle` returns `ESP_BOARD_ERR_DEVICE_NO_HANDLE` when
`device_handle == NULL` (`esp_board_device.c:154-156`) — that is the live `0x801fe`.
Signature confirmed: `esp_board_manager.h:172`
`esp_err_t esp_board_manager_init_device_by_name(const char *dev_name)`.

**Acceptance:** on a failed boot, SD log shows `Re-init lcd_touch: ESP_OK` within
a few seconds → `Touch handle acquired, polling started`, *without* a power cycle;
`touch.json` flips `touch_ready:false → true` and `polls` increments. Healthy
boots show **zero** `Re-init` / `Waiting` lines. No Gemini quota needed.

### Patch #2 — Reset CST9217 before the first I2C probe (root cause) · *applied via bootstrap*
**File:** `scripts/bootstrap.sh` — new idempotent
`apply_touch_reset_before_probe_patch()` (present at `bootstrap.sh:2125`,
dispatched before `apply_touch_handle_deref_patch`). It patches an esp_board_manager
**managed_component that bootstrap overwrites every build**, so it must live here,
not as a bare edit.

**Root cause (verified in source):**
`dev_lcd_touch_i2c.c:51-62` seeds `io_i2c_config.dev_addr = 0x00` then runs a
single `i2c_master_probe(addr, 200 ms)` (`dev_lcd_touch_i2c.c:56`) **before the
chip is ever reset.** The CST9217 reset pulse lives *inside* the driver,
reached only at `dev_lcd_touch_i2c.c:86` (`lcd_touch_factory_entry_t`) — **after**
the probe. A chip that hasn't ACKed fails the probe → `dev_addr` stays `0x00` →
panel IO built for addr 0 → init returns `-1` → handle NULL → `0x801fe`.

**Fix:** drive `rst_gpio_num` (40) as OUTPUT and pulse it (low 10 ms / high 60 ms,
matching the driver's own levels) immediately before the probe loop, so the chip
is awake the first time it is addressed. Guarded by `rst_gpio_num != GPIO_NUM_NC`
and `gpio_config() == ESP_OK` (warns and falls through on failure — cannot make a
working boot worse). Idempotent via a sentinel comment + an include-presence check.

**Acceptance (multi-boot, not single):** **10/10 cold boots** acquire the handle on
the first try — SD-log census shows zero `0x801fe`, zero `handle is NULL`, zero
`Waiting for LCD touch handle`; `DEV_LCD_TOUCH_I2C` logs `Successfully initialized:
lcd_touch` every boot; `touch.json touch_ready:true` every boot. The bug is
intermittent, so a single good boot is **not** sufficient.

### Patch #3 — Make touch failure visible (emote alert overlay) · *applied*
**Files:** `firmware/emote/emote.h`, `firmware/emote/emote.c`,
`firmware/main/touch_demo.c` (all canonical).

Adds `emote_set_alert(emote_alert_t kind, const char *line)` / `emote_clear_alert()`
(declared in `emote.h:35-40`, `EMOTE_ALERT_GENERIC` / `EMOTE_ALERT_TOUCH`). The
overlay uses **only existing mechanisms** — no new asset, no second display owner:
- the dark-visor **`offline`** baked face (the only "trouble" face baked; the 5
  faces are `neutral/listen/thinking/happy/offline` per `firmware/mascot/emote.json`
  — there is **no** dedicated error face),
- a ≤24-char strip line via the existing `EMOTE_MGR_EVT_SYS` text path,
- `EMOTE_FACE_OFF` to kill the reactive waveform.

A new `s_alert_active` flag **early-returns** in both `emote_render_status()` and
`emote_set_status_detail()` so a late status write can't clobber the alert. Refresh
fires **only** when `display_arbiter_is_owner(EMOTE)` — the same deadlock-safe
pattern `emote_apply` uses — and all calls are pure state writes from the
`touch_mon` task (never the render task or session task), per the documented
render-lock deadlock note.

Wiring in `touch_demo.c`: after ~3 s of NULL (`TOUCH_ALERT_AFTER_TICKS = 6`,
`touch_demo.c:297`) it raises `emote_set_alert(EMOTE_ALERT_TOUCH, "Touch offline -
reboot")` **once** (bool guard) while the re-init from patch #1 keeps running, and
calls `emote_clear_alert()` on acquisition.

**Acceptance:** on a failed boot, `GET /api/display/snapshot.ppm` shows the dark
offline face + "Touch offline - reboot" instead of the calm idle face; on a healthy
boot no alert appears. Fully offline-testable.

### Patch #5 — Boot-time hardening (retry + breadcrumb + coredump erase) · *applied via bootstrap*
**File:** `scripts/bootstrap.sh` — new idempotent `apply_boot_touch_breadcrumb_patch()`
(present at `bootstrap.sh:3440`). Patches `application/edge_agent/main/main.c`
(exists only in esp-claw → patched, never bare-edited).

Three defensive fixes:
- **(a) Bounded boot retry:** `esp_board_manager_init()` returns `ESP_OK` even when
  touch failed — confirmed: `esp_board_device_init_all` logs `Failed to initialize
  device` but unconditionally `return ESP_OK` (`esp_board_device.c:404-408`), so
  `ESP_ERROR_CHECK` can't catch it. Add a bounded loop (up to 3×) after board init
  that re-inits touch if the handle is NULL, giving clean retries before the app
  starts.
- **(b) SD touch breadcrumb:** the decisive `Failed to init device: lcd_touch` line
  is UART-only because the logger starts *after* board init. Stash a `touch=ok/failed`
  flag and emit it in `boot_diag_log()` (runs after the logger) so every boot's
  touch state lands in the SD log.
- **(c) Stale-coredump erase:** call `esp_core_dump_image_erase()` after reporting a
  PRESENT coredump, so `coredump=PRESENT` reflects only a fresh crash (today it
  reads PRESENT every boot, masking new crashes).

**Acceptance:** every SD log carries an explicit `touch=ok`/`touch=failed`
breadcrumb; a flaky boot shows 1–3 retries then success; `coredump=PRESENT` only on
a boot that actually crashed.

### Patch #4 — Voice verification (lwIP TX buffer + d9031ac) · *pending quota*
**File:** `esp-claw/application/edge_agent/sdkconfig` (regenerate) + verify
d9031ac. The lwIP `16384` TCP send-buffer override is suspected not to reach the
built firmware (bootstrap reads an existing sdkconfig and may not re-fold the board
override). **Action:** `idf.py fullclean` (or remove the stale sdkconfig) so the
board override is re-folded, then grep the generated sdkconfig for
`CONFIG_LWIP_TCP_SND_BUF_DEFAULT=16384`. Separately, the committed-but-unverified
d9031ac set (in-session WS resume, `activityHandling=NO_INTERRUPTION`, barge guard,
PCM retry, state-aware mic gain, 20 s reply watchdog) needs a hardware regression
**after quota resets** — see §5. Heap context: `health.json
internal_largest_free_block:31744` confirms the internal-heap pressure that
motivated the AEC OOM and WS-death work.

> ⚠️ **Patch #4 is design-asserted, not yet verified.** Grep the built sdkconfig and
> run the live cycles before trusting it.

---

## 4. The interactive display — answering "can the screen do this?" → **YES**

The user wants the round AMOLED to show **data**, give **tappable options**, and
show **images** — not just the animated face. **This is fully buildable today with
ZERO new graphics infrastructure**, because the 2D renderer, the multi-owner
arbiter, and the decoders already exist in-tree and are already compiled into this
firmware.

### Mechanism: reuse the existing `display_hal` as a *second* arbiter owner
Not baked emote-gfx (no runtime canvas — proven dead on HW), not a fresh LVGL port.
A new **C "UI layer"** (`firmware/ui_layer/`, a canonical in-tree component wired
like `firmware/emote`) becomes a second display-arbiter owner driving `display_hal`
directly — the same HAL the Lua module already wraps.

**Three facts make this cheap, all verified in source:**

1. **A multi-owner arbiter already exists.** `display_arbiter.h:17-27` defines
   owners `NONE / LUA / EMOTE` plus `acquire/release/is_owner`. The emote flush
   callback early-returns when not the EMOTE owner, so the face *already politely
   yields the panel* the moment another owner acquires it (Lua uses this today).

2. **A full 2D HAL already drives this exact panel.** `display_hal.h` exposes
   (verbatim) `display_hal_fill_arc(cx,cy,inner,outer,start,end,color)` /
   `display_hal_draw_arc` — the exact choice-arc primitive — plus
   `fill_round_rect`, `fill_circle`, `draw_text` / `_aligned`, `draw_bitmap` /
   `_crop` / `_scaled`, `draw_jpeg` / `_crop` / `_scaled`, `begin_frame` / `present`
   / `present_rect`, `set_clip_rect`, `create` / `destroy`
   (`display_hal.h:52-141`). Double-buffered in PSRAM, does the CO5300 byte-swap.

3. **Decoders + font are already linked.** `CONFIG_APP_CLAW_CAP_LUA=y` links
   `display_hal`; libpng + esp_new_jpeg back `draw_jpeg`; `esp_painter` font 24 is
   enabled.

### What each requested capability maps to
| User ask | Mechanism | HAL call |
|----------|-----------|----------|
| **Tappable options** | annular choice-arc wedges hugging the bezel | `display_hal_fill_arc` + `draw_arc` + `draw_text` |
| **Data / selectable list** | round cards + text + a radial dial menu | `display_hal_fill_round_rect` + `draw_text` |
| **Images / photos** | decode a stored/fetched JPEG, present once | `display_hal_draw_jpeg_scaled` |

### Tappable mechanism (CST9217 → arc hit-test)
`touch_demo.c` already reads true 0..466 panel coordinates via
`esp_lcd_touch_get_coordinates` (board `x_max/y_max=466`). The prototype's
hit-test math (`docs/prototype/jarvisnano-os.html:407-416`) ports straight to C:
`dx=x-233; dy=y-233; dist=hypot(dx,dy); ang=atan2(dy,dx)`, normalized, wrap-aware
compared against each option's stored `[a0,a1]` with `dist>inner_radius`. The
choice-arc geometry mirrors the prototype `drawChoices()` (lines 278-293:
`topGap=1.2` rad reserved at 12 o'clock for the question, inner ~132 / outer ~168).
**One I2C reader** stays in `touch_demo.c`: when a UI scene is active, taps route
to the UI layer; otherwise they fall through to the existing Gemini toggle (the
50 ms shared-bus poll rule is preserved).

### Triggers (how a scene appears) — all use existing transport
- **Gemini tool-call.** `cap_gemini_live.c` already declares `functionDeclarations`
  (`:1437`) alongside `googleSearch` (`:1429`) and dispatches `toolCall` frames to a
  worker that returns `functionResponses` (`gl_run_tool_call:3265`, detach at
  `:3649`, response array at `:3270`). Add an `ask_user(question, options[])`
  declaration; on `toolCall` → show the choice arcs; the user's tap returns the
  chosen label as the `functionResponse` — **zero new transport.** This is the
  VISION's "asks you questions you tap to answer."
- **HTTP `/api`.** Mirror `POST /api/display/face` (`http_server_display_api.c:357`)
  with `POST /api/ui/choice|data|image` for headless testing without burning quota;
  verify with `GET /api/display/snapshot.ppm` (`:347`).
- **Local.** Long-press / gesture opens the radial menu via the existing
  `touch_demo.c` long-press hook.

### Arbitration & safety (verified, must be honored)
- **One writer at a time:** both flush paths gate on `display_arbiter_is_owner`
  before `esp_lcd_panel_draw_bitmap` — no concurrent draw. Drain the flush-done
  semaphore on handoff to avoid a torn frame.
- **No deadlock:** `emote.c` documents that calling `emote_apply()` from the voice
  task deadlocks the render task. The UI layer must drive `display_hal` from **its
  own dedicated task** — never the gfx render task or the Gemini session task.
- **PSRAM budget:** `display_hal` allocs ~2×434 KB framebuffers + a swap buffer on
  `create`. Use **create-on-show / destroy-on-dismiss** so it isn't resident
  alongside the emote strip buffers; ~1.3 MB of the 8 MB PSRAM is display if both
  stay live. `health.json spiram_free:1537624, spiram_largest_free_block:1245184`
  is the budget to verify against.
- **FPS ceiling:** the QSPI panel tops out ~23 fps for a full 466×466 RGB565 frame.
  Static UI (arc / data / image) presents once at near-zero ongoing cost; **do not**
  expect 60 fps full-screen animation — keep animated regions small or use
  `present_rect`.

### Phased milestones (every step except P4 is offline-testable)
- **P0 — Touch reliability (gating).** §3 patches #1–#2 verified across many cold
  boots. *No UI work until this is solid.*
- **P1 — UI-layer skeleton + arbiter handoff.** `firmware/ui_layer/`,
  `ui_layer_show/dismiss` acquiring `DISPLAY_ARBITER_OWNER_LUA` (reuse) →
  `display_hal_create` → render ONE hard-coded test frame (black + centered amber
  circle) → `dismiss` returns to the live face within ~1 frame. Proof via
  `snapshot.ppm`.
- **P2 — One static 2–3 option choice arc** via `POST /api/ui/choice` (no quota).
- **P3 — Make it tappable:** `ui_layer_on_tap(x,y)` hit-test wired into the
  `touch_demo.c` dispatcher; UI-scene taps select an option and do **not** toggle a
  Gemini session.
- **P4 — Gemini `ask_user` tool-call** (needs quota, do **last**): toolCall →
  arcs → tap resolves the tool-call → Gemini speaks the matching branch.
- **P5 — Breadth:** data panel, image view (`draw_jpeg_scaled`), radial menu;
  document in a new `docs/reference/ui-layer.md` per the capture rule, and narrow
  `display-emote-gfx.md`'s blanket "no runtime rendering" line to "no gfx_canvas;
  gfx_label/gfx_button exist at runtime in esp_emote_gfx 3.0.2."
- **P6 (deferred, optional):** LVGL as a second arbiter owner — only if rich
  scrolling/momentum/many widgets are later required (Waveshare's own LVGL demo hits
  200–300 fps on this panel). Keep `display_hal` for v1.

---

## 5. Execution checklist (for the orchestrator)

Reliability first; UI strictly behind verified touch. Build/flash one-liner and
`idf.py flash --flash-mode dio` per `CLAUDE.md`.

1. **Build with the touch patches** — confirm bootstrap logs
   `applying touch reset-before-probe patch (cure CST9217 boot race)` and
   `applying boot_touch_breadcrumb patch`. Confirm the injected reset code landed in
   the regenerated esp-claw `dev_lcd_touch_i2c.c` (grep the sentinel == 1) and the
   build compiled (gpio/FreeRTOS symbols resolved).
2. **Idempotency** — run bootstrap a second time; expect `... patch already applied`
   and a still-clean build.
3. **Flash** — `idf.py flash --flash-mode dio`.
4. **Touch reboot-loop test (the gate):** power-cycle **10×** (full power removal).
   PASS = **N/N boots** acquire the handle: zero `0x801fe`, zero `handle is NULL`,
   zero `Waiting for LCD touch handle`; `touch.json touch_ready:true` every boot;
   every SD log carries a `touch=ok` breadcrumb; tapping registers. A single good
   boot is **not** sufficient — the bug is intermittent.
5. **Failure-feedback check** (if a fault boot is caught, or by forcing NULL):
   `snapshot.ppm` shows the dark offline face + "Touch offline - reboot"; re-init
   keeps running; on late recovery the alert clears and the face returns to neutral.
6. **Voice re-verify — after quota resets (midnight Pacific):**
   - Grep the built sdkconfig for `CONFIG_LWIP_TCP_SND_BUF_DEFAULT=16384` (fullclean
     first if it still reads 5760).
   - Run 3 long + 3 short Gemini cycles: PASS = 6/6 complete with audio out, zero
     `transport_poll_write(0)` WS death, AEC create/destroy churn within heap budget
     (`internal_largest_free_block`), reply before the 20 s watchdog.
7. **Coredump hygiene** — clean power-on reads `coredump=NONE`; only the boot right
   after a real crash reads `coredump=PRESENT`.
8. **UI Phase 1** — *only after step 4 passes N/N across many boots.* Stand up
   `firmware/ui_layer/`, render the P1 test frame, verify clean handoff back to the
   face via `snapshot.ppm`. Then P2 → P3 (offline) → P4 (post-quota) → P5.

---

## Source citations (file:line, verified in-tree 2026-06-13)

- `firmware/main/touch_demo.c:301-340` — self-healing acquire loop (re-init,
  `TOUCH_REINIT_MAX=20`, alert raise, cap-then-`vTaskDelete`).
- `firmware/main/touch_demo.c:30-32` — shared-I2C ≥50 ms poll rule.
- `esp-claw/.../espressif__esp_board_manager/include/esp_board_manager.h:172` —
  `esp_board_manager_init_device_by_name` signature; `:83` get-handle; `:51` init.
- `esp-claw/.../esp_board_manager/include/esp_board_manager_defs.h:29` —
  `ESP_BOARD_DEVICE_NAME_LCD_TOUCH "lcd_touch"`.
- `esp-claw/.../esp_board_manager/src/esp_board_device.c:126` ref_count++;
  `:129-131` already-init early-return; `:136` init call; `:139` decrement on
  failure; `:154-156` `ESP_BOARD_ERR_DEVICE_NO_HANDLE` when handle NULL;
  `:404-408` `init_all` logs then `return ESP_OK` unconditionally.
- `esp-claw/.../esp_board_manager/devices/dev_lcd_touch_i2c/dev_lcd_touch_i2c.c:51`
  `dev_addr=0x00`; `:56` probe (before reset); `:86` `lcd_touch_factory_entry_t`
  (driver reset lives here, after the probe); `:69`,`:91` `return -1` on failure.
- `boards/waveshare/esp32s3_touch_amoled_1_75/board_devices.yaml:3-4` init order;
  `:127-149` CST9217 (`dev_addr 0x5A`, `int_gpio 11`, `rst_gpio 40`, x/y_max 466).
- `scripts/bootstrap.sh:2125` `apply_touch_reset_before_probe_patch`; `:3440`
  `apply_boot_touch_breadcrumb_patch`.
- `firmware/emote/emote.h:35-40` — `emote_alert_t` + `emote_set_alert` /
  `emote_clear_alert` (applied).
- `firmware/mascot/emote.json` — 5 baked faces (no error face; `offline` is the
  trouble face).
- `esp-claw/components/common/display_arbiter/include/display_arbiter.h:17-27` —
  owners NONE/LUA/EMOTE + acquire/release/is_owner.
- `esp-claw/components/lua_modules/lua_module_display/include/display_hal.h:52-141`
  — create/destroy, begin_frame/present/present_rect, fill_arc/draw_arc,
  fill_round_rect, fill_circle, draw_text/_aligned, draw_bitmap*/draw_jpeg*,
  set_clip_rect.
- `firmware/components/cap_gemini_live/src/cap_gemini_live.c:1429` googleSearch;
  `:1437` functionDeclarations; `:3265` `gl_run_tool_call`; `:3270`
  functionResponses; `:3649` toolCall detach.
- `firmware/http_server/http_server_display_api.c:347` snapshot.ppm; `:357`
  `/api/display/face` POST template.
- `docs/prototype/jarvisnano-os.html:278-293` `drawChoices()` geometry
  (`topGap=1.2`, inner edge ring at r=135); `:407-416` pointerdown hit-test
  (`hypot`/`atan2`).
- Live evidence (`/tmp/jarvis-ev2/`, captured 2026-06-13): `touch.json`
  (`touch_ready:false, polls:0`), `display.json` (`running:true,
  display_owned:true`), `health.json` (`internal_largest_free_block:31744,
  spiram_free:1537624`), `gemini.json` (`mic_pga_db:24, ref_pga_db:12,
  barge_rms_threshold:9000, state IDLE`); SD-log census: `801fe`/`handle is NULL`
  782× each, `Waiting for LCD touch handle` 195×.

## External references (mark unverified)
- Gemini Live API best practices — quota/`goAway`/`RESOURCE_EXHAUSTED` behaviour:
  https://ai.google.dev/gemini-api/docs/live-api (not fetch-verified this pass).
- esp_emote_gfx 3.0.x runtime `gfx_label`/`gfx_button`:
  https://components.espressif.com/components/espressif2022/esp_emote_gfx (not
  fetch-verified this pass).
- Waveshare ESP32-S3-Touch-AMOLED-1.75 LVGL demo (200–300 fps; CST9217 init):
  https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75 (not fetch-verified
  this pass).
