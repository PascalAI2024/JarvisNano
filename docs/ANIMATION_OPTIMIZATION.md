# Animation Optimization — Waveshare AMOLED-1.75 (ESP32-S3 / CO5300 / esp_emote_gfx)

> Research compiled 2026-05-23. Purpose: diagnose current animation quality issues and
> document the exact, hardware-grounded changes needed to achieve smooth, high-quality
> display animations on the JarvisRobot platform.

---

## 0. Hardware Summary

| Item | Value | Notes |
|------|-------|-------|
| Display | Waveshare ESP32-S3-Touch-AMOLED-1.75 | Round form factor |
| Panel driver IC | **CO5300** | AMOLED/LTPS single-chip controller |
| Interface | **QSPI** (Quad SPI) | 4-bit data; D/CX signal for cmd vs data |
| Resolution | **466 × 466** pixels | ~217,156 total pixels |
| Color depth | 16-bit RGB565 (rendered), 24-bit panel capability | |
| Touch controller | CST9217 via I2C | Separate from display SPI bus |
| MCU | ESP32-S3 | Dual-core Xtensa LX7 |
| PSRAM | 8 MB Octal (OPI), 80 MHz | `CONFIG_SPIRAM_MODE_OCT=y`, `CONFIG_SPIRAM_SPEED_80M=y` |
| Flash | XIP from PSRAM enabled | `CONFIG_SPIRAM_XIP_FROM_PSRAM=y` |
| CPU frequency | **240 MHz** | Already at maximum |
| FreeRTOS tick | **100 Hz** (10 ms resolution) | `CONFIG_FREERTOS_HZ=100` |

---

## 1. Current State Audit (What's Causing the Problems)

### 1.1 🔴 CRITICAL — Compiler is in DEBUG Mode

```
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y   # This is -O0 (NO optimization)
CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
```

**Impact:** `-O0` disables ALL compiler optimizations. Every pixel-blend loop,
every lerp calculation, every EAF frame decode runs at maximum instruction count
with zero loop-unrolling, no inlining, no register allocation. This alone can
reduce rendering throughput by **3–5×** compared to `-O2`.

**Fix:** Change to performance optimization in `sdkconfig`:
```
CONFIG_COMPILER_OPTIMIZATION_PERF=y
# Remove: CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
```

Or via `idf.py menuconfig` → Compiler options → Optimization Level → Performance (-O2).

---

### 1.2 🔴 CRITICAL — GFX Render Engine FPS Capped at 10 FPS

In `esp-claw/components/common/emote/emote.c` line 155:
```c
.fps = 10,   // ← The gfx_core render loop only fires 10 times per second
```

The `gfx_render_loop_task` (inside `esp_emote_gfx/src/core/gfx_core.c`) uses this
value via `gfx_timer_mgr_init` to gate how often it actually renders and flushes.
No matter what FPS you set on the animation via `gfx_anim_set_segment(…, fps, …)`,
the engine only *presents* a new frame 10 times per second.

Our animation constants in `reactive_face.c`:
```c
#define RWAVE_FPS_IDLE     20   // ← Never achievable; engine cap is 10
#define RWAVE_FPS_REACTIVE 24   // ← Never achievable; engine cap is 10
#define RWAVE_FPS_THINK    24   // ← Never achievable; engine cap is 10
```

**Fix:** Raise the engine FPS in `emote_get_default_config()`:
```c
.fps = 30,   // or up to 60 — validate against bandwidth (see §2)
```

---

### 1.3 🟡 MODERATE — Partial Buffer is Very Small (1.6% of screen)

```c
.buf_pixels = (size_t)s_lcd_width * 16,
// = 466 * 16 = 7,456 pixels = 14,912 bytes per buffer
// Full screen = 217,156 pixels = 434,312 bytes
// Coverage = 7,456 / 217,156 = 3.4%  (each buffer)
```

With double buffering ON, LVGL/GFX must divide the 466×466 screen into ~29 flush
strips per frame. Each strip is a separate SPI transaction with command overhead.
At 10 FPS this is 290 transactions/second just for display. At 30 FPS it would be
870 transactions/second — likely unsustainable.

**Fix options (pick based on available PSRAM):**
| Buffer size | Pixels | Bytes (×2 for double) | Strips/frame | Notes |
|-------------|--------|----------------------|--------------|-------|
| Current: `w×16` | 7,456 | ~30 KB | 29 | Too many strips |
| `w×60` | 27,960 | ~112 KB | ~8 strips | Good balance |
| `w×120` | 55,920 | ~224 KB | ~4 strips | Better |
| `w×v_res/4` | 54,289 | ~218 KB | 4 strips | Quarter-screen |
| Full frame | 217,156 | ~868 KB | 1 strip | Best quality, needs PSRAM |

