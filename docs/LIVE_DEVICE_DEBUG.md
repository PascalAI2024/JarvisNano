# Live device debugging

Use this guide against the primary 1.75C firmware. The route authority is
[`PROTOCOL.md`](PROTOCOL.md); `/api/cockpit` and `/api/gemini/live` are the
combined runtime surfaces. Old overlay routes such as `/api/display/face`,
`/api/audio/level`, and `/api/ui/snapshot.ppm` are not v5 contracts.

A successful flash is not a successful boot. Prefer USB serial first, then use
Wi-Fi diagnostics after the device is visibly running.

## Connect

```bash
export JARVIS_DEVICE_HOST='<device-ip>'

python3 scripts/usb-monitor.py --seconds 10 --send status
python3 scripts/jarvisctl.py status
python3 scripts/jarvis-desk.py --host "$JARVIS_DEVICE_HOST" doctor
```

The firmware does not advertise mDNS. Keep host addresses and all captured
logs outside the repository.

## Core commands

```bash
# Timestamped health/report capture
python3 scripts/live-device.py report --host "$JARVIS_DEVICE_HOST"

# Local PPM/PNG submission mirror; never panel readback
python3 scripts/live-device.py screen --host "$JARVIS_DEVICE_HOST"
python3 scripts/jarvisctl.py screen

# Physical input receipt ring
python3 scripts/jarvisctl.py gestures 80

# Bounded incident log
python3 scripts/jarvisctl.py logs 131072

# Audio metadata and optional bounded diagnostic turn
python3 scripts/live-device.py audio --host "$JARVIS_DEVICE_HOST" --seconds 5
python3 scripts/live-device.py gemini-cycle \
  --host "$JARVIS_DEVICE_HOST" \
  --text 'Say one short sentence.' \
  --report

# Observe without mutating
python3 scripts/live-device.py watch --host "$JARVIS_DEVICE_HOST"
```

Reports and local display captures land under `.build_logs/live-device/`, which
is ignored. Redact addresses, SSIDs, MACs, tokens, endpoints, and device-specific
logs before promoting any artifact to `docs/evidence/`.

## What to inspect

### Voice and transport

`/api/gemini/live` should show a live session, capture enabled while listening,
server activity detection, and stable generations. During a clean natural turn:

- codec reads and transmitted frames increase;
- `tx_drops`, transport deaths, parse errors, and allocation failures do not;
- `audio_parts` increases during the answer;
- the device returns to Listening after completion or interruption;
- deliberate hold/flip privacy remains set across reconnect, operator, and
  repair actions.

`/api/diag/vadlog` is a bounded PSRAM CSV ring. It records mic RMS, floor, gate,
playback peak, phase, and decisions. The 1.75C has no SD mirror, so fetch the
ring before reboot when it is evidence.

### Audio proof boundary

`/api/audio/taps` and `/api/audio/tap.wav?source=...` prove samples at firmware
seams: raw mic, AEC-clean mic, echo reference, and playback write. They do not
prove that the speaker was audible. Close acoustic-output claims with a human
listen, microphone capture, or documented loopback.

### Display proof boundary

- `/api/display/snapshot.json` reports source, owner, freshness, and
  `panel_readback:false`.
- `/api/display/snapshot.ppm` and `.rgb565` are the same submitted software
  mirror in different encodings.
- A mirror proves renderer/control behavior, not CO5300 glass output.
- Use `/api/diag/panel-touch?action=start` and the randomized physical challenge
  when a human must bind software state to the panel and touch hardware.

Healthy Jarvis/Watch cadence is roughly 15–16 FPS. The controls surface remains
a known optimization target; treat `flush_errors > 0`, a stalled frame ID, or a
shrinking internal block as a fault, not cosmetic noise.

### Input proof

Keep `jarvisctl gestures 80` visible while exercising:

1. PWR short → `LISTENING`, never privacy mute.
2. PWR long → battery/charging status.
3. BOOT short after boot → controls open/close.
4. Left-edge vertical from every space → persisted volume ±5.
5. Right-edge vertical from every space → persisted brightness ±5.
6. Centre vertical swipe → Jarvis/Watch/Power/Desk/Tools ring (wraps); horizontal → Watch peek.
7. Top-edge down → controls; double tap → Jarvis Home.
8. Glass hold and flip → physical privacy; remote resume must refuse to clear it.

HTTP input injection proves queue routing only. It cannot prove PWR, BOOT,
physical touch, motion orientation, audio, or panel output.

## Release-shaped acceptance circuit

After a clean build/flash or OTA:

```bash
python3 scripts/jarvisctl.py status
python3 scripts/jarvis-desk.py --host "$JARVIS_DEVICE_HOST" doctor
python3 scripts/live-device.py screen --host "$JARVIS_DEVICE_HOST"
python3 scripts/live-device.py gemini-cycle \
  --host "$JARVIS_DEVICE_HOST" \
  --text 'Use current_time, then answer in one sentence.' \
  --report
python3 scripts/jarvisctl.py gestures 80
python3 scripts/jarvisctl.py logs 131072
```

Then perform one physical conversation: speak, interrupt mid-reply, accept a
choice arc, navigate every space, use both edge levels, open controls with BOOT,
enter/leave hold privacy, flip privacy, and press PWR. Close only what the
captured evidence actually observed.

## Security boundary

The root page and coarse hardware counters are intended for a trusted
development LAN. Cockpit/session detail, logs, audio taps, Agent Link, and
display pixels require the host-bound pairing token. Mutating control routes
also require `X-JarvisNano-Control: 1`. Plain HTTP does not encrypt either
header; never use these surfaces on an untrusted network.
