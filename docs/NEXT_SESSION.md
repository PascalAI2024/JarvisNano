# Next Session Handoff

Last reconciled: **2026-09-02**.

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
| Horizontal swipe | Watch peek, 10 s |
| Top-edge down | Open controls |
| Centre up | Detail or controls close |
| Double tap | Jarvis Home |
| Glass hold | Physical privacy mute/unmute |
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
  all stable for 10 s. If it happens again, sample `/api/cockpit` and
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
3. **Frame rate on the ring:** cache the shell veil so ring screens match the
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