**Recommendation:** `w × 120` or `w × v_res / 4` — keeps both buffers under 250 KB
total from PSRAM, dramatically reducing transaction overhead.

```c
// In emote_get_default_config():
.buf_pixels = (size_t)s_lcd_width * (s_lcd_height / 4),  // quarter-screen strips
.buff_spiram = true,   // keep in PSRAM (DMA-capable on S3)
```

---

### 1.4 🟡 MODERATE — rwave_task Runs at Only 20 Hz

```c
#define RWAVE_DRIVER_HZ  20
#define RWAVE_DRIVER_MS  (1000 / RWAVE_DRIVER_HZ)   // = 50 ms
```

This task polls amplitude and calls `gfx_anim_set_segment()`. The GFX engine
renders independently, so 20 Hz here is OK for amplitude tracking. However the
`vTaskDelay(pdMS_TO_TICKS(50))` granularity depends on `FREERTOS_HZ=100` (10 ms
tick), so 50 ms is exact.

For state transitions (idle/listen/speak/think), this 50 ms latency is perceptible.
Raising to 30 Hz (`RWAVE_DRIVER_MS = 33`) keeps the tick-aligned delay reasonable.

---

### 1.5 🟢 GOOD — Already Configured Correctly

- ✅ **Double buffering enabled**: `double_buffer = true`
- ✅ **DMA enabled**: `buff_dma = true`
- ✅ **CPU at 240 MHz**: `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240`
- ✅ **PSRAM Octal 80 MHz**: `CONFIG_SPIRAM_MODE_OCT=y`, `CONFIG_SPIRAM_SPEED_80M=y`
- ✅ **PSRAM DMA capable**: `CONFIG_SOC_PSRAM_DMA_CAPABLE=y`
- ✅ **GDMA in IRAM**: `CONFIG_GDMA_CTRL_FUNC_IN_IRAM=y`, `CONFIG_GDMA_ISR_HANDLER_IN_IRAM=y`
- ✅ **GFX render task has DMA-capable stack**: `task_stack_in_ext` conditional
- ✅ **Ease-path amplitude lerp in rwave_task**: `RWAVE_LERP = 0.30f` smooths abrupt jumps

---

## 2. Bandwidth Budget (QSPI Reality Check)

The CO5300 uses QSPI (4-bit data bus). Maximum practical throughput:

```
QSPI clock: ~80 MHz (limited by CO5300 spec and board trace quality)
QSPI effective bandwidth: 80 MHz × 4 bits = 320 Mbps = 40 MB/s
Frame size (RGB565): 466 × 466 × 2 bytes = ~434 KB per frame

Maximum theoretical FPS = 40,000 KB/s ÷ 434 KB/frame ≈ 92 FPS
```

**Reality** (add SPI command overhead, PSRAM bus contention, CPU rendering time):
- **30 FPS is very achievable** for the EAF baked animations (decode is cheap)
- **45–60 FPS may be achievable** with optimized buffer sizes and `-O2` compiler
- Exceeding 60 FPS is unnecessary (AMOLED panels typically run at 60 Hz VSync)

**Safe target: 30 FPS** — double the current 10 FPS with headroom to spare.

---

## 3. Recommended Changes (Prioritized)

### Priority 1 — Compiler Optimization (biggest win, zero risk)

**File:** `esp-claw/application/edge_agent/sdkconfig`

```diff
-CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
-CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
-CONFIG_COMPILER_OPTIMIZATION_DEFAULT=y
+CONFIG_COMPILER_OPTIMIZATION_PERF=y
+CONFIG_COMPILER_OPTIMIZATION_LEVEL_PERF=y
```

Or run: `idf.py menuconfig` → Compiler options → Optimization Level → **Performance (-O2)**

> ⚠️ Note: Debug builds with `-O0` are 3–5× slower for pixel math. Switching to
> `-O2` alone may fix most of the perceived choppiness without any other changes.
> After this change, re-flash and evaluate before making further changes.

---

### Priority 2 — Raise Engine FPS Cap

**File:** `esp-claw/components/common/emote/emote.c`

```diff
-            .fps = 10,
+            .fps = 30,
```

This unlocks the `gfx_render_loop_task` to present up to 30 frames per second.
The EAF animation FPS (set in `gfx_anim_set_segment`) is the *animation* playback
rate; the engine FPS is the *display* refresh rate. They are decoupled — animation
FPS should be ≤ engine FPS.

