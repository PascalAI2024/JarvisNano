> **Status:** design record, 2026-07-18, with Phase 0–4 mostly **shipped** by
> 2026-07-19 (see `docs/evidence/` and `git log v5`). Do not re-execute Phases
> 0–4 from this file. Remaining product work is Phase 5 (WakeNet + power moods)
> and hardware re-verification. Current handoff: [`NEXT_SESSION.md`](../NEXT_SESSION.md).
>
> Produced by a 49-agent audit of the actual source (7 subsystem audits +
> adversarial verification of every "missing" claim; 9 of 40 were refuted).
> Yardstick: `docs/prototype/jarvisnano-os.html`. Claims below cite `file:line`
> as of 2026-07-18; later commits moved some of those lines.

# JarvisNano: Why the Device Looks Nothing Like the Prototype — and How to Fix It

## Decisions taken (2026-07-18)

- **Phase 0 — COMPLETE.** All items closed; see "Phase 0 results" below.
- **D1 — RESOLVED: kill LVGL, extend the overlay compositor.** Interactive UI
  (choice arcs, radial menu, cards) is built on `apply_*_overlay` +
  `hud_render.c`, not LVGL. Rationale: LVGL links zero objects today
  (`jarvisrobot_v5.map:34465,34528`), its draw buffers do not fit (largest free
  internal DMA block measured 14,336 B; `spi_master` cannot DMA PSRAM here per
  `jr_display.c:46-49`), and the compositor already draws hit-tested UI on
  glass. This deletes an engine, the SVC-07 arbiter, and the shared-`panel_io`
  freeze bug class from the plan. **Do not re-propose the two-engine split in
  `UPGRADE_RESEARCH.md` §4 — it is superseded.**
- Still open: **D3** (DOA — recommend hemisphere lean, no respin), **D4**
  (retire the 5 baked `.eaf` clips? revisit end of Phase 3), **D5** (companion
  app in the choice loop?), **D6** (dual-OTA before the partition table
  calcifies). **D2** is answered by the Phase 0 commit above: land v5.

## DESIGN RULE: the overlay lives in the baked art's negative space

