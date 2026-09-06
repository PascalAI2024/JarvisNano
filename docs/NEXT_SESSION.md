# Next Session Handoff

Last reconciled: **2026-09-05**.

The live target is the **Waveshare ESP32-S3-Touch-AMOLED-1.75C** with 32 MB
flash. The active image is plain ESP-IDF v5 rooted at `main/` and
`components/jr_*`.

## Start here

```bash
./scripts/build-v5.sh
./scripts/flash-v5.sh

export JARVIS_DEVICE_HOST='<device-ip>'
# Blank host/device only: hold BOOT 1.5–5 s, then run:
python3 scripts/jarvis-desk.py --host "$JARVIS_DEVICE_HOST" pair
python3 scripts/jarvisctl.py status
python3 scripts/jarvis-desk.py --host "$JARVIS_DEVICE_HOST" doctor
python3 scripts/jarvisctl.py gestures 80
python3 scripts/jarvisctl.py logs 131072
python3 scripts/jarvisctl.py screen
```

USB-Serial-JTAG is single-owner. Stop an active monitor before flashing. A
charge-only cable can power the board without creating `/dev/cu.usbmodem*`;
Wi-Fi OTA may still pass preflight when USB power is present.

## Current interaction grammar

| Input | Action |
|---|---|
| PWR short | Listen/wake only; never mute |
| PWR hold | Power off completely; hold PWR 1 s to start (2026-09-01) |
| BOOT short after boot | Open/close controls |
| BOOT hold 1.5–5 s after boot | Open a visible 60-second pairing claim window |
| BOOT held during reset | Enter ROM downloader |
| Left-edge vertical | Volume +/− 5 globally |
| Right-edge vertical | Brightness +/− 5 globally |
| Centre vertical swipe | The ring: Jarvis ↔ Watch ↔ Weather ↔ Status ↔ (Desk, only while an agent/claim/lease is live) ↔ Activity (wraps) |
| Horizontal swipe | Watch peek, 10 s; on WATCH itself: RIGHT = next watch style, LEFT = previous (JARVIS, DIVER, DRESS, PILOT, MINIMAL, FUTURE), kept in NVS |
| Top-edge down | Open controls |
| Centre up | Detail or controls close |
| Double tap | Jarvis Home |
| Glass hold (inner disc, r < 168) | Physical privacy mute/unmute; a hold that starts on the rim is ignored (2026-09-05) |
| Sustained face-down / face-up | Enter flip privacy / clear only a flip-origin mute |

The controls surface is the on-device legend: `L VOL`, `R LIGHT`,
`PWR LISTEN`, `BOOT CLOSE`, and centre MUTE/LISTEN. Do not reintroduce the
failed continuous circular-rotation recognizer; the CST9217’s reliable signal
is the classified edge-origin swipe.

## Current runtime truth

- Gemini Live is direct from the device over Wi-Fi.
- Capture/playback share a native 24 kHz I²S clock; uplink is AEC-cleaned and
  downsampled to 16 kHz.
- Server VAD owns turn boundaries. Local VAD is observability/pacing; local
  barge is not the primary path.
- Display uses one compositor, 12-row internal-DMA strips, baked EAF faces, and
  a sparse listening halo. Normal Jarvis/Watch cadence is roughly 15–16 FPS.
- PWR and USB-powered mood policy keep the desk assistant listening; deliberate
  glass/flip privacy still wins.
- OTA uses two 4 MB slots and validates voice/network/tools/HTTP/wake/display
  before marking a new image valid.
- JarvisMCP server policy is live; byte-budgeted device catalog projection and
  cursor semantics remain incomplete.

## What changed on 2026-09-05 (wave N13)

The owner asked for polish and enhancement over Wi-Fi. A read-only baseline
came first (`/api/cockpit`, `/api/device/health`, `/api/diag/tasks`,
`/api/logs`, the panel mirror) and found seven things the 2026-09-02 handoff
did not know. Fixes first, then the features; `PLAN.md` wave N13 has the
rows and gates, `docs/evidence/20260905-*` the proof.