After this change, also update animation FPS constants in `reactive_face.c` to
actually reach their targets:
```diff
-#define RWAVE_FPS_IDLE      20
-#define RWAVE_FPS_REACTIVE  24
-#define RWAVE_FPS_THINK     24
+#define RWAVE_FPS_IDLE      24
+#define RWAVE_FPS_REACTIVE  30
+#define RWAVE_FPS_THINK     30
```

---

### Priority 3 — Increase Buffer Size

**File:** `esp-claw/components/common/emote/emote.c`

```diff
-            .buf_pixels = (size_t)s_lcd_width * 16,
+            .buf_pixels = (size_t)s_lcd_width * (s_lcd_height / 4),
```

This gives each buffer ~54,000 pixels (~108 KB at 16bpp), reducing the number of
SPI flush transactions from ~29 strips to ~4 strips per frame — a 7× reduction in
transaction overhead.

**PSRAM usage check:** 2 buffers × 108 KB = ~216 KB from the 8 MB PSRAM pool.
This is 2.7% of available PSRAM. Completely safe.

---

### Priority 4 — Animation Polish (rwave_task tuning)

**File:** `firmware/emote/reactive_face.c`

**4a. Faster amplitude poll rate:**
```diff
-#define RWAVE_DRIVER_HZ  20
+#define RWAVE_DRIVER_HZ  30
```

**4b. Softer lerp for smoother attack/decay:**
```diff
-#define RWAVE_LERP  0.30f
+#define RWAVE_LERP  0.18f   // Slower attack = smoother waveform growth
```

A lerp of 0.18 means the displayed amplitude reaches 86% of the target in ~10 ticks
(~333 ms at 30 Hz). The visual difference is a smoother, less "snappy" waveform
that feels more fluid on AMOLED.

**4c. Coarse amplitude buckets:**
Use 8 buckets and a 4+ frame loop window for LISTENING/SPEAKING. This reduces
`gfx_anim_set_segment()` churn when the RMS value jitters, while still giving
clear low/medium/high visual levels.

**4d. Idle clip plays at 1/3rd speed vs speaking — consider a lower idle FPS:**
The idle breathing animation at 24 FPS on a 30-FPS engine is fine. If the idle EAF
has fewer than 30 frames, LVGL/GFX will interpolate by repeating frames, which may
look stuttery. Consider generating idle EAFs with at least 30 frames for the full
breathing cycle.

---

### Priority 5 (Optional) — FreeRTOS Tick Rate

**File:** sdkconfig (via menuconfig)

```
CONFIG_FREERTOS_HZ=1000   # 1ms tick resolution
```

Current 100 Hz (10 ms) tick means `vTaskDelay(pdMS_TO_TICKS(33))` rounds to 30 ms
or 40 ms depending on alignment. At 1000 Hz ticks, 33 ms is exact.

> ⚠️ Trade-off: Higher tick rate increases scheduler overhead slightly. On a loaded
> system with WiFi + audio + display, this can cause priority inversions. Only enable
> if animation jitter persists after the above changes. 500 Hz is a good middle ground.

---

## 4. Animation Design Best Practices for this Hardware

### 4.1 EAF Asset Guidelines
- **Target frame rate for baked clips**: Design EAF clips at **30 FPS** so every
  engine frame maps 1:1 to an animation frame with no stuttering.
- **Ideal clip lengths**:
  - Idle breathing: 60–90 frames (2–3 second cycle at 30 fps)
  - Listen/Speak reactive: 30 frames (1 second ramp from flat to peak amplitude)
  - Think: 60 frames (slow pulsing loop)
- **Avoid large frame size jumps**: Sudden amplitude transitions (e.g. 0→1000 in one
  tick) cause jarring visual snaps. The `RWAVE_LERP` easing handles this at the
  segment-end level, but the EAF clip itself should also have gentle ramp-in/ramp-out.
- **Current reactive-face bake**: `firmware/mascot/gen_reactive_face.py` now emits a
  restrained premium face language: amber idle, cyan listening, violet thinking,
  amber speaking. The generator avoids random sparkle particles, dense arc stacks,
  and per-frame shimmer; motion comes from slow halos, stable segmented rings, and
  a smooth lower waveform.

