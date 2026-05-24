# Live Device Debug Framework

Use this when the Waveshare board is on Wi-Fi and you need evidence from the
device, not vibes.

Default live device:

```bash
scripts/live-device.py status
```

Override the target:

```bash
export JARVIS_DEVICE_HOST=<device-host-or-ip>
scripts/live-device.py status --host $JARVIS_DEVICE_HOST
```

Write a timestamped diagnostic report:

```bash
scripts/live-device.py report --host $JARVIS_DEVICE_HOST
```

Reports and local display captures land in:

```text
.build_logs/live-device/
```

## Core Commands

Run a Gemini start/text/stop cycle:

```bash
scripts/live-device.py gemini-cycle --host $JARVIS_DEVICE_HOST --text "Say one short sentence." --report
```

Capture the emote display mirror locally and on SD:

```bash
scripts/live-device.py screen --host $JARVIS_DEVICE_HOST --save-sd
```

Force a face state and optionally capture it:

```bash
scripts/live-device.py face --host $JARVIS_DEVICE_HOST listen --amp 700 --screen --save-sd-screen
scripts/live-device.py face --host $JARVIS_DEVICE_HOST idle
```

Sample the mic and optionally exercise the speaker path through Gemini:

```bash
scripts/live-device.py audio --host $JARVIS_DEVICE_HOST --seconds 5
scripts/live-device.py audio --host $JARVIS_DEVICE_HOST --speaker --text "Say one short sentence." --report
```

Watch the board while tapping or testing animations:

```bash
scripts/live-device.py watch --host $JARVIS_DEVICE_HOST
```

Filter logs for the current problem lanes:

```bash
scripts/live-device.py logs --host $JARVIS_DEVICE_HOST --grep touch,rwave,gemini,audio,ws,flush
```

Restart over HTTP and wait for Wi-Fi to return:

```bash
scripts/live-device.py restart --host $JARVIS_DEVICE_HOST --wait 60
```

## What It Checks

- `/api/health`, `/api/status`
- `/api/audio/level`
- `/api/gemini/live`
- `/api/display/face`
- `/api/display/snapshot.json`
- `/api/logs?tail=N`
- Gemini start/text/stop state transitions
- Gemini push-to-talk state transitions and counters:
  - `activity_open`
  - `tx_frames_sent`
  - `tx_codec_reads`
  - `tx_raw_reads`
  - `tx_send_failures`
  - `tx_read_failures`
- Known failure signatures:
  - `Gateway stop timed out`
  - `WS connect timeout`
  - `WS start returned ESP_FAIL`
  - `setupComplete timeout`
  - `Current mode playback conflict`
  - `Tap: local scene`
  - `Touch controller must be initialized`
  - `rwave_task skip`
  - `display arbiter NOT owned`
  - `display flush completion timeout`
  - `display flush sync disabled`
  - `i2s_channel_disable`

## Acceptance Test

After flashing a build with the display, touch-routing, and Gemini fixes:

```bash
scripts/live-device.py status --host $JARVIS_DEVICE_HOST
scripts/live-device.py face --host $JARVIS_DEVICE_HOST listen --amp 700
sleep 25
curl -s http://$JARVIS_DEVICE_HOST/api/display/face
curl -s http://$JARVIS_DEVICE_HOST/api/display/snapshot.json
scripts/live-device.py screen --host $JARVIS_DEVICE_HOST --save-sd
scripts/live-device.py audio --host $JARVIS_DEVICE_HOST --speaker --text "Say one short sentence." --report
```

Expected:

- `driver_ticks` and `frame_id` keep increasing after 25+ seconds.
- Screenshot PNG has a black background, not the old pink/black-key artefact.
- Screenshot capture does not freeze the reactive face afterward.
- Mic samples return `valid=true`.
- Gemini reaches `connected=true` / `listening=true`, text POST returns `200`, and `audio_parts > 0`.
- After stop, `/api/gemini/live` returns `state=IDLE`.

Voice push-to-talk acceptance test:

```bash
scripts/live-device.py gemini-cycle --host $JARVIS_DEVICE_HOST --text "Say audio check passed." --turn-wait 8
```

Then start a live voice session, speak near the board, and end input by tapping
again or by posting `{"action":"end_input"}` to `/api/gemini/live`.

Expected:

- Listening starts with `activity_open=true`.
- `tx_codec_reads` and `tx_frames_sent` increase while speaking.
- `tx_raw_reads=0`, unless the codec path genuinely failed and raw fallback is
  being diagnosed.
- Ending input logs `Audio activity end requested`.
- The board enters `SPEAKING`, `audio_parts > 0`, then returns to `LISTENING`
  with `turn_complete=1` or `generation_complete=1`.
- `drops=0`, `tx_send_failures=0`, and no live `Current mode playback conflict`.

Then physically short-tap the display while watching logs:

```bash
scripts/live-device.py logs --host $JARVIS_DEVICE_HOST --grep touch,Tap,gemini --lines 120
```

Expected:

- Short tap logs `Tap: toggling Gemini Live on` when idle.
- A second short tap while listening logs `Tap: ending Gemini Live input`.
- Short tap does not log `Tap: local scene`.
- Long press can still start the local hardware demo.

## Current Limitations

- The screen capture is a firmware mirror of the emote flush path, not panel
  readback. It is valid for reactive-face debugging, but it will not capture
  arbitrary Lua display drawing unless that drawing uses the same mirror path.
- The tooling cannot physically tap the capacitive glass. A person must tap
  while `logs` or `watch` is running, or a future diagnostic route must inject a
  touch event.
- Speaker verification is indirect. `audio --speaker` proves Gemini returned
  audio frames and the firmware drove the playback path; it does not prove
  acoustic output unless someone hears it or a loopback test is added.
- Full PPM capture is intentionally slower than `/api/display/snapshot.json`.
  Use JSON for animation heartbeat checks and PPM/PNG only when pixels matter.
