# Device evidence

Tracked, durable proof that firmware actually ran on hardware — boot logs,
serial captures, screen snapshots, measured numbers.

**Why this directory exists:** `.build_logs/` is gitignored (`.gitignore:8`), so
every device artifact ever captured there was invisible to git, to CI, and to
anyone auditing the repo. That is the mechanical reason the project reached
2026-07-18 with no in-repo evidence that the v5 image had ever booted, despite
it booting fine. Evidence that isn't tracked isn't evidence.

**The rule** (see `docs/JARVISNANO_OS_PLAN.md`): no phase closes without a
photo, video, or log of the device, committed here. A green test suite does not
substitute — the project had 90 passing host tests and a 3,000-turn soak while
the display gap went unnoticed.

## Redaction — required before committing

Device-specific identifiers are banned from this repo (see `CLAUDE.md`). Strip
them from every capture: IPv4/IPv6 addresses, MAC addresses, SSIDs, bearer
tokens, API keys. Run `./scripts/check-secrets.sh` before committing.

Note that `check-secrets.sh` will also flag the `jr_memory` guard's own
detection patterns and test vectors (`components/jr_memory/`) — those are
intentional fixtures, not leaks.

## Contents

| File | What it proves |
|---|---|
| `20260719-hud-speaking.png` | The 48-bar reactive waveform live on the panel at amplitude 246 — spoke length is driven by real playback audio, not a loop. Measured cost: 1.7 fps (18.0 -> 16.3), down from a 4 fps hit before per-strip culling was added. |
| `20260719-hud-idle.png` | Idle breathing ring — the calm state. |
| `20260719-thinking-spinner.png` | **STATE-03 on glass — the first element of the JarvisNano OS design to render on the physical panel.** A real panel-submission-mirror frame captured mid-THINKING: the baked reactor face underneath, the procedural dim cyan track ring at r=150, and the orbiting comet with its white-hot head. Verified numerically, not just visually — 0 cyan pixels on the r=150 ring when not thinking, 274 while thinking, clustered at 141-142 deg (a localized comet, not a ring artifact). |
| `20260718-v5-boot-after-phase0.log` | The Phase 0 cleanup did not regress the device: identical boot path after removing the `jr_transport.c` placeholder, the `jr_vad_*` stub and `jr_dsp_resample_linear`, and after adding `hud_render.c` to the `jr_display` SRCS. |
| `20260718-v5-boot.log` | v5 boots clean on the Waveshare 1.75" AMOLED: PSRAM 8 MB, SD mounted, CST9217 touch IRQ-driven, CO5300 466×466 @ 24 fps with 12-row internal DMA strips, emote assets mounted (3,929,405 / 6,619,121 bytes), faces 0/1/2 rendering, Wi-Fi associated, ES7210 MIC1+MIC2+MIC3 enabled, audio adc+dac+aec up, Gemini Live TLS + handshake → Listening, VAD firing. Two open issues visible: `sta disconnected (reason=2/205)` retries before association, and `transport_ws: Error transport_poll_write(0)` at ~23 s. |