- **A privacy long-press cost every reply 300 ms.** The mute sweep went
  through the reply-underrun accounting (`docs/evidence/20260905-earcon-preroll.log`):
  four "holes mid-reply", `replies 1`, pre-roll 600 → 900 with no Gemini
  reply ever played. N10.11's "stepped 600→900 on its own after a hole" was
  this. Now earcon samples are counted at `pb_enqueue` and the feeder books
  nothing while they drain (+80 ms tail); tones render whole; the walk is a
  pure `jr_dsp` stepper (`host/test_jitter.c`, eight tests; the mutation
  fails three). **Proven by hand on the glass:** mute + unmute sweeps →
  `replies 0, underruns 0`; one spoken turn → `replies 1, underruns 0`
  (`docs/evidence/20260905-hand-test-earcon-rail.log`); twelve spoken turns
  during the fps runs read `replies 12, prerolls 12, underruns 0`. Reference page:
  `docs/reference/playback-jitter.md`.
- **`jr_pb_feed` had 524 bytes of stack.** The 1536-byte chunk is a static
  now: 3196 bytes free on the same 4096 stack (`/api/diag/tasks`). The
  largest internal block read 31744 after (32768 before) — the static costs
  1 KB of internal RAM, deliberately, for a feeder that cannot overflow.
- **The brightness fade wrote fifteen log lines.** `co5300_spi` is at WARN
  from boot; 97 of 271 lines in the 16 KB tail were a fade. After: zero.
- **A slow finger on the volume rail muted the device** (log:
  `long press … dy 42 … 870 ms` → `gesture: long-press mute`). The hold
  slop (48) exceeded the swipe minimum (42), so 43–48 px of drift was a
  hold that fired mid-press before the swipe classifier could run, and the
  privacy consumer read no coordinates. Now the three slops agree at 42, a
  hold that starts on the rim (r ≥ 168) is ignored with the neutral ack,
  `gesture-doctor` asserts `hold_slop <= swipe_min`, `INPUT_MAP.md` says
  "inner disc only". Hand test: three rail drags moved the volume, the
  centre holds toggled privacy, nothing else did.
- **The FUTURE weather cell cut a word** (`75* LIGHT DRIZZ`,
  `docs/evidence/20260905-future-cut-word.png`). Found on the way: the
  12-glyph `condition` cap had already made it `LIGHT DRIZZL` before the
  cell saw it. Cap 24; one fitter (`wx_cond_fit`) serves the cell and the
  WEATHER headline: drop the qualifier, else cut at a word with the "." mark.
  Shell test with a mutation check (the old clip fails 7 checks).
- **WATCH fps levers** (N12.6): `render_us`/`render_frames`/`render_frame_us`
  on `/api/display`; per-strip work hoisted to per-frame (words, angles,
  hand specs, tick pre-check — output identical, strip-vs-whole memcmp
  pinned); shadows only at the 24 fps cadence; four AA sample lines for
  hands wider than 6 px, eight for hairlines. The outline fold was tried,
  measured null (±5 %), cost 932 B of internal RAM, and was reverted — do
  not retry it. Numbers on the panel: see the table below.
- **The glance asks for the sun and the hours.** `weather_glance` also
  fetches Open-Meteo sunrise/sunset and 36 hourly rain probabilities in its
  own `try` (226 bytes measured); an AWAKE/AMBIENT device refreshes every
  30 min from any screen (`weather: fetch submitted (half hour)`), never at
  rest. `/api/cockpit` carries `sun_rise_min`/`sun_set_min`.