### 4.2 QSPI Display Tips
- The CO5300 supports a "partial window" update mode via `SET_COLUMN_ADDRESS` /
  `SET_ROW_ADDRESS` commands. The esp_emote_gfx flush_cb uses this for dirty-region
  updates. Keep rendered objects at the same position (don't animate x/y position)
  to maximize the dirty-rectangle efficiency.
- Colors on AMOLED look most vibrant with **high-saturation mid-tone values**.
  Pure white (#FFFFFF) and very bright colors look harsh. The existing
  `CONFIG_EMOTE_DEF_BG_COLOR=0x171617` (very dark grey, not pure black) is ideal —
  pure black is indistinguishable from OLED-off pixels and makes the display feel
  washed at the edges.

### 4.3 Thread Safety
- The `emote_lock` / `emote_unlock` mutex serializes all gfx_* calls.
  `rwave_task` correctly wraps `gfx_anim_set_segment` / `gfx_anim_start` inside
  this lock.
- `emote_set_obj_visible` / `emote_set_anim_visible` acquire their **own** internal
  lock — do NOT call them inside `emote_lock` (causes deadlock, already documented
  in reactive_face.c).
- Never call `gfx_anim_set_src` or `gfx_anim_set_segment` from an ISR or from the
  Gemini session task — always dispatch to `rwave_task` via state flag.

### 4.4 PSRAM Access Patterns
- EAF clip data lives in PSRAM (`MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`). The GFX
  EAF decoder reads sequentially through frames — this is cache-friendly on the S3's
  PSRAM controller.
- Display frame buffers are also in PSRAM (`buff_spiram = true`). The S3 GDMA engine
  can DMA-read directly from PSRAM (confirmed `CONFIG_SOC_PSRAM_DMA_CAPABLE=y`).
  No CPU copy needed.
- If you add new large assets (images, icon sheets), always allocate them with
  `MALLOC_CAP_SPIRAM` to avoid fragmenting the small internal SRAM pool.

---

## 5. Debugging Animation Quality

### 5.1 Measure actual render FPS
Add to `gfx_render_loop_task` or use the existing log:
```c
// In emote.c or a wrapper — count renders per second
static uint32_t s_renders = 0;
static uint32_t s_fps_ts = 0;
s_renders++;
uint32_t now = esp_log_timestamp();
if (now - s_fps_ts >= 1000) {
    ESP_LOGI("fps", "render FPS = %lu", (uint32_t)(s_renders * 1000 / (now - s_fps_ts)));
    s_renders = 0; s_fps_ts = now;
}
```

### 5.2 Check heap fragmentation
```c
// Run periodically in rwave_task:
ESP_LOGI(TAG, "PSRAM free=%u largest=%u internal free=%u",
    heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
    heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
    heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
```

### 5.3 Serial log patterns to watch for
| Log pattern | Meaning | Action |
|-------------|---------|--------|
| `rwave_task skip: reason=4` | EAF clip not loaded | Check SD card / partition |
| `rwave_task: display arbiter NOT owned` | Display taken by another subsystem | Normal during OTA/camera |
| `Failed to allocate frame buffer` | Out of PSRAM | Reduce buf_pixels |
| Repeated `gfx_anim_set_src FAILED` | Invalid EAF pointer | Validate clip load |
| No `reactive waveform face ready` at boot | All 4 clips failed to load | Check emote partition |

---

## 6. Quick-Start Change Sequence

Apply in this order, re-flashing and testing after each step:

```
Step 1: Change compiler to -O2 (menuconfig or sdkconfig edit)
         → Expected: noticeable smoothness improvement immediately

Step 2: Raise engine FPS to 30 in emote.c
         → Expected: animation playback doubles from 10 → 30 fps

Step 3: Increase buf_pixels to (w × h/4) in emote.c
         → Expected: reduced tearing/banding during fast motion

Step 4: Update RWAVE_FPS_* constants in reactive_face.c to match new engine FPS
         → Expected: listen/speak waveforms animate at full 30 fps

Step 5 (optional): Tune RWAVE_LERP and RWAVE_DRIVER_HZ for feel
         → Subjective; dial to taste
```

---

## 7. References

- Waveshare AMOLED-1.75 Wiki: https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75
- Waveshare GitHub: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75
- CO5300 datasheet: see Waveshare wiki downloads section
- esp_emote_gfx engine source: `esp-claw/application/edge_agent/managed_components/espressif2022__esp_emote_gfx/`
  - Render loop: `src/core/gfx_core.c` lines 185–210
  - Display flush + double buffer: `src/core/gfx_disp.c` lines 43–96
  - Animation timer: `src/core/gfx_timer.c`
  - EAF decoder: `src/widget/gfx_anim.c`
- Our animation driver: `firmware/emote/reactive_face.c`
- Our emote init/config: `esp-claw/components/common/emote/emote.c` lines 136–167
- Display UX design: `docs/DISPLAY_UX_PLAN.md`
- Build config: `esp-claw/application/edge_agent/sdkconfig`