Found 2026-07-19 by user observation ("2 layers of design playing at the same
time... overlapping?") — and they were right.

The baked `rwave_*.eaf` faces are **not blank backdrops**. Each is already a
complete arc-reactor HUD: core, radial spokes, inner ring, outer ring, segment
blocks, bezel ticks. The first overlay drew its own ring at r=150 and its own 48
radial spokes at r=70-128, directly on top of the art's ring and spokes. Two
HUDs in the same visual language, slightly different geometry, beating against
each other.

Worse, it was **duplicating work already done**: the clips are named `rwave_*`
for *reactive wave* and the display diag exposes `applied_bucket` tracking audio
level. The baked faces ARE the amplitude-reactive waveform. FACE-01 was already
satisfied; the overlay added nothing but visual noise.

**The measured negative space** (6 frames averaged across animation phases,
baked art only, via the `/api/display/hud?on=0` toggle — mean AND peak
luminance both zero):

| Band | Width | Reserved for |
|---|---|---|
| **r135-149** | 15 px | thinking comet |
| **r185-194** | 10 px | breathing ring, listen countdown (STATE-02) |
| **r215-239** | 25 px | battery rim, and the choice arcs — which the prototype wanted "hugging the bezel" anyway |

Occupied, do not draw here: core r0-94, inner ring r125-134, outer ring and
segments r155-179, bezel ticks r200-214. The old battery radius (212) sat right
on the bezel ticks.

**The rule: before adding any visual element, measure where the art already
draws and put the new thing in a free band.** Toggle the HUD off, capture
several frames, profile luminance by radius. If no free band fits the element,
that is a signal the element duplicates something the art already provides —
as the waveform did. Re-measure if the baked art is ever changed.

Corollary for decision D4: going fully procedural remains viable —
`hud_render_rows()` renders all five faces and was previewed on the host — but
it is a **downgrade in richness** today. The procedural idle is a thin ring and
a faint core; the baked idle has depth, glow and segments. Retire the clips only
if the flash/PSRAM is needed, not for looks.

## Two features that turned out not to need building (2026-07-19)

Found while working the board. Both would have been real effort spent on
premises the firmware disproves — recorded so nobody starts them.

**STATE-02's listen countdown rim has nothing to count.** The prototype assumes
a 6000 ms windowed listen. This firmware is always-ready listening
(`VOICE_ALWAYS_READY`): `s_listen_idle_deadline_ms` is declared and read by two
diag handlers, but **all six of its assignments in `main.c` set it to 0**, so
`auto_idle_ms` is permanently zero. There is no window. The product question —
should listening be windowed at all? — has to be answered before any rim is
drawn.

**SVC-08's acceptance criterion is already met by the polling it was meant to
replace.** The stated target was "state transition → face change under 30 ms".
The voice pump paces at `vTaskDelay(1 or pdMS_TO_TICKS(20))` with
`CONFIG_FREERTOS_HZ=100` (1 tick = 10 ms) and pushes the face on every iteration
where it changed, so worst case is 20 ms and typical is 10 ms. `present()` is an
atomic mailbox store, so feeding it every loop is nearly free. Build the event
bus for **testability and the caption/status bus**, not for latency —
`JR_CMD_PUBLISH_SNAPSHOT` still lands in a no-op default branch, so the core
cannot notify observers and the display path has no host coverage. That is the
real debt; the latency framing was wrong.

## Internal RAM budget — the real constraint on Phases 2–5 (measured 2026-07-18)

The audit named PSRAM and flash as the hard ceilings. They are real, but the
binding constraint for everything interactive turns out to be **largest
contiguous INTERNAL block**, and it is far tighter than anyone had measured.

Measured on hardware via `/api/cockpit`:

| State | free internal | largest block | Gemini TLS handshake |
|---|---|---|---|
| Baseline (samplers off) | 22,771 B | 13,824 B | ✅ connects |
| Both samplers running | 15,635 B | 10,752 B | ✅ connects, and **reconnects** |
| Both samplers started at boot | 13,803 B | 7,680 B | ❌ `esp-aes: Failed to allocate memory` |

The two sensor samplers cost **7,136 B** of internal RAM (two task stacks +
TCBs). Starting them at boot pushed the largest block to 7,680 B and the Gemini
TLS handshake failed outright — `esp-aes` needs DMA-capable INTERNAL memory,
which PSRAM cannot serve. Voice regressed end to end.

**The handshake threshold sits between 7,680 B and 10,752 B of largest
contiguous block.** Total free is not the predictor; contiguity is.

### RESOLVED 2026-07-19 — the budget is now healthier *with* always-on sensors
### than it used to be *without* them

| Configuration | free internal | largest block | voice |
|---|---|---|---|
| Original baseline, samplers OFF | 22,771 B | 13,824 B | ✅ |
| Original, samplers ON (on demand) | 15,635 B | 10,752 B | ✅ marginal |
| Original, samplers ON at boot | 13,803 B | 7,680 B | ❌ broken |
| **Optimized, samplers ON at boot** | **26,783 B** | **13,824 B** | ✅ **0/3 reconnect failures, deaths=0** |

Four changes got there, in order of value:

1. **PSRAM task stacks for both samplers** (`xTaskCreateWithCaps` with
   `MALLOC_CAP_SPIRAM`, guarded, with an internal fallback). This is the one
   that mattered: it removes the samplers' 7,136 B internal cost outright.
   The pattern was already proven in-tree by `jr_tools`. Tasks created this way
   MUST be torn down with `vTaskDeleteWithCaps` or the stack leaks — both
   components track which path they took. Safe here because neither task ever
   touches flash, so neither runs with the cache disabled.
2. **Recovering the WiFi/LWIP tuning into `sdkconfig.defaults`** — see below.
3. **Right-sizing four stacks from measured high-water marks** via the new
   `/api/diag/tasks`: `gfx_render` 12288→5120 (peak use 1,784 B), `jr_touch`
   8192→5120, `jr_present` 6144→5120, `websocket_task` 8192→6144. Total 13,312 B
   reclaimed, every one left with >2.6 KB margin.
4. **Raising `httpd` 6144→8192.** Its measured slack was 1,224 B — the thinnest
   in the build. Some of the reclaim is deliberately spent buying safety back.

**`jr_voice` was deliberately NOT reduced, and that is the instructive part.**
A first pass measured its peak at 15,236 B and cut 20480→17408. Three
disarm/rearm reconnect cycles then drove min-ever-free to **1,412 B** — the
reconnect path is deeper than any steady voice turn. 8% headroom on the task
owning TLS, JSON and tool dispatch is not worth 3 KB, so it was restored to
20480 (~4.5 KB margin, measured 5,148 B after the same churn).

**The lesson, which applies to every future stack change here: right-size from
adversarial exercise, never from one happy-path run.** Reconnect churn, tool
calls and error paths are where stacks actually peak. `/api/diag/tasks` reports
min-ever-free per task; read it *after* abusing the device, not before.

### The tuning was living in a gitignored file

`CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=10`, `CONFIG_ESP_WIFI_RX_BA_WIN=6` and
`CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS=4` had been tuned by hand directly in
`sdkconfig`, which is **gitignored**. A single `idf.py set-target` reverted them
to Kconfig defaults, cost ~16 KB of internal RAM (22,771 → 6,467 free; 13,824 →
2,432 largest) and broke voice — with no diff to show for it. They now live in
tracked `sdkconfig.defaults` and survive a full reconfigure. Anything tuned for
memory belongs there, never in `sdkconfig` alone.

Consequence for the plan: **Phase 3 is no longer gated.** Always-on sensors are
viable, so GEST-01..06, the tilt UI and PWR-06's battery arc can proceed.
Remaining caution: after a voice turn the largest block settles at ~8,704 B,
which is above the ~7,680 B failure point but not by much — the handshake itself
occurs from the higher steady state, and 3/3 reconnects passed. Re-check
`/api/diag/tasks` before adding anything else that holds internal memory.

## SECURITY — Google API key is logged in plaintext (found 2026-07-18)

The key is correctly stored in NVS and is **not** hardcoded in source or present
in any committed file. But `main.c:4513` builds the WS URL as
`"%s?key=%s"`, and on any TLS failure `esp_websocket_client` / `transport_ws`
logs the entire request line:

```
E (14889) transport_ws: Error write Upgrade header
  GET /ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=AIza... HTTP/1.1
```

That reaches the serial console **and** the SD logger, which appends to
`/sdcard/logs/jarvis.log` and is retrievable over HTTP (`/api/logs`). Anyone on
the LAN can therefore read the key. It also means any shared serial capture
leaks it — the Phase 1 boot logs in the scratchpad did, which is why they are
NOT in `docs/evidence/`.

Actions:
1. **Rotate the key** — it has been written to device logs. STILL OPEN (owner:
   Pascal); old SD-card log lines from before the fix below may retain it.
2. ~~Firmware fix~~ **FIXED 2026-07-19 (commit a88a1941):** the key now rides
   an `x-goog-api-key` header on the WS upgrade and the URL is bare, so the
   logged URI carries no secret. Empirically verified on hardware: the Live
   endpoint accepted the header (setup completed, 2/2 reconnects, spoken
   reply). A two-strike runtime fallback to the `?key=` URL protects voice if
   the endpoint ever stops honoring header auth (fallback trades privacy for
   availability and logs loudly when it fires).
3. Serial/SD captures from BEFORE the fix remain secret-bearing — redact
   before sharing. `scripts/check-secrets.sh` does not currently catch
   `AIza…` in `.build_logs/` — add that pattern.

## Phase 0 results (2026-07-18)

Executed in full. Three of the seven planned items turned out to rest on **wrong
premises** — recorded here so they are not "fixed" later by someone reading the
original Phase 0 list.

| Item | Outcome |
|---|---|
| Commit the working tree | Done — 9 commits pushed to `origin/v5`; `git status` clean. |
| **v5 boot log** | **Done, and it is the headline result: v5 boots CLEAN.** `docs/evidence/20260718-v5-boot.log`. PSRAM 8 MB, SD mounted, CST9217 touch IRQ-driven, CO5300 466×466 @ 24 fps on 12-row internal DMA strips, emote assets mounted (3,929,405 / 6,619,121 B), faces 0/1/2 rendering, Wi-Fi up, ES7210 MIC1+MIC2+MIC3, audio adc+dac+aec, Gemini Live TLS → handshake → Listening, VAD firing. **D2 (land v5) is now evidence-backed, not a bet.** |
| Wire `hud_render.c` into SRCS | Done — `components/jr_display/CMakeLists.txt`. Compiles clean in the device build (object emitted); currently linker-GC'd since nothing calls it yet, so image size is unchanged. It will link when Phase 2.4 wires it. |
| Host target for `hud_render.c` | Done — but **not** in `host/CMakeLists.txt` as originally planned. That harness enforces "NO path to jr_hal, jr_audio, or jr_display" as an architectural invariant; adding it there would break it. Instead: standalone `components/jr_display/tests/`, matching `jr_memory/tests` and `jr_tools/host`. 7 tests, clean under `-Wall -Wextra -Werror`. The load-bearing one is **strip invariance** — whole-frame render must equal the concatenation of 12-row strips, including the ragged 10-row tail (466 = 38·12 + 10). That is the seam-on-glass bug class. |
| ~~Partition-label mismatch~~ | **FALSE FINDING — do not "fix" this.** There is no mismatch. `sdkconfig.defaults:28` selects `partitions_16MB.csv`; its line 18 declares label `emote_assets`; `jr_display.c:35` mounts `"emote_assets"`. `/emote` (`jr_display.c:34`) is the *mount point*, a different thing — the original audit conflated the two. The booted table and a successful mount confirm it. `bootstrap.sh` writes a table for the legacy esp-claw tree, which the v5 build never uses. |
| Delete dead-code false signals | Done, after verifying each had zero production callers. Removed: `jr_transport.c` + its header (nothing included it; the real `jr_realtime_voice_client_t` is `gemini_live.c` + `gemini_device_ws.c`); the `jr_vad_*` stub (superseded — the real adaptive VAD with floor tracking runs in `main.c`, visible in the boot log as `vad: speech start rms=633.3 floor=40.0`); `jr_dsp_resample_linear` (superseded by `downsample_24to16_4lane()` at `jr_audio.c:323`, which must preserve the 4-lane TDM interleave — a generic resampler cannot). Host tests 90 → **88**: exactly the two that asserted stub behaviour. `jr_dsp_rms` was KEPT — it has real production callers. |
| ~~`hal.c` "Phase 3 unstarted" comment~~ | Comment corrected, **code kept**. The `jr_hal` display stub is not dead: it is the live headless fallback when the real presenter fails to start (`main.c:4409-4414`). Only the comment was stale. |

**Also fixed, found in passing:** `scripts/jarvis-desk.py` hardcoded the device
LAN IP as `DEFAULT_DEVICE_HOST`, which `CLAUDE.md` bans. Now defaults to mDNS,
overridable via the `JARVIS_DEVICE_HOST` env var `resolve_token` already
honoured; tests use RFC 5737 TEST-NET-1.

**Structural finding — why evidence never survived:** `.build_logs/` is
gitignored (`.gitignore:8`). Every device artifact ever captured there was
invisible to git and CI. That is the mechanical reason this project reached
2026-07-18 with no in-repo proof the image booted, *while it booted fine*.
Evidence now goes in `docs/evidence/` (tracked). See its README.

**Two open issues visible in the boot log**, neither blocking:
`sta disconnected (reason=2/205)` retries before association, and
`transport_ws: Error transport_poll_write(0)` at ~23 s.

---

# QUESTION 1 — The Diagnosis

## The short version

You have not gotten near the prototype because **the project has been restarted three times and has never once shipped a UI foundation.** Every restart rebuilt the *plumbing* (transport, state machine, audio, hexagon) and every restart deferred the *presentation layer*. The prototype is 100% presentation. So the gap is not "some features are missing" — it is that **the entire axis the prototype lives on has never been started**, on any branch, in any version.

The evidence for this is blunt: after five weeks and three architectures, the device can display **exactly five baked animation clips** (`components/jr_ports/include/jr_ports/display.h:23-29` — `IDLE/LISTENING/THINKING/SPEAKING/ERROR`) selected by a 32-bit mailbox. The prototype specifies **four moods × seven scenes + five transitions + eight gestures**. You are at roughly 5 of ~60 visual states, and the 5 you have are the ones that fall out of the voice pipeline for free.

Ranked by explanatory power:

---

## Root cause #1 — Serial rewrites, not branch fragmentation (SOFT) — explains ~50% of the gap

**The premise "branches are fragmented" is false.** I verified the topology directly:

```
git rev-list --left-right --count main...v5   →  0    7   (main ⊂ v5)
git rev-list --left-right --count v5...grok-hud →  9    0   (grok-hud ⊂ v5)
```

`grok-hud`, `origin/gpt`, `origin/feat/native-24khz-output` are **all already ancestors of v5**. Nothing is stranded on a branch. The Orbital Cockpit HUD you think is "on grok-hud" is sitting in your working tree right now at `firmware/ui_layer/ui_layer.c:589-647`.

The real problem is worse and more specific: **the repo contains two complete, non-overlapping firmware stacks, and the one that builds is the one with no UX.**

| | legacy `firmware/` (esp-claw) | v5 `components/jr_*` |
|---|---|---|
| Lines C/H | 13,824 | 19,580 |
| Orbital HUD, arcs, radial menu | ✅ `firmware/ui_layer/ui_layer.c:312-340, 606-625` | ❌ |
| IMU driver + power moods | ✅ `firmware/components/jarvis_imu/`, `firmware/main/touch_demo.c:287-311` | ❌ |
| Device-proven voice | ✅ `cap_gemini_live` | rewritten, unproven on HW |
| **Is it compiled by the v5 build?** | **NO** | YES |

Root `CMakeLists.txt:5` states the decision explicitly: *"no esp-claw, no bootstrap.sh heredocs, no vendored-tree patching."* The v5 project auto-discovers only `components/` and `main/`. It imports **one** legacy file (`jarvis_board.c`, `components/jr_display/CMakeLists.txt:9-11`) and five `.eaf` assets. Everything else that was ever proven on glass — the HUD, the arcs, the IMU, the gestures, the power moods — is **architecturally stranded, not branch-stranded**.

That distinction matters enormously for the plan: you cannot fix this with `git merge`. There is nothing to merge. You deleted the UX by changing the build system.

**Each rewrite paid the plumbing tax again and never reached the UI.** v4 got to a HUD. v5 threw the HUD away, rebuilt the hexagon to a genuinely higher standard (90 host tests, 3000-turn self-heal soak, `host/test_soak.c:752`), and arrived back at five faces.

---

## Root cause #2 — ~27,000 lines uncommitted for 10 days (SOFT) — explains ~15%, and is an existential risk

Last commit: `5ec731bb`, **2026-07-08**. Today is **2026-07-18**.

Verified working-tree state:

- `main/main.c`: HEAD = 569 lines → worktree = **4,562 lines** (+3,993 uncommitted)
- `components/jr_net/src/jr_net.c`: 159 → 1,182
- `components/jr_display/src/jr_display.c`: 17 → 1,458
- **Untracked entire components**: `components/jr_http/` (1,146 lines), `components/jr_memory/` (2,577), `components/jr_tools/` (2,576)
- **Untracked**: `components/jr_display/src/hud_render.c` (506 lines), `components/jr_hal/src/input_touch.c`, `main/diagnostics.html`, five `scripts/*.py`

`git stash` or `git reset --hard` deletes the majority of v5 permanently. Untracked files survive neither.

Second-order effect: **anyone auditing the v5 branch is auditing a skeleton.** Your own plan docs, your own agents, and any CI are reasoning about a version of the code that does not resemble what flashes. That is a direct cause of the "plans don't match reality" problem.

Third-order: 618 lines of that uncommitted work (`hud_render.c` + header) — **the single most complete arc/ring/waveform primitive in the entire repo, procedural, integer-only, host-compilable** — is referenced by nothing. Not in `components/jr_display/CMakeLists.txt` SRCS. Not `#include`d anywhere. It is the closest thing you have to the prototype's face, and it is dead code that one command away from nonexistence.

---

## Root cause #3 — Missing foundations: there is no UI *layer*, only a face *mailbox* (MISSING FOUNDATIONS) — explains ~25%

This is the part that is genuinely structural, and it is where the plan must start.

**What is actually missing before any prototype feature is possible:**

1. **No state→display event bus.** The face is updated by *polling*: the voice task calls `jr_orch_phase()` every loop tick and maps it to a face (`main/main.c:4306-4321`, `phase_to_face` at `main.c:1236-1249`). There is no observer, no subscribe, no callback. The command designed for exactly this — `JR_CMD_PUBLISH_SNAPSHOT` (`components/jr_core/include/jr_core/session.h:186`) — is **silently dropped into the no-op default branch** at `main/main.c:1226-1229`. Any richer UI has no way to learn a state changed.

2. **The UI port exists but covers ~10% of the UI.** `jr_display_t` is two callbacks, `blank()`/`present(face, amplitude)` (`components/jr_ports/include/jr_ports/display.h:31-44`). Everything else — consent cards, touch challenge, shade, test patterns, snapshots — is called **concretely** from L6 against L5: `jr_display_surface_present` (`main.c:775`), `jr_display_set_test_pattern` (`main.c:742`), `jr_display_surface_dismiss` (`main.c:679`). Declared at `components/jr_display/include/jr_display/jr_display.h:114-149`, **not in `jr_ports`**. So the entire human-facing surface — including the physical consent gate on your only mutating tool — is unmockable and untestable on host.

3. **No IMU in the v5 build at all.** `ls components/` returns no IMU component. `main/CMakeLists.txt` REQUIRES has no IMU entry. A grep for `imu|accel|gyro|tilt|shake|gesture` across `components/ main/` returns zero real hits. **GEST-01 through GEST-06 (double-tap, flip-to-mute, shake, lift-to-wake, no-motion, tilt-to-scroll) have zero code in the shipping build.** The driver exists — `firmware/components/jarvis_imu/` — stranded in the dead tree.

4. **No power policy of any kind.** `esp_pm_configure` appears **zero times** repo-wide. `CONFIG_PM_ENABLE is not set` (`sdkconfig:1347`). No `esp_light_sleep_start`, no `esp_deep_sleep_start`, no `esp_sleep_enable_*`. `jr_power_monitor_t` (`components/jr_ports/include/jr_ports/power_monitor.h:19-23`) has **zero implementations and zero consumers**. The AXP2101 is deliberately `init_skip: true` and `jarvis_pmic` is documented read-only — it writes exactly two enable bits and **never touches a rail register in 0x80-0x99**. Panel brightness is called **once, hardcoded to 100**, at `firmware/components/jarvis_board/src/jarvis_board.c:125`, with no runtime setter anywhere.

   The `power_mood_t` that exists has **two** values, not four (`firmware/main/touch_demo.h:17-19`), lives in the dead tree, and DREAM changes **no power state at all** — it stops Gemini, dims a face, sets a flag (`touch_demo.c:287-296`). PWR-01..06 is ~5% built.

5. **No touch→session path.** `JR_EV_TAP` is handled in all 10 states (`session.c:339,370,403,485,571,642,674,694,715`) and exercised in the soak — but **the composition root never injects it.** The tap-to-accelerate-retry recovery you proved in `test_soak.c` is unreachable on hardware.

6. **No `Ask` state.** `jr_state_t` is 10 configurations (`session.h:42-54`); none is Ask/Awaiting/Choice. The centerpiece prototype feature (STATE-05/06, choice arcs) has no state to park a turn in — and if you park in `Thinking`, `JR_NOREPLY_MS = 20000` (`session.h:261`) tears down the session on any human who deliberates for 20 seconds.

---

## Root cause #4 — Two beliefs that are factually wrong and have cost you real work (SOFT/knowledge) — explains ~10%

**Belief A: "esp_emote_gfx has no canvas, so arcs are undrawable."**
This is a misread of your own doc. `docs/reference/display-emote-gfx.md:9-19` says **`gfx_img_set_src` cannot accept a runtime CPU buffer.** That is all it says. Meanwhile v5 **already draws arbitrary runtime pixels on hardware every frame**: `panel_flush()` at `components/jr_display/src/jr_display.c:851-859` calls `apply_test_pattern()`, `apply_shell_overlay()`, `apply_surface_overlay()` which mutate the outgoing DMA strip *in place* before `esp_lcd_panel_draw_bitmap()` at `:861`.

You have shipped, on glass: a bordered card, a 5×7 bitmap font, tappable buttons with exact hit-testing, three radial quick-action circles, a progress bar, an **8-sector annulus** (`jr_display.c:742-753`), and an integer sector-from-vector primitive with no `atan2` (`jr_display.c:355-369`).

**Choice arcs and radial dial menus are drawable today, on the path you already have.** This is the single most important correction in this document.

**Belief B: "the conversational compass is hardware-blocked — we need a mic array."**
You *have* two live MEMS mics, confirmed empirically by tone test, not assumption: `firmware/components/cap_gemini_live/src/cap_gemini_live.c:180-190` ("lane 1 = live MEMS mic", "lane 3 = live MEMS mic", "lane 2 = MIC4 unconnected/dead"). v5 simply **throws lane 3 away**: `components/jr_audio/src/jr_audio.c:48-50` defines `GL_MIC_LANE 1`/`GL_REF_LANE 0` and `:375-376` demuxes only those two. FACE-02 is software work, not a board respin. (Caveat below.)

---

## Root cause #5 — Genuine hard constraints (HARD) — explains ~10%

These are real and they *do* bound the design. But note they are last, not first — they are not why you are here.

| Constraint | Evidence | What it actually forbids |
|---|---|---|
| **PSRAM residency** | `jr_display.c:103-107` — all clips resident, never freed; 5 clips = 3.72 MiB of ~7.4 MiB. Enumerated PSRAM commitment already **5.49 MiB**. | A 4-mood × 4-state baked matrix (16 clips ≈ 12-15 MiB) is **impossible**. Hard stop. |
| **Flash emote partition** | 6.875 MiB, table ends flush at `0x1000000` (`partitions_16MB.csv:15-20`). Measured **34.7 KiB/frame** at 466×466 RLE. | 16 baked clips ≈ 2× the whole partition. Also hard stop. |
| **Internal SRAM for DMA** | `jr_display.c:46-49` — PSRAM strips fail; `spi_master` can't DMA unaligned external RAM. Live: internal_free 47,935 B, **largest block 14,336 B**. | A full LVGL double buffer (2×434 KB) is impossible; even a 1/10-screen pair (2×43.8 KB) exceeds the largest free internal block. **LVGL cannot run the way the prototype assumes.** |
| **QSPI bandwidth** | Panel already at ~23 fps against a 24 fps target (`jr_display.c:42-45`). Overlays iterate every pixel of every strip with no dirty-region culling (`jr_display.c:625-673`). | Rich per-frame overlay without bounding-box early-outs will drop frames. |
| **AXP2101 rails locked** | C-board `board_devices.yaml` keeps `init_skip: true`; runtime reads ADC/fuel/PKEY only. | Keep factory rail sequencing until current and rail ownership are measured. |
| **AXP2101 PKEY** | C revision removed TCA9554; short/long PKEY latches are polled directly at AXP2101 `0x34` and are live. | Soft power gestures are available; PMIC hard-cut behavior remains factory-owned. |
| **WakeNet9 active** | `jr_wake` is live and feeds from the always-open codec path. | WHISPER/DREAM can park Gemini, but CPU/light sleep still needs a proven wake-source/current budget. |
| **DOA angular resolution** | Mic baseline remains too small for a trustworthy continuous angle. | Use coarse directional expression only after physical calibration. |

---

## The uncomfortable summary

> The project is not behind because the hardware is hard. It is behind because **three consecutive architectures each optimized the layer the user cannot see**, and the one artifact that would have forced the issue — a visible pixel that isn't a face — was never a phase gate. The v5 hexagon is genuinely excellent engineering (90/90 tests, a 3000-turn adversarial soak with `permanent_silence=0`). It is also the third consecutive time you built a beautiful skeleton and stopped before the skin. And right now the skeleton itself is 10 days uncommitted, with its most UI-relevant file untracked and unbuilt.

One more thing, stated plainly: **there is no in-repo evidence the v5 image has ever booted.** `build/jarvisrobot_v5.bin` is dated Jul 11 22:10. The newest device artifact in `.build_logs/live-device/` is `20260709-190753-screen.png` — Jul 9. `grep -rl 'jarvis_v5' .build_logs/live-device/` returns nothing. Code comments claim live measurement (`main.c:330-331`, `:4354`, `:4494`), and I believe them — but comments are not artifacts. "v5 links" and "v5 boots" are different claims and only the first is evidenced.

---

# QUESTION 2 — The Plan

## Governing principles

1. **Commit first, always.** Nothing else matters if the tree evaporates.
2. **Every phase ends with a photograph of the device.** Not a test suite. Not a design doc. A picture of the screen doing something new. This is the antidote to the failure mode above.
3. **Extend the overlay compositor. Do not adopt LVGL.** (Decision D1 below — this is the biggest single call.)
4. **Do not port the legacy tree wholesale.** Port *primitives*, reimplement *policy*.
5. **No new architecture until Phase 4.** Every foundation below is <300 lines added to an existing file.

---

## Phase 0 — Stop the bleeding (½ day, do this before reading further)

**Goal:** the work exists in git. No new features.

**Tasks:**

1. `git add -A` and commit the working tree in 4-6 logical commits on `v5`. Do **not** rebase, do **not** clean, do **not** stash. Order: (a) untracked components `jr_http`/`jr_memory`/`jr_tools`, (b) `hud_render.{c,h}` + `input_touch.c`, (c) `main/main.c` + `main/CMakeLists.txt`, (d) `components/jr_*` modifications, (e) scripts + docs + android.
2. `git push origin v5` immediately after each commit.
3. Add `components/jr_display/src/hud_render.c` to that component's `CMakeLists.txt` SRCS — even if nothing calls it yet. An unbuilt file rots.
4. Add a host target for `hud_render.c` in `host/CMakeLists.txt`. Its header already claims host-compilability (`hud_render.h:12-14`); prove it.
5. **Flash the Jul 11 image and capture a serial boot log** into `.build_logs/live-device/`. This is the missing evidence.
6. Fix the partition-label landmine: root `partitions_16MB.csv:18` says `emote_assets`; `bootstrap.sh` writes `emote`; `jr_display.c:35` mounts `"emote_assets"`. Offsets match byte-for-byte so the failure is silent — a blank face. Delete or rename one.
7. Delete the three known false-signal artifacts: the Phase-0 stubs `jr_vad_update`/`jr_dsp_resample_linear` (`components/jr_dsp/src/dsp.c:36-50`, no production callers, 2 of your 90 tests assert dead code), the 16-line TODO shell `components/jr_transport/src/jr_transport.c`, and the stale "Phase 3 unstarted" comment at `components/jr_hal/src/hal.c:49-50`.

**Acceptance (hardware):** `git status --porcelain` is empty. A serial log in the repo shows `jarvisrobot_v5` booting and rendering a face. `host/` builds `hud_render.c` and passes.

**Effort:** 4 hours.

---

## Phase 1 — The consolidation decision, executed (1-2 days)

**Goal:** one tree, one build, and an explicit written kill of the other. No feature work.

### Exactly what to take from where

| Take | From | To | Why |
|---|---|---|---|
| QMI8658 driver (286 lines) | `firmware/components/jarvis_imu/` | **new** `components/jr_imu/` | Only IMU code that works. Rewrite `jarvis_imu_read()`'s blocking 40-70 ms burst (`jarvis_imu.c:245-251`) into a task+queue — it must not stall the voice pump. |
| AXP2101 telemetry | `firmware/components/jarvis_pmic/` | **new** `components/jr_power/` | Battery arc (PWR-06) needs it. Read-only is fine for now. |
| Arc/wedge **math only** | `firmware/ui_layer/ui_layer.c:312-340` | `components/jr_display/src/hud_render.c` | Take the geometry, **not** `display_hal_*` (esp-claw-only) and **not** the scene code. |
| `hud_render.c` primitives | already in tree, untracked | wire into `apply_*_overlay` | Ring bands, 48 waveform bars, orbiting comets, boot bloom — this is FACE-01/03, TRANS-01 already written. |
| Nothing else | — | — | — |

### Kill list

- **Delete or archive** `firmware/ui_layer/`, `firmware/main/touch_demo.c`, `firmware/http_server/`, and the 895 MB vendored `esp-claw/` tree (git-rm, keep the tag).
- **Delete** `scripts/bootstrap.sh`'s relevance: mark it dead in `CLAUDE.md`. Right now `CLAUDE.md` documents a build one-liner that **does not build the branch you are on** — that is an active source of wasted agent cycles.
- **Rewrite `CLAUDE.md`** to describe the v5 plain-IDF build (`idf.py set-target esp32s3 && idf.py build`, `scripts/flash-v5.sh`).
- **Write `docs/V5.md`** — the doc that does not exist. Scope, done-criteria, what v5 is. A grep for `v5|hexagonal|jr_core` across `plan.md`, `docs/ROADMAP.md`, `docs/NEXT_SESSION.md`, `docs/UPGRADE_RESEARCH.md` matches **nothing relevant**. That absence is why every session re-litigates.

**Acceptance (hardware):** device flashes from a tree with no `esp-claw/`, no `firmware/ui_layer/`. `jr_imu` reports a live accel reading over the diag HTTP endpoint. Battery percent appears in the diag JSON from `jr_power`.

**Effort:** 1.5 days. **Deliverable photo:** unchanged screen — but a curl showing live IMU + battery from the v5 image.

---

## Phase 2 — Foundations, each ending in a visible pixel (4-5 days)

This is the phase you have skipped three times. It is **four small things**, and each one is independently visible.

### 2.1 — The event bus (½ day) → *visible: face reacts instantly, not on poll*

- Implement `JR_CMD_PUBLISH_SNAPSHOT` instead of discarding it at `main/main.c:1226-1229`. Route it to a `jr_ui_event_t` queue.
- Add `jr_ui_publish(evt)` in `jr_ports` — one function, one queue, drop-oldest.
- Move the face update off the `jr_orch_phase()` poll (`main.c:4306`) onto the bus.

**Acceptance:** state transition → face change latency measured <30 ms on device, logged.

### 2.2 — Widen the UI port (½ day) → *visible: nothing new, but consent card becomes host-testable*

- Move the rich surface API (`jr_display.h:114-149`) into `components/jr_ports/include/jr_ports/ui.h` as function pointers on an extended `jr_display_t`.
- Add a fake in `host/fakes/` and a first host test for the **consent flow** — currently your only physical security gate has zero automated coverage.

**Acceptance:** `host/` test count goes 90 → 95+, including one asserting that a `remember` toolCall presents a 2-action surface and a tap on index 1 produces a `functionResponse`.

### 2.3 — Touch → session (½ day) → *visible: tapping a sleeping/backoff device visibly accelerates reconnect*

- Inject `JR_EV_TAP` from `main.c`'s touch poll (~`main.c:4095`). It is handled in all 10 states already and proven in soak; the wire is simply absent.

**Acceptance:** force a Backoff state (kill the AP), tap the screen, observe an immediate retry in the serial log.

### 2.4 — The arc render surface (2-3 days) → *visible: THE FIRST PROTOTYPE PIXEL*

This is the phase's payoff. Build **STATE-03, the thinking orbital spinner** — deliberately the cheapest prototype feature that is unmistakably from the design.

- Add `overlay_arc(cx, cy, r, w, a0, a1, rgb565)` to `hud_render.c`, strip-oriented, integer-only, using the existing sector primitive at `jr_display.c:355-369`.
- **Add row-range early-outs** to `apply_shell_overlay`/`apply_surface_overlay`. Today they iterate every pixel of every strip (`jr_display.c:625-673`) with 4 `surface_text_pixel()` calls per pixel. Compute a bounding box per overlay and skip strips outside it. This is not optional — it is the budget for everything after.
- **Deduplicate the geometry constants.** Draw side `jr_display.c:649-652` and hit-test side `jr_display.c:1445-1453` independently spell `left=70, right=396, gap=8`. Move them to shared `static const`. A layout change today silently produces visible-but-untappable buttons with no compile or test signal.
- Render: dimmed track ring at r=150 (#20242e, α0.6), one cyan dot r=4 orbiting at `angle = now/420` rad, 1.1 rad comet trail at 50% α. Suppress the waveform while THINKING (the prototype returns early — `jarvisnano-os.html:211-219`).

**Acceptance (hardware): a photograph of the device showing the orbital spinner while Gemini is thinking, at ≥20 fps measured via `gfx_timer_get_actual_fps`.**

**Effort:** 4-5 days total. **This is the first phase in the project's history that ends with a new kind of pixel.**

---

## Phase 3 — Cheap wins (1 week) → *the device starts to look like the prototype*

Everything here is 80% there. Ordered by ratio of visual impact to effort.

| Feature | Why cheap | Effort |
|---|---|---|
| **TRANS-05 touch ripple** | Touch events already flow (`input_touch.c`); needs one expanding-ring overlay at the touch point. 220 units/s growth, 1.6/s α decay. Renders above everything, even asleep. | 3h |
| **FACE-01 reactive waveform** | `hud_render.c` **already has 48-bar waveform + ring bands written**. Amplitude bus already exists (`main.c:1294-1301` `rms_to_amp`). Wire and tune to the spec envelope: `len = base + amp*70*env*(0.5+noise*0.8)`, `env = sin(πi/N)^0.6`. | 1d |
| **FACE-03 arc-reactor halo** | Same file, already written (orbiting comets, ring bands). Two rings r=205/196 + 48 particles on a `now/4000` phase. Persistent behind every state. | 4h |
| **TRANS-01/02/03 bloom / iris / settle** | `hud_render.c` has boot bloom + crossfade. Iris is an even-odd fill = one radius comparison per pixel. Durations 900/900/700, easing `easeInOutQuad`. | 1.5d |
| **PWR-06 battery rim arc** | `jr_power` lands in Phase 1; arc primitive lands in 2.4. r=212 w=5, red <20% / cyan charging / green else. | 4h |
| **STATE-02 listen countdown rim** | Arc primitive + a 6000 ms modulo. r=150 w=3 draining from 12 o'clock. | 3h |
| **UI-01 watch face** | Procedural SNTP-backed ticks/hands; the C revision has no PCF85063. | 1.5d |
| **STATE-08 error / no-network** | Red rim shudder + a plain-language line. You already have `error.eaf` and a `FATAL`/`Backoff` state. | 4h |
| **POLISH-06 attract reel** | 20 scripted steps at 2100 ms. Cancels on manual input. Doubles as your **end-to-end visual soak** — carry it over verbatim from `jarvisnano-os.html:481-507`. | 6h |

**Acceptance:** a 60-second video of the attract reel running on the device, walking idle → listening → thinking → speaking → ambient watch → whisper, with ripples on every touch.

---

## Phase 4 — The centerpiece: choice arcs, end to end (1.5 weeks) → EXPENSIVE but highest value

STATE-05/06 is the single feature that makes this a product rather than a face. It needs work at **four** layers, and every one is a real change.

**4.1 — Model vocabulary.** `jr_gemini_fn_decl_t` (`components/jr_transport/include/jr_transport/gemini_live.h:62-67`) carries **exactly one string arg**, and the setup builder hardcodes `"string"` with a single-element `required` array (`gemini_live.c:195-201`). You cannot declare `options[]`. Extend the struct to a small property list with array support, then add an `ask_user(question, options[])` declaration to `s_device_tool_fns` (`main.c:86-127`).

**4.2 — The Ask state.** Add an 11th configuration to `jr_state_t`. Critically: **`JR_NOREPLY_MS = 20000` (`session.h:261`) must not run in Ask** — a human deliberating for 21 seconds currently tears down the session. Use a separate, longer, user-facing timeout (the existing `TOOL_CONSENT_TIMEOUT_MS` pattern at `main.c:761` is the model).

**4.3 — Polar hit-testing.** Not free from any widget toolkit. Normalize the touch angle into [0,τ), test span membership **with wraparound** (`a0<a1 ? a>=a0&&a<=a1 : a>=a0||a<=a1`), and require `dist > 110` so center taps don't answer (`jarvisnano-os.html:411-417`).

**4.4 — Answer → functionResponse.** Today choice taps write a `brain_action_event` into a ring for the **companion app** to poll and then dismiss (`main.c:2393-2415`). Only the CONSENT branch (`main.c:2384-2392`) reaches the tool pipeline, hardcoded to index 0=DENY / 1=ALLOW. Generalize `device_tool_resolve_consent_locked` (`main.c:628-685`) into `device_tool_resolve_choice(call_id, index)`.

**Also raise the text budget.** Title 25 chars / body 49 (split at a hard 24 with `surface_split_body` explicitly refusing word-wrap, `jr_display.c:593-606`) / 3 actions × 13 chars. The `remember` path **rejects the call outright** when text doesn't fit (`main.c:697-713`). Agent-authored questions will hit this constantly. Add word-wrap and raise the caps.

**Acceptance (hardware):** say "ask me whether to reply to Sarah"; the device renders three bezel-hugging arcs with the question in the 1.2 rad gap at 12 o'clock; tapping "Later" fills that arc, captions "You chose: Later", and 900 ms later Gemini speaks a confirmation — proving the `functionResponse` closed the loop.

---

## Phase 5 — Power moods (2 weeks) → EXPENSIVE, gated on a hard prerequisite

Do **not** start this until Phase 4 ships. It is the largest genuinely-new work in the plan and it has a hard dependency you do not control.

**The gate: no wake word = no mood ladder.** AMBIENT, WHISPER and DREAM are all *defined* by wake-word listening. No `CONFIG_SR_WN*` model is compiled in. Until esp-sr WakeNet9s is integrated and motion-gated, every mood below AWAKE can only be exited by touch or motion — which means the headline experience (speak to a sleeping device) is unreachable regardless of how much power plumbing you write.

**Sequence:**
1. esp-sr AFE + WakeNet9s, motion-gated. Note the CPU budget correction: the ES7210 has **no on-chip AEC** — it only digitizes the echo reference on TDM lane 2 through a ~-23.5 dB pad. AEC is software, in esp-sr. `UPGRADE_RESEARCH.md:264` calls it "hardware AEC" and overstates it.
2. IMU on **interrupt**, not poll. The current 400 ms `esp_timer` I2C poll (`touch_demo.c:879-880`) is architecturally incompatible with sleeping through it. Configure QMI8658 No-Motion / Any-Motion / Tap engines on INT2→GPIO21 (RTC-capable, EXT0-wakeable). This also unlocks GEST-01..05 for free.
3. Runtime brightness. `esp_lcd_panel_co5300_set_brightness()` exists (`esp_lcd_co5300.h:88`) and is called exactly once, hardcoded 100. Expose a setter. TRANS-04's eased scalar with the 0.18 α floor is then ~20 lines.
4. AMBIENT (dim + WiFi off) → WHISPER (panel off + breathing pixel) → DREAM (deep sleep, EXT0 on GPIO21).
5. **Measure wake-to-first-word.** DREAM reboots from RTC memory and pays full WiFi association + TLS + Gemini setup on every wake. That cost is measured **nowhere in this repo** and it alone decides whether DREAM is usable. Instrument it before committing to the design.

**Rail control stays out of scope.** The AXP2101 is `init_skip: true` by a documented safety decision. Cutting the ES8311/ES7210 analog rails requires reversing it. Recommend: don't, in this phase.

**Acceptance (hardware):** a bench-measured current table — AWAKE / AMBIENT / WHISPER / DREAM in mA — and a video of the ladder descending on no-motion and blooming back on lift.

---

## Hardware-blocked features and their substitutes

| Feature | Verdict | Substitute |
|---|---|---|
| **FACE-02 conversational compass (DOA)** | **NOT blocked — correct the premise.** Two live mics confirmed (`cap_gemini_live.c:181-186`). But mic baseline is undocumented; at 16 kHz a board-scale baseline gives a handful of samples of delay. | Ship **3-position hemisphere lean** (left / center / right), not a continuous angle. Measure the physical mic spacing first — it is not written down anywhere. Note lane 3 currently gets no independent PGA (`jr_audio.c:268-272` sets MIC1\|MIC2 as one masked pair). |
| **UI-03/04 tilt-to-scroll radial menu** | Continuous IMU polling contends with touch, PMIC, and codecs on the shared 400 kHz I2C bus; the C revision has no RTC/expander. | Keep swipe navigation; move QMI8658 to a proven interrupt path before adding more continuous motion UI. |
| **GEST-08 power button** | **LIVE ON C:** AXP2101 PKEY short/long latches are polled over I2C; short press controls privacy and long press shows status. | Keep PMIC hard power-off factory-owned; do not invent an expander/IRQ path removed on C. |
| **POLISH-05 haptic tick** | **HARDWARE-BLOCKED.** No LRA on the board. | Defer. If you respin: PWM LRA + MOSFET on a spare GPIO, deliberately **off** the I2C bus (a DRV2605L adds load to an already-crowded bus). |
| **4 moods × 4 states baked matrix** | **HARDWARE-BLOCKED.** ~12-15 MiB needed; partition is 6.875 MiB and PSRAM is ~7.4 MiB total with 5.49 already spoken for. | **Procedural, not baked.** `hud_render.c` is the answer and it is already written — integer-only, no assets, no PSRAM residency. Moods become *palette + brightness + amplitude parameters* on one procedural face, not 16 clips. This is the single most important design substitution in the plan. |
| **LVGL for interactive UI** | **HARDWARE-BLOCKED in the form the prototype assumes.** A full double buffer is 2×434 KB; a 1/10-screen pair is 2×43.8 KB; measured largest internal free block is **14,336 B**, and PSRAM can't be DMA'd by `spi_master` here. | See D1. |
| **SVC-05 ambient-light theming** | No ALS on board. | RTC-scheduled time-of-day palette only. An I2C ALS adds load to a bus already carrying touch, PMIC, IMU, RTC and expander. |

---

## Decisions you must make

**D1 — LVGL vs. the overlay compositor. (Answer: overlay compositor. Decide now; everything sequences off it.)**

The prototype's footer and `UPGRADE_RESEARCH.md:335-341` prescribe a two-engine split with LVGL at 200-300 fps. That plan is not viable on this board:
- LVGL needs draw buffers. Internal SRAM largest free block is 14,336 B; PSRAM buffers can't be DMA'd (`jr_display.c:46-49`, verified live).
- It would have to run at the same 12-row strip granularity as `esp_emote_gfx` and **share `panel_io`** — and your own memory already records a "freeze-after-UI (shared panel_io callback clobber)" bug class on exactly that path.
- LVGL is vendored and compiled but **zero objects link** (`jarvisrobot_v5.map:34465,34528` — LOAD lines only). Nothing is lost by dropping it.
- Meanwhile the overlay compositor **already draws interactive, hit-tested UI on hardware.**

Recommendation: **kill LVGL, delete the dependency, extend `apply_*_overlay` + `hud_render.c`.** This deletes an entire engine, the arbiter (SVC-07), and the buffer-contention bug class from the plan. It is the largest scope reduction available and it costs you nothing you actually have.

**D2 — Land or abandon v5. (Answer: land it.)** The hexagon is the best code in the repo and the branch is a strict superset of everything else. Abandoning it would be restart #4 and would repeat root cause #1 exactly. But land it *conditionally*: Phase 0's serial boot log is the gate. If the Jul 11 image does not boot, fix that before anything in Phase 1.

**D3 — Is DOA worth a hardware change? (Answer: no.)** You have two mics. Measure the baseline, ship hemisphere lean, evaluate. A respin for a third mic before the software estimator exists is speculative.

**D4 — Baked assets vs. procedural rendering.** Following from D1 and the flash/PSRAM ceilings: recommend **procedural for everything new** (arcs, waveform, halo, transitions, watch), keeping the 5 existing `.eaf` clips only as the idle/listen/think/speak base layer — or retiring them too, which would free 3.72 MiB of PSRAM and 3.6 MiB of flash. Worth explicitly evaluating at end of Phase 3.

**D5 — Do you want the companion app in the loop?** The CHOICE surface today is reachable only from the remote HTTP path (`main.c:2322, 2872, 2949`) and choice taps feed the companion, not Gemini. Phase 4 assumes the *device* is authoritative. If the companion should stay in the loop, that changes 4.4's design.

**D6 — Dual-OTA.** `partitions_16MB.csv` has a single `factory` slot and **no otadata** — the header comment defers it to "Phase 4". Restoring dual-OTA costs another 4 MiB out of `emote_assets` or `storage`. If D4 goes procedural, `emote_assets` frees up and this becomes easy. Decide before the partition table calcifies.

---

## Timeline

| Phase | Effort | Ends with |
|---|---|---|
| 0 — Stop the bleeding | 0.5 d | Empty `git status`; a serial boot log |
| 1 — Consolidate | 1.5 d | One tree; live IMU + battery from the v5 image |
| 2 — Foundations | 4-5 d | **Photo: the orbital thinking spinner** |
| 3 — Cheap wins | 5 d | **Video: the 20-step attract reel** |
| 4 — Choice arcs | 7-8 d | **Video: agent asks, you tap an arc, agent confirms** |
| 5 — Power moods | 10 d | **Measured mA table + the mood ladder on video** |
| **Total** | **~6 weeks** | |

Phases 0-3 (≈2 weeks) get you a device that visually reads as the prototype. Phase 4 makes it interactive. Phase 5 makes it a product you can leave on a desk.

---

## The one rule that prevents restart #4

**No phase is complete without a photograph or video of the device.** Not a passing test suite — you already have 90 passing tests and a 3000-turn soak, and they did not prevent this situation. A picture. Attached to the commit. Every time.