- **Rain in the next three hours, once per front** (`jr_core/glance.c`,
  `jr_rain_warning_step`, six host tests on the real day's answer): a
  `RAIN IN 2H` caption when muted, spoken with an open session, always a
  `RAIN` ACTIVITY row; re-arms only under 30 %.
- **The day arc.** FUTURE and PILOT draw the daylight as a thin gold band
  at r204–208 on a 24-hour scale (noon at 12) with a dot for now;
  `jr_display_sun_set` at 1 Hz beside the clock; shell test pins the angles
  for 07:02/19:35, "no sun → nothing", "DRESS → nothing", "inverted → nothing".
- **The morning glance speaks.** First lift after a rest (or a deep-sleep
  wake by lift/touch, counted once after the first weather fetch had its
  chance), once per day in 05:00–11:00 (NVS `brief_yday` — the morning lift
  is usually a fresh boot): one text turn through `handle_say` naming the
  date, the weather, rain within 12 h, delegated tasks finished since the
  last briefing, and the battery under 30 % off USB. Muted: a
  `GOOD MORNING 75* DRIZZLE 2 DONE` caption. Pure `jr_briefing_*` with host
  tests. `POST /api/debug/briefing` (dev-gated) delivers it now at any hour
  without touching the latch, for the bench. **Proven from the bench at
  23:52:** muted it captioned `GOOD MORNING 76* OVERCAST`; unmuted it spoke
  the composed turn (`briefing: spoken, forced … It is Saturday 5 September.
  Fort Lauderdale is 76 degrees and OVERCAST, high 91, low 76 …`). The
  first lift of a morning is still unseen — expect `briefing: spoken` or
  `briefing: caption, muted` in the log.
- **A chime when a delegated task lands.** `jr_audio_play_chime` (raised-
  cosine notes) plays E5–G5–B5 in `board_announce()` before the caption or
  the spoken line, half volume at WHISPER, never over a reply. **Proven
  23:50:** an item created from the desk was claimed by the poll, researched
  by the gateway (two polls hit the 30 s cap with `http_error 500` on the
  way; the recovery path completed it), then `chime: done` and
  `board: done Find the CO5300 …` with the `DONE:` caption on the muted
  glass (`docs/evidence/20260905-hand-test-earcon-rail.log`).
- **The scripts run on Windows** (PIL before `sips`, `JARVIS_PAIRING_TOKEN`
  env fallback, UTF-8 stdout, `tempfile`, no `termios` at import, a clean
  no-host message) and **one host-tests entry**: `./scripts/host-tests.sh`
  runs all five suites with positive counts and takes a Docker lane
  (`jarvisnano-hosttests`) when there is no `cc`. Git Bash rewrites
  `/project` to `C:/Program Files/Git/project` before Docker sees it;
  `MSYS_NO_PATHCONV=1` in `build-v5.sh` stops that
  (`docs/reference/build-toolchain.md`).

**WATCH render cost per frame, muted at the 160 MHz rest gear
(`gfx_render` run-time delta ÷ frames, `/api/diag/tasks`), before → after:**

| Style | 2026-09-02 doc | 2026-09-05 before | after |
|---|---|---|---|
| JARVIS | 69 | 65 | 67 |
| DRESS | 89 | 59 | 55 |
| DIVER | 108 | 79 | 71 |
| PILOT | 112 | 87 | 88 |
| FUTURE | 127 | 103 | 100 |
| MINIMAL | 109 | 110 | 108 |

**And at the live gear, unmuted, a session open (240 MHz, AWAKE, Listening)
— the case the ≥ 17 fps gate names** (`docs/evidence/20260905-watch-fps.md`):

| Style | actual fps | frames/4 s | `gfx_render` ms/frame | overlay `render_frame_us` |
|---|---|---|---|---|
| DRESS | 12 | 32.9 | 51 | 16.8 ms |
| JARVIS | 10 | 45.5 | 54 | 23.3 ms |
| DIVER | 10 | 33.5 | 66 | 29.4 ms |
| PILOT | 11 | 33.2 | 74 | 34.8 ms |
| FUTURE | 7 | 33.5 | 79 | 33.6 ms |
| MINIMAL | 8 | 31.0 | 92 | 60.8 ms |

**The gate is not met, and the levers were the wrong ones** — and the
scout that followed found why (PLAN wave N14): the frame is overlay +
engine CPU (bg memset, block decode, palette expand) + 21.7 ms of QSPI DMA
the engine waits for before filling the next strip + up to 10 ms of forced
tick delay, all serialized; and every baked dial's anim timer ran at 8 fps,
which is the only thing that marks the glass dirty, so the four dials could
never flush more than 8 frames a second. The timer is 24 now (N14.1, in the
last image of the evening); the dial-off-the-engine blit and the deferred DMA
wait are N14.2–N14.4 with the arithmetic. The overlay
(the hands, cells and arc) is 17–61 ms of a 51–92 ms frame; the rest —
31 ms on JARVIS, which has no dial at all — is the engine's own work (the
face/dial decode per strip and the QSPI flush), and it barely moves between
160 and 240 MHz, so it is not CPU-bound. With a live session the render task
also shares the chip with AEC, WakeNet and the uplink. What would move the
gate: decode a resident dial ONCE into a raw RGB565 frame in PSRAM
(434 KB each; four fit) and blit it per strip instead of re-decoding it
39 times a frame, and let a static dial skip the engine's face path
entirely; then MINIMAL's 61 ms procedural disc is the last big overlay
cost. The 2026-09-02 estimate ("≈ 12–17 fps at 240") scaled the 160 MHz
number by 1.5 and was wrong for the same reason.

## What changed on 2026-09-02

- **Jarvis delegates.** `delegate_task` → `coordination.createWorkItem` on
  project `jarvisnano-desk` (config `project_id`), `delegated_tasks` lists it,
  and `board_poll` (90 s, awake, Wi-Fi, not DREAM) announces a completed or
  blocked item once: spoken with an open unmuted session, `DONE: <title>`
  caption when muted, always an ACTIVITY `TASK` row. Proven end to end from
  a desk. **The worker is the gateway itself:** the same poll claims one item
  and settles it in the call — repo goals to the managed Pi sandbox worker
  (branch delivery, allowlisted repos only), everything else researched with
  the owner's notes as context and filed in the company brain. No devbox, no
  host, no keys beyond the one already in NVS. `tools/board-worker/worker.py`
  is a reference for jobs the gateway cannot do. An SSH key pasted into chat
  on 2026-09-02 should be rotated — it was never installed.
- **Six watches, second cut.** Dial = baked art through the face pipeline
  (`JR_FACE_DIAL_*`, requested by `watch_dial_face()` on WATCH with the
  clock on; a missing clip falls back to IDLE underneath and the watch
  clears its disc and draws the black stand-in). Hands = `hud_overlay_watch`
  in `hud_render.c`: anti-aliased scanline polygons (16 sub-rows, half-pixel
  tips, so no combing — the probe sweeps 13 hands × 360°), a two-tone bevel
  that follows a fixed top-left light, hairline outline, soft shadow, lume,
  hubs ≥ 12 px over the art's centre hole. Seconds sweep from
  `s_clock_phase_ms`, latched once per frame. Text (DIVER's day, FUTURE's
  four cells) is drawn in `watch_cells()` with the shell's glyphs; the
  geometry table is `HUD_WATCH_*` in `hud_render.h`, on the art's measured
  numbers. Three style bits (17–19) in the nav word; `jr_display_clock_set_date`
  carries the day. The four dial clips overflowed the SPIFFS image at first
  staging — the art lane's call. **Found from the captures and fixed after
  both lanes landed:** the ring's backdrop veil halved every baked dial
  (`watch_art_is_content`, `jr_display.c`; the shell test now demands the
  art's exact pixel value survive on a settled WATCH and be veiled under a
  sheet). **Not met: the ≥ 17 fps awake gate.**
  Measured at 160 MHz per frame: JARVIS 69 ms, DRESS 89, DIVER 108, MINIMAL
  109, PILOT 112, FUTURE 127 (≈ 12–17 fps at 240 MHz); numbers and the
  remaining levers in `docs/GLASS_DESIGN.md` "As shipped — the firmware".
  Also seen once: an unexplained reboot right after the first walk of the
  six styles on the first second-cut image (before the coverage row moved
  off the render stack); not reproduced on the next three images.
- **Muted is a watch.** Privacy mute feeds the ladder a `quiet` input:
  five seconds still and the glass is the WHISPER watch (brightness 22,
  6 fps, 160 MHz) with USB no longer holding it awake; touch, pickup, a live
  phase and face-down keep their meanings. Verified on the device: see the
  evidence note in `docs/reference/power-modes.md`.
- **Touch Y was mirrored.** Owner: "the edge gesture for volume and
  brightness are backwards, up should make volume go up." Measured with
  `/api/touch` during a real upward stroke on the left edge: start y 131,
  end y 321, dy +190 — the controller's Y grows toward the top of the
  glass. `TOUCH_MIRROR_Y` in `components/jr_hal/src/input_touch.c` sets
  `esp_lcd_touch_set_mirror_y`, a software flip (`y' = 466 − y`) because the
  CST9217 driver has no hardware one. Every vertical gesture the docs
  describe was physically inverted before this and nobody noticed, because
  the ring's direction is arbitrary and the shade also opens on BOOT.
  Verified by finger on the flashed image: two upward strokes on the left
  edge read `dy −193` and `dy −199` and the volume climbed 85 → 90 → 95;
  sideways strokes on WATCH read RIGHT and walked the styles. The shade's
  top edge is the physical top again — untested by finger, same axis.
- **Weather retries.** A failed `weather_glance` is retried every two
  minutes from any screen (never in DREAM, never off Wi-Fi) until it
  succeeds; a good fetch still refreshes only on the WEATHER screen after
  ten minutes. The failure that prompted it was Open-Meteo answering the
  gateway with a 200 and a text body (`allEndpointsUnavailable`) while
  answering a laptop normally; the gateway's `weather` service surfaces that
  as a JSON parse error and `execute_tool weather` burned the 30 s budget on
  two 15 s timeouts. Nothing on the device could have fixed that morning.
- **Three quiet faces** (rest, muted, linking) from the same generator, 0.93 MB;
  the art partition ships separately: `jarvisctl art` → `POST /api/ota/assets`
  (~150 s, refused in app probation). A firmware that names a clip the
  partition lacks shows the parent face once and logs it.
- **The display has a cadence** (24/12/6/3 fps per ladder rung, touch restores
  24). Unmeasured on the cell: on USB the ladder holds AWAKE.
- **`main/` is four files** (`app.h`, `main.c`, `http_routes.c`, `power.c`,
  `device_tools.c`); 140 functions before and after.
- **Muted under a live session** now shows the gold slit too (LISTENING was
  the open reactor under a gold ring).
- **One probation rollback, not reproduced.** The last OTA of the evening
  stayed `pending-verify` past 100 s and the 120 s deadline rolled it back
  (`last_invalid: ota_0`); the identical image re-flashed confirmed at 58 s.
  The criteria (`ota_confirm_running_image_if_healthy`, `main/http_routes.c`):
  voice heartbeat < 2 s, Wi-Fi up, tools worker ready, HTTP up, WakeNet up,
  display ready with no new flush errors, ≥ 12 fps and progress within 1 s,
  all stable for 10 s. **Since the muted watch, the fps floor follows the
  requested cadence** (`display_fps_floor`, `main/http_routes.c`): a device
  boots muted and is a 6 fps watch in five seconds, and the first image with
  the watch would have rolled back for it — the floor is 2 fps whenever the
  ladder asked for less than 12, with flush progress still required. If it
  happens again, sample `/api/cockpit` and
  `/api/device/health` every 10 s from boot to 130 s and read which column
  dips; the sampler is a 15-line script and the columns are `network`,
  `tools.worker_ready`, `voice.phase`, `display.flush_errors`,
  `display.actual_fps`. With the cadence knob live, a ladder that ever left
  AWAKE during probation would put the fps under 12 and fail it by design;
  on USB it cannot.

## What changed on 2026-09-01

- **Voice is smooth.** The speaker now runs behind the network: 600 ms pre-roll (was 1000 for an hour on 2026-09-01; the owner chose latency)
  before a reply's first word, a 1500 ms lead rebuilt after any hole, and a
  96-deep WebSocket queue. Cause, measured: Gemini paces native audio near real
  time with 0.8–1.34 s stalls mid-sentence. Counters live at
  `/api/device/health` (`playback`, `rx`); reset with
  `POST /api/debug/audio-stats?reset=1`; tune with
  `/api/debug/gain?preroll=&refill=`. Probe: reset → `/api/debug/say` → poll →
  read. See `docs/reference/gemini-live-api-v5.md` §7.
- **SETTINGS is gone.** The update ring draws on every screen; UPDATE/SLOT rows
  live on the POWER sheet; volume/brightness readouts are on the shade.
- **TOOLS shows all eight tools**, the DESK sheet heads with the task, the orbit
  rail stays in r185–194, one battery red, panic-home clears everything, the
  shade survives rapid volume taps, the peek caption leaves with the peek.
- **The useless screens are gone, later the same day.** TOOLS is replaced by
  ACTIVITY (the last three things Jarvis did, newest first, or "NOTHING
  YET"); WEATHER is new (a 40–100 °F gauge whose low-to-high span is the day
  and whose mark is the temperature now, honest about its age); POWER became
  STATUS (battery arc, "83% CHARGING"-style headline, a nine-row sheet with
  link, mic and uptime); DESK is on the ring only while an agent, claim or
  lease is live, and a DESK that goes dark under you moves you to ACTIVITY.
  `docs/GLASS_DESIGN.md` §B has the ring as shipped; the host suite pins the
  DESK skip, the strand, the weather mark angle, the stale dim and the
  activity order (mutation-checked).
- **S21 refuted:** a lease never froze the glass; synthetic swipes were being
  refused under a lease. They now walk the ring; taps/holds stay physical-only.
- **Flashing a live device:** esptool could not sync over USB-JTAG while the
  firmware ran. POST `build/jarvisrobot_v5.bin` to `/api/ota/upload` with
  `X-JarvisNano-Control: 1` (no token while `JR_DEV_OPEN_DIAGNOSTICS` is 1);
  ~45 s, back in ~5 s. `jarvisctl ota` no longer refuses without a keychain
  token.

- **Later still: STATUS is the device, and the device sleeps.** STATUS was
  rebuilt after the owner called the first cut a junk screen: LINK/TOOLS
  lamps, the battery arc with the percentage inside, Wi-Fi bars + dBm, a
  headline that says the worst thing or the uptime, and a nine-row sheet
  (battery, power, Wi-Fi, IP, link, tools, chip temperature, radio mode,
  update). Deep sleep ten minutes into DREAM on battery; wake on lift (QMI8658
  WoM on INT1), touch (GPIO11) or a 4 h timer; `GET/POST /api/debug/sleep`.
  A deaf-session watchdog reconnects after two unanswered utterances. The
  weather refreshes while idle (`JR_TOOLS_SESSION_ANY`). Jarvis names its
  tools when asked. `docs/reference/power-modes.md` has the recipe and the
  two gotchas (probation rollback, no `CmdDone`).

- **Late evening: the assistant, the mic, and the battery.** Persona rewritten
  as the owner's personal AI (it declined out-of-"domestic" topics); tools
  widened to notes, calendar, work board (destructive names refused on the
  device); a spoken "remember" needs no tap; recall projected. Privacy now
  gates the microphone frames themselves (they were flowing under a muted
  ring). The first unanswered utterance gets a nudge, the second reconnects.
  Pre-roll is adaptive 600–1500 ms. CPU gears 240/160 by mood
  (`cpu_gear_set`, `CONFIG_PM_ENABLE`, no DFS), a four-times-faster ladder
  below 20 % on the cell, run-time counters on `/api/diag/tasks`. PWR hold
  now powers off completely through the PMIC (`jr_power_off`): the off half
  is proven from the desk, the on half (a one-second hold of PWR, or a USB
  replug as the guaranteed way back) had not been seen when this was written.
  Also not yet seen in the wild: the lift wake and the 160 gear engaging on
  their own.

## Current blockers

1. **Lift wake by hand:** off USB, face-down ten minutes, lift; expect
   `wake: lift` from `/api/debug/sleep`. Lower `SLEEP_WOM_MG` if it reads
   `timer`.
2. **Deaf-session watchdog in the wild:** watch for `utterance unanswered` /
   `session is deaf` in the log; raise `UTT_DEAF_COUNT` if ambient chatter
   trips it.
3. **Frame rate on the watch:** N14.2 (dial decoded once, blitted) and N14.3
   (DMA wait one strip later) are the two that move it; the veil lever below
   is spent on WATCH.
3b. **Frame rate on the ring:** cache the shell veil so ring screens match the
   face's 19 fps (N9.10).
4. **Two violets:** the update ring in probation and the companion rim share a
   hue (N9.11).
5. **Release security:** `JR_DEV_OPEN_DIAGNOSTICS` back to 0, signed images,
   authenticated encrypted upload, at-rest credential protection, exact
   third-party notices.

The actionable order and acceptance criteria are in [`../PLAN.md`](../PLAN.md).

## Safety invariants

- Synthetic input cannot clear privacy, answer asks, approve consent, or escape
  operator ownership.
- Remote resume never clears physical hold/flip privacy.
- Screenshots are submitted software buffers, not panel readback.
- PCM taps prove codec-write data, not audible speaker output.
- Never print or commit keys, tokens, endpoints, SSIDs, addresses, NVS images,
  or device-specific logs.
- Secure Boot/eFuse work is physically attended and separate from ordinary OTA.

## Do not repeat

- Do not build `firmware/` or `esp-claw/` when validating v5.
- Do not add LVGL or a second renderer beside the live compositor.
- Do not put display DMA buffers in PSRAM.
- Do not treat `transport_poll_write(0)` as a socket death.
- Do not use raw tool result count as a byte-budget guarantee.
- Do not turn a normal tap or PWR press into an accidental privacy toggle.
- Do not claim physical proof from an HTTP response alone.